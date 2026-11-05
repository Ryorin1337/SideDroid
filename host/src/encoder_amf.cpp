// AMD VCN/VCE through AMF, loaded at runtime from the driver, so that the host still starts
// on machines without an AMD GPU.
//
// No AMD GPU was available when this was written: it has been compiled, not run. So every
// step below follows AMD's documentation (AMF SDK v1.5.3), and where it says so, the source
// is named. docs/encoders.md has the same list with quotes. What is left to chance is covered
// by the session, which moves on to the next encoder if this one keeps failing.
//   [Ref]  amf/doc/AMF_API_Reference.md
//   [AVC]  amf/doc/AMF_Video_Encode_API.md
//   [HEVC] amf/doc/AMF_Video_Encode_HEVC_API.md
//   [SE]   amf/public/samples/CPPSamples/SimpleEncoder/SimpleEncoder.cpp
#include <windows.h>

#include <d3d11.h>

#include "amf/components/VideoEncoderHEVC.h"
#include "amf/components/VideoEncoderVCE.h"
#include "amf/core/Factory.h"
#include "encoder.h"
#include "log.h"
#include "sidedroid/wire.h"

namespace sd {

namespace {

constexpr UINT kVendorAmd = 0x1002;
constexpr ULONGLONG kOutputTimeoutMs = 500;
constexpr amf_int64 kQueryTimeoutMs = 50;

class AmfEncoder final : public Encoder {
public:
    ~AmfEncoder() override {
        // [SE] "cleanup in this order": the encoder, then the context, which goes last.
        encoder_ = nullptr;
        if (component_) component_->Terminate();
        component_ = nullptr;
        if (context_) context_->Terminate();
        context_ = nullptr;
        if (dll_) FreeLibrary(dll_);
    }

    bool init(const EncoderConfig& cfg) {
        cfg_ = cfg;
        hevc_ = cfg.codec == kCodecHevc;

        // [Ref 2.2.1] The runtime comes with the driver: load it, ask its version, and start it
        // with that. The name is AMF_DLL_NAME in core/Factory.h.
        dll_ = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll_) return false;
        auto queryVersion = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(dll_, AMF_QUERY_VERSION_FUNCTION_NAME));
        auto initFactory = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll_, AMF_INIT_FUNCTION_NAME));
        if (!queryVersion || !initFactory) return false;
        amf_uint64 version = 0;
        if (queryVersion(&version) != AMF_OK) return false;
        amf::AMFFactory* factory = nullptr;
        if (initFactory(version, &factory) != AMF_OK || !factory) return false;

        // [Ref AMFContext] A context bound to the D3D11 device that the pictures live on.
        if (factory->CreateContext(&context_) != AMF_OK) return false;
        if (context_->InitDX11(cfg.device) != AMF_OK) {
            logf("AMF: InitDX11 failed");
            return false;
        }

        // [HEVC 2] AMFFactory::CreateComponent with the ids in the two component headers.
        // Not every GPU has every codec, and failing here sends the session to the next codec.
        const wchar_t* id = hevc_ ? AMFVideoEncoder_HEVC : AMFVideoEncoderVCE_AVC;
        if (factory->CreateComponent(context_, id, &component_) != AMF_OK) return false;
        encoder_ = component_;

        if (!configure()) return false;

        // [Ref AMFComponent] Init takes the format and size. NV12 is what the converter makes.
        if (component_->Init(amf::AMF_SURFACE_NV12, cfg.width, cfg.height) != AMF_OK) {
            logf("AMF: Init failed for %dx%d %s", cfg.width, cfg.height, hevc_ ? "HEVC" : "H.264");
            return false;
        }
        return true;
    }

    bool encode(ID3D11Texture2D* nv12, bool forceKeyframe, const Output& out) override {
        // [Ref AMFContext] Wraps our texture without copying it. The encoder keeps its own
        // reference until it is done, and we wait for the output before the texture changes.
        amf::AMFSurfacePtr surface;
        if (context_->CreateSurfaceFromDX11Native(nv12, &surface, nullptr) != AMF_OK) return false;

        // Some of the first frame's handling is only promised on a forced one: no periodic IDR
        // is configured, and the decoder needs the headers right away.
        if (forceKeyframe || firstFrame_) setKeyframe(surface);
        firstFrame_ = false;

        // [SE] AMF_INPUT_FULL and AMF_DECODER_NO_FREE_SURFACES mean wait and submit again.
        // AMF_NEED_MORE_INPUT means it was taken, and output is not ready yet.
        const ULONGLONG deadline = GetTickCount64() + kOutputTimeoutMs;
        for (;;) {
            const AMF_RESULT r = component_->SubmitInput(surface);
            if (r == AMF_OK || r == AMF_NEED_MORE_INPUT) break;
            if (r != AMF_INPUT_FULL && r != AMF_DECODER_NO_FREE_SURFACES) {
                logf("AMF: SubmitInput failed: %d", static_cast<int>(r));
                return false;
            }
            if (GetTickCount64() > deadline) {
                logf("AMF: input stayed full");
                return false;
            }
            Sleep(1);
        }

        // [Ref AMFComponent] Input and output are not tied together, so this waits rather than
        // assuming a result: the texture cannot be reused before the encoder has finished with
        // it. In this usage no frames are held back, as B-pictures are off and low latency on.
        // [HEVC QUERY_TIMEOUT] QueryOutput blocks for up to the timeout, so this is no busy loop.
        // AMF_REPEAT is "not ready yet" (wiki: Guide for Video CODEC Encoder App Developers).
        for (;;) {
            amf::AMFDataPtr data;
            const AMF_RESULT r = component_->QueryOutput(&data);
            if (r == AMF_OK && data) return deliver(data, out);
            if (r != AMF_OK && r != AMF_REPEAT) {
                logf("AMF: QueryOutput failed: %d", static_cast<int>(r));
                return false;
            }
            if (GetTickCount64() > deadline) {
                logf("AMF: no output in time");
                return false;
            }
        }
    }

    bool setBitrate(uint32_t kbps) override {
        // [HEVC 2.2.2] Changed properties are "flushed to encoder only before the next Submit()".
        // Listed as dynamic by the wiki guide.
        const amf_int64 bits = static_cast<amf_int64>(kbps) * 1000;
        const auto target = hevc_ ? AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE : AMF_VIDEO_ENCODER_TARGET_BITRATE;
        const auto peak = hevc_ ? AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE : AMF_VIDEO_ENCODER_PEAK_BITRATE;
        return component_->SetProperty(target, bits) == AMF_OK && component_->SetProperty(peak, bits) == AMF_OK;
    }

    const char* name() const override { return "AMF"; }
    const char* kind() const override { return "amf"; }

