// Software encoding on the CPU, with the encoders that come with Windows. The last resort, for
// computers with no hardware encoder at all, such as virtual machines. Slow compared to any GPU.
// It takes its picture from memory, because such a computer may have no GPU to convert it with:
// on WARP, "QueryInterface ... ID3D11VideoDevice ... E_NOINTERFACE".
// Sources: learn.microsoft.com/windows/win32/medfound/h-264-video-encoder and h-265---hevc-video-encoder.
//
// Its rate control is only advice: asked for 8 Mbps on moving noise it made about 80, whichever
// mode or limit it was given, though 0.7 on an ordinary desktop. So the session limits how often
// it encodes, by what it really produced. See paceByOutput in host.cpp.
#include <windows.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#include "encoder.h"
#include "log.h"
#include "sidedroid/wire.h"

namespace sd {

namespace {

using Microsoft::WRL::ComPtr;

class SoftwareEncoder final : public Encoder {
public:
    ~SoftwareEncoder() override {
        if (transform_) {
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
    }

    bool init(const EncoderConfig& cfg) {
        ensureMediaFoundationStarted();
        cfg_ = cfg;
        const GUID subtype = cfg.codec == kCodecHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, subtype};
        IMFActivate** list = nullptr;
        UINT32 count = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &outInfo, &list, &count)) || count == 0) return false;
        bool ok = false;
        for (UINT32 i = 0; i < count && !ok; ++i) {
            ok = tryActivate(list[i]);
            if (!ok) transform_.Reset();
        }
        for (UINT32 i = 0; i < count; ++i) list[i]->Release();
        CoTaskMemFree(list);
        return ok;
    }

    bool encode(ID3D11Texture2D*, bool, const Output&) override { return false; }

    bool wantsMemoryInput() const override { return true; }
    bool overshootsTarget() const override { return true; }

    bool encodeMemory(const uint8_t* nv12, bool forceKeyframe, const Output& out) override {
        if (forceKeyframe && codecApi_) {
            VARIANT v{};
            v.vt = VT_UI4;
            v.ulVal = 1;
            codecApi_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        }
        const DWORD size = static_cast<DWORD>(cfg_.width) * cfg_.height * 3 / 2;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(size, &buffer))) return false;
        BYTE* p = nullptr;
        if (FAILED(buffer->Lock(&p, nullptr, nullptr))) return false;
        memcpy(p, nv12, size);
        buffer->Unlock();
        buffer->SetCurrentLength(size);
        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) return false;
        sample->AddBuffer(buffer.Get());
        const LONGLONG interval = 10'000'000 / cfg_.fps;
        sample->SetSampleTime(pts_);
        sample->SetSampleDuration(interval);
        pts_ += interval;

        const HRESULT in = transform_->ProcessInput(0, sample.Get(), 0);
        if (FAILED(in)) {
            logf("software encoder: ProcessInput failed: 0x%08lx", static_cast<unsigned long>(in));
            return false;
        }
        // Whatever is ready. It may hold on to a frame, which is fine as this one was copied.
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            transform_->GetOutputStreamInfo(0, &info);
            MFT_OUTPUT_DATA_BUFFER db{};
            ComPtr<IMFSample> own;
            if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                ComPtr<IMFMediaBuffer> outBuffer;
                if (FAILED(MFCreateMemoryBuffer(info.cbSize ? info.cbSize : size, &outBuffer))) return false;
                MFCreateSample(&own);
                own->AddBuffer(outBuffer.Get());
                db.pSample = own.Get();
            }
            DWORD status = 0;
            const HRESULT hr = transform_->ProcessOutput(0, 1, &db, &status);
            if (db.pEvents) db.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> t;
                if (SUCCEEDED(transform_->GetOutputAvailableType(0, 0, &t))) transform_->SetOutputType(0, t.Get(), 0);
                continue;
            }
            if (FAILED(hr)) {
                logf("software encoder: ProcessOutput failed: 0x%08lx", static_cast<unsigned long>(hr));
                return false;
            }
            const bool delivered = deliver(db.pSample, out);
            if (!own && db.pSample) db.pSample->Release();
            if (!delivered) return false;
        }
    }

    bool setBitrate(uint32_t kbps) override {
        if (!codecApi_) return false;
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = kbps * 1000;
        return SUCCEEDED(codecApi_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v));
    }

    const char* name() const override { return "Software"; }
    const char* kind() const override { return "sw"; }