private:
    // Everything that is required for the stream to be right fails the setup. What only makes
    // it better is logged: an older GPU may not know a property, and is still worth using.
    bool required(const wchar_t* property, const amf::AMFVariantStruct& value) {
        if (component_->SetProperty(property, value) == AMF_OK) return true;
        logf("AMF: cannot set %ls", property);
        return false;
    }
    void preferred(const wchar_t* property, const amf::AMFVariantStruct& value) {
        if (component_->SetProperty(property, value) != AMF_OK) logf("AMF: %ls not accepted", property);
    }
    static amf::AMFVariantStruct number(amf_int64 v) {
        amf::AMFVariantStruct s;
        AMFVariantInit(&s);
        AMFVariantAssignInt64(&s, v);
        return s;
    }
    static amf::AMFVariantStruct flag(bool v) {
        amf::AMFVariantStruct s;
        AMFVariantInit(&s);
        AMFVariantAssignBool(&s, v);
        return s;
    }
    static amf::AMFVariantStruct size(amf_int32 w, amf_int32 h) {
        amf::AMFVariantStruct s;
        AMFVariantInit(&s);
        AMFVariantAssignSize(&s, AMFConstructSize(w, h));
        return s;
    }
    static amf::AMFVariantStruct rate(amf_uint32 fps) {
        amf::AMFVariantStruct s;
        AMFVariantInit(&s);
        AMFVariantAssignRate(&s, AMFConstructRate(fps, 1));
        return s;
    }

    bool configure() {
        const amf_int64 bits = static_cast<amf_int64>(cfg_.kbps) * 1000;
        const amf_uint32 fps = static_cast<amf_uint32>(cfg_.fps);
        // [HEVC 2.2.1] Static properties must be set before Init, and usage is one of them.
        // Usage goes first, because the defaults of many others depend on it ([HEVC Table A-1],
        // "Default Value associated with usages"), and the choices below are meant to override.
        //
        // [HEVC 1.2] Low Latency's intended use is "Video collaboration, remote desktop", which
        // is this program. Ultra-low latency is for "Video game streaming" and caps the bits per
        // frame, at the cost of text on a screen that was just changed.
        if (hevc_) {
            if (!required(AMF_VIDEO_ENCODER_HEVC_USAGE, number(AMF_VIDEO_ENCODER_HEVC_USAGE_LOW_LATENCY))) return false;
            // [HEVC LOWLATENCY_MODE] "Enables low latency mode in the encoder."
            preferred(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, flag(true));
            // [HEVC QUALITY_PRESET] Speed is already the default of this usage. Set for certainty.
            preferred(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, number(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED));
            if (!required(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, size(cfg_.width, cfg_.height))) return false;
            if (!required(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, rate(fps))) return false;
            // [HEVC RATE_CONTROL_METHOD] CBR, as with the other encoders: the rate controller
            // above this class assumes the target is what is sent. Filler data stays off, its
            // default ([HEVC FILLER_DATA_ENABLE]), so that an idle desktop sends next to nothing.
            preferred(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, number(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR));
            if (!required(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, number(bits))) return false;
            preferred(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, number(bits));
            // [HEVC GOP_SIZE] "0 means only insert the first IDR/CRA (infinite GOP size)".
            // Keyframes are asked for per frame, when a device has lost something.
            preferred(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, number(0));
            // [HEVC QUERY_TIMEOUT] Reduces "polling load when QueryOutput is called".
            preferred(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, number(kQueryTimeoutMs));
            return true;
        }
        if (!required(AMF_VIDEO_ENCODER_USAGE, number(AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY))) return false;
        preferred(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, flag(true));
        preferred(AMF_VIDEO_ENCODER_QUALITY_PRESET, number(AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED));
        if (!required(AMF_VIDEO_ENCODER_FRAMESIZE, size(cfg_.width, cfg_.height))) return false;
        if (!required(AMF_VIDEO_ENCODER_FRAMERATE, rate(fps))) return false;
        preferred(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, number(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR));
        if (!required(AMF_VIDEO_ENCODER_TARGET_BITRATE, number(bits))) return false;
        preferred(AMF_VIDEO_ENCODER_PEAK_BITRATE, number(bits));
        // [AVC FORCE_PICTURE_TYPE] "to use this feature, set AMF_VIDEO_ENCODER_IDR_PERIOD to 0".
        // That makes it required here: without forced IDRs, a phone cannot recover from loss.
        if (!required(AMF_VIDEO_ENCODER_IDR_PERIOD, number(0))) return false;
        // [AVC B_PIC_PATTERN] "0 indicates that B-pictures are not used", needed for latency.
        // Not supported at all by VCE 1.0, which then has none anyway, so only logged.
        preferred(AMF_VIDEO_ENCODER_B_PIC_PATTERN, number(0));
        preferred(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, number(kQueryTimeoutMs));
        return true;
    }

    // [HEVC Table A-2], [AVC Table of frame per-submission parameters]: set on the surface,
    // for that frame. With no periodic IDR there is no periodic header either, so the
    // parameter sets are asked for along with every keyframe: a phone that lost its place
    // needs both, and they come as part of the stream.
    void setKeyframe(amf::AMFSurface* s) {
        if (hevc_) {
            s->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR));
            s->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
        } else {
            s->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
            s->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            s->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }
    }

    bool deliver(amf::AMFData* data, const Output& out) {
        // [HEVC 2.1] Output comes as AMFBuffer objects.
        amf::AMFBufferPtr buffer(data);
        if (!buffer) {
            logf("AMF: output is not a buffer");
            return false;
        }
        // [HEVC OUTPUT_DATA_TYPE] Says what kind of picture this one turned out to be.
        amf_int64 type = -1;
        const auto typeName = hevc_ ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE;
        data->GetProperty(typeName, &type);
        const bool key = type == (hevc_ ? static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR)
                                        : static_cast<amf_int64>(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR));
        out(static_cast<const uint8_t*>(buffer->GetNative()), buffer->GetSize(), key);
        return true;
    }

    EncoderConfig cfg_{};
    bool hevc_ = false;
    bool firstFrame_ = true;
    HMODULE dll_ = nullptr;
    amf::AMFContextPtr context_;
    amf::AMFComponentPtr component_;
    amf::AMFComponentPtr encoder_;
};

}  // namespace

std::unique_ptr<Encoder> createAmfEncoder(const EncoderConfig& cfg) {
    if (adapterVendor(cfg.device) != kVendorAmd) return nullptr;
    auto e = std::make_unique<AmfEncoder>();
    if (!e->init(cfg)) return nullptr;
    return e;
}

}  // namespace sd