private:
    bool tryActivate(IMFActivate* a) {
        if (FAILED(a->ActivateObject(IID_PPV_ARGS(&transform_)))) return false;
        transform_.As(&codecApi_);
        applySettings();
        // "The output type must be set before the input type."
        const GUID subtype = cfg_.codec == kCodecHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        ComPtr<IMFMediaType> outType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, subtype);
        outType->SetUINT32(MF_MT_AVG_BITRATE, cfg_.kbps * 1000);  // "Must be greater than zero."
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, cfg_.fps, 1);
        MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        HRESULT hr = transform_->SetOutputType(0, outType.Get(), 0);
        if (FAILED(hr)) {
            logf("software encoder: output type refused: 0x%08lx", static_cast<unsigned long>(hr));
            return false;
        }
        ComPtr<IMFMediaType> inType;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, cfg_.fps, 1);
        MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        inType->SetUINT32(MF_MT_DEFAULT_STRIDE, cfg_.width);
        hr = transform_->SetInputType(0, inType.Get(), 0);
        if (FAILED(hr)) {
            logf("software encoder: input type refused: 0x%08lx", static_cast<unsigned long>(hr));
            return false;
        }
        // "the encoder adds MF_MT_MPEG_SEQUENCE_HEADER": kept in case the stream lacks them.
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(transform_->GetOutputCurrentType(0, &current))) {
            UINT32 bytes = 0;
            if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &bytes)) && bytes > 0) {
                sequenceHeader_.resize(bytes);
                current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader_.data(), bytes, nullptr);
            }
        }
        applySettings();
        if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return false;
        if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return false;
        return true;
    }

    void setValue(const GUID& property, VARTYPE type, ULONG value, const char* name) {
        VARIANT v{};
        v.vt = type;
        if (type == VT_BOOL) v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        else v.ulVal = value;
        const HRESULT hr = codecApi_->SetValue(&property, &v);
        if (FAILED(hr)) logf("software encoder: %s refused: 0x%08lx", name, static_cast<unsigned long>(hr));
    }

    void applySettings() {
        if (!codecApi_) return;
        // On by default in the H.264 encoder ("slice encoding ... to minimize latency").
        setValue(CODECAPI_AVLowLatencyMode, VT_BOOL, 1, "low latency");
        setValue(CODECAPI_AVEncCommonRateControlMode, VT_UI4, eAVEncCommonRateControlMode_CBR, "rate control");  // the HEVC one takes CBR or Quality
        setValue(CODECAPI_AVEncCommonMeanBitRate, VT_UI4, cfg_.kbps * 1000, "mean bitrate");
        setValue(CODECAPI_AVEncMPVDefaultBPictureCount, VT_UI4, 0, "B pictures");
        setValue(CODECAPI_AVEncMPVGOPSize, VT_UI4, 0xFFFFFFFF, "GOP size");  // "If zero, the encoder selects": keyframes come when asked for
    }

    bool deliver(IMFSample* sample, const Output& out) {
        ComPtr<IMFMediaBuffer> flat;
        if (FAILED(sample->ConvertToContiguousBuffer(&flat))) return false;
        BYTE* p = nullptr;
        DWORD length = 0;
        if (FAILED(flat->Lock(&p, nullptr, &length))) return false;
        UINT32 clean = 0;
        sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
        if (clean && !sequenceHeader_.empty() && !annexBHasParameterSets(p, length, cfg_.codec == kCodecHevc)) {
            scratch_.assign(sequenceHeader_.begin(), sequenceHeader_.end());
            scratch_.insert(scratch_.end(), p, p + length);
            out(scratch_.data(), scratch_.size(), true);
        } else {
            out(p, length, clean != 0);
        }
        flat->Unlock();
        return true;
    }

    EncoderConfig cfg_{};
    ComPtr<IMFTransform> transform_;
    ComPtr<ICodecAPI> codecApi_;
    std::vector<uint8_t> sequenceHeader_, scratch_;
    LONGLONG pts_ = 0;
};

}  // namespace

std::unique_ptr<Encoder> createSoftwareEncoder(const EncoderConfig& cfg) {
    auto e = std::make_unique<SoftwareEncoder>();
    if (!e->init(cfg)) return nullptr;
    return e;
}

}  // namespace sd
