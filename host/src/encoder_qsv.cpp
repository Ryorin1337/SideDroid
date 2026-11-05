// Intel Quick Sync through oneVPL (API 2.10 or newer, to hand over D3D11 textures without
// copying them). Older Intel GPUs have no such runtime, and use Media Foundation instead.
// Sources: https://intel.github.io/libvpl/latest/ and examples/api2x/hello-sharing-dx11.
//
// Texture import is still marked experimental in the headers, though Intel's own D3D11 sample
// uses it. If a runtime disagrees, the import fails, and the session moves on to Media Foundation.
#define ONEVPL_EXPERIMENTAL

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

#include "encoder.h"
#include "log.h"
#include "sidedroid/wire.h"
#include "vpl/mfx.h"
#include "vpl/mfxmemory.h"

namespace sd {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kVendorIntel = 0x8086;
constexpr ULONGLONG kOutputTimeoutMs = 500;

template <typename T>
bool filter(mfxLoader loader, const char* property, mfxVariantType type, T value) {
    mfxConfig cfg = MFXCreateConfig(loader);
    if (!cfg) return false;
    mfxVariant v{};
    v.Type = type;
    if constexpr (std::is_same_v<T, mfxU32>) v.Data.U32 = value;
    return MFXSetConfigFilterProperty(cfg, reinterpret_cast<const mfxU8*>(property), v) == MFX_ERR_NONE;
}

// oneVPL picks hardware by the adapter's position among those DXGI lists.
bool adapterIndexOf(ID3D11Device* device, mfxU32* index) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter))) return false;
    DXGI_ADAPTER_DESC want{};
    adapter->GetDesc(&want);
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> candidate;
    for (UINT i = 0; factory->EnumAdapters1(i, candidate.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        candidate->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == want.AdapterLuid.LowPart && d.AdapterLuid.HighPart == want.AdapterLuid.HighPart) {
            *index = i;
            return true;
        }
    }
    return false;
}

void setKbps(mfxVideoParam& p, uint32_t kbps) {
    // The field is 16 bits, and the multiplier is how larger values fit.
    const mfxU16 multiplier = static_cast<mfxU16>(std::max<uint32_t>(1, kbps / 65535 + 1));
    p.mfx.BRCParamMultiplier = multiplier;
    p.mfx.TargetKbps = static_cast<mfxU16>(kbps / multiplier);
    p.mfx.MaxKbps = p.mfx.TargetKbps;
}

class QsvEncoder final : public Encoder {
public:
    ~QsvEncoder() override {
        if (session_) {
            MFXVideoENCODE_Close(session_);
            MFXClose(session_);
        }
        if (loader_) MFXUnload(loader_);
    }

    bool init(const EncoderConfig& cfg) {
        cfg_ = cfg;
        mfxU32 adapterIndex = 0;
        if (!adapterIndexOf(cfg.device, &adapterIndex)) return false;

        loader_ = MFXLoad();
        if (!loader_) return false;
        const mfxU32 codec = cfg.codec == kCodecHevc ? MFX_CODEC_HEVC : MFX_CODEC_AVC;
        // API 2.10: "new interfaces for sharing surfaces".
        const mfxU32 minimumApi = (2u << 16) | 10u;
        const bool filtered =
            filter<mfxU32>(loader_, "mfxImplDescription.Impl", MFX_VARIANT_TYPE_U32, MFX_IMPL_TYPE_HARDWARE) &&
            filter<mfxU32>(loader_, "mfxImplDescription.AccelerationMode", MFX_VARIANT_TYPE_U32, MFX_ACCEL_MODE_VIA_D3D11) &&
            filter<mfxU32>(loader_, "mfxImplDescription.mfxEncoderDescription.encoder.CodecID", MFX_VARIANT_TYPE_U32, codec) &&
            filter<mfxU32>(loader_, "mfxImplDescription.ApiVersion.Version", MFX_VARIANT_TYPE_U32, minimumApi) &&
            filter<mfxU32>(loader_, "DXGIAdapterIndex", MFX_VARIANT_TYPE_U32, adapterIndex);
        if (!filtered) return false;
        if (MFXCreateSession(loader_, 0, &session_) != MFX_ERR_NONE) {
            session_ = nullptr;
            return false;
        }
        // "The handle must remain valid until after the application calls the MFXClose function."
        // The pipeline owns the device for longer than this encoder. It is multithread
        // protected, as the guide requires (createDeviceOnAdapter).
        if (MFXVideoCORE_SetHandle(session_, MFX_HANDLE_D3D11_DEVICE, cfg.device) != MFX_ERR_NONE) return false;

        mfxVideoParam p{};
        p.mfx.CodecId = codec;
        p.mfx.TargetUsage = MFX_TARGETUSAGE_BEST_SPEED;
        p.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
        setKbps(p, cfg.kbps);
        p.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(cfg.fps);
        p.mfx.FrameInfo.FrameRateExtD = 1;
        p.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        p.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        p.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        p.mfx.FrameInfo.CropW = static_cast<mfxU16>(cfg.width);
        p.mfx.FrameInfo.CropH = static_cast<mfxU16>(cfg.height);
        p.mfx.FrameInfo.Width = static_cast<mfxU16>((cfg.width + 15) & ~15);
        p.mfx.FrameInfo.Height = static_cast<mfxU16>((cfg.height + 15) & ~15);
        p.mfx.GopRefDist = 1;      // no B-frames: nothing is held back
        p.mfx.NumRefFrame = 1;
        p.mfx.GopPicSize = 0xFFFF;  // keyframes come when asked for
        p.AsyncDepth = 1;           // one at a time, synchronized after each
        p.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;

        // Without HRD conformance a change in bitrate needs no new keyframe. And "It is
        // recommended to set this value equal to the number of reference frames."
        mfxExtCodingOption option{};
        option.Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
        option.Header.BufferSz = sizeof(option);
        option.NalHrdConformance = MFX_CODINGOPTION_OFF;
        option.MaxDecFrameBuffering = 1;
        mfxExtBuffer* extra[] = {&option.Header};
        p.ExtParam = extra;
        p.NumExtParam = 1;

        const mfxStatus queried = MFXVideoENCODE_Query(session_, &p, &p);
        if (queried < MFX_ERR_NONE) {
            logf("Quick Sync: query failed: %d", static_cast<int>(queried));
            return false;
        }
        const mfxStatus started = MFXVideoENCODE_Init(session_, &p);
        if (started < MFX_ERR_NONE) {
            logf("Quick Sync: init failed: %d", static_cast<int>(started));
            return false;
        }

        if (MFXGetMemoryInterface(session_, &memory_) != MFX_ERR_NONE || !memory_) {
            logf("Quick Sync: no memory interface");
            return false;
        }

        // The working values, as the guide advises reading before changing any bitrate.
        mfxVideoParam working{};
        if (MFXVideoENCODE_GetVideoParam(session_, &working) != MFX_ERR_NONE) return false;
        const size_t bytes = static_cast<size_t>(working.mfx.BufferSizeInKB) * std::max<mfxU16>(1, working.mfx.BRCParamMultiplier) * 1000;
        output_.resize(std::max<size_t>(bytes, 1024 * 1024));
        return true;
    }

    bool encode(ID3D11Texture2D* nv12, bool forceKeyframe, const Output& out) override {
        mfxSurfaceD3D11Tex2D external{};
        external.SurfaceInterface.Header.SurfaceType = MFX_SURFACE_TYPE_D3D11_TEX2D;
        // Zero copy is preferred, and copying is allowed where that is not possible.
        external.SurfaceInterface.Header.SurfaceFlags = MFX_SURFACE_FLAG_IMPORT_SHARED | MFX_SURFACE_FLAG_IMPORT_COPY;
        external.SurfaceInterface.Header.StructSize = sizeof(external);
        external.texture2D = nv12;
        mfxFrameSurface1* surface = nullptr;
        mfxStatus status = memory_->ImportFrameSurface(memory_, MFX_SURFACE_COMPONENT_ENCODE, &external.SurfaceInterface.Header, &surface);
        if (status != MFX_ERR_NONE || !surface) {
            logf("Quick Sync: cannot import the picture: %d", static_cast<int>(status));
            return false;
        }

        mfxBitstream stream{};
        stream.Data = output_.data();
        stream.MaxLength = static_cast<mfxU32>(output_.size());

        // In display order, "only key frames are enforceable" through the frame type.
        mfxEncodeCtrl control{};
        control.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_REF | MFX_FRAMETYPE_IDR;

        const ULONGLONG deadline = GetTickCount64() + kOutputTimeoutMs;
        mfxSyncPoint sync = nullptr;
        for (;;) {
            status = MFXVideoENCODE_EncodeFrameAsync(session_, forceKeyframe ? &control : nullptr, surface, &stream, &sync);
            // "Call this function again after MFXVideoCORE_SyncOperation or in a few milliseconds."
            if (status != MFX_WRN_DEVICE_BUSY) break;
            if (sync) MFXVideoCORE_SyncOperation(session_, sync, 100);
            else Sleep(2);
            if (GetTickCount64() > deadline) break;
        }
        surface->FrameInterface->Release(surface);

        // Not every call generates output.
        if (status == MFX_ERR_MORE_DATA) return true;
        if (status < MFX_ERR_NONE || !sync) {
            logf("Quick Sync: encode failed: %d", static_cast<int>(status));
            return false;
        }
        do {
            status = MFXVideoCORE_SyncOperation(session_, sync, 100);
        } while (status == MFX_WRN_IN_EXECUTION && GetTickCount64() < deadline);
        if (status != MFX_ERR_NONE) {
            logf("Quick Sync: sync failed: %d", static_cast<int>(status));
            return false;
        }
        out(stream.Data + stream.DataOffset, stream.DataLength, (stream.FrameType & MFX_FRAMETYPE_IDR) != 0);
        return true;
    }

    bool setBitrate(uint32_t kbps) override {
        mfxVideoParam p{};
        if (MFXVideoENCODE_GetVideoParam(session_, &p) != MFX_ERR_NONE) return false;
        setKbps(p, kbps);
        // Continues the sequence: no keyframe just because the rate moved.
        mfxExtEncoderResetOption reset{};
        reset.Header.BufferId = MFX_EXTBUFF_ENCODER_RESET_OPTION;
        reset.Header.BufferSz = sizeof(reset);
        reset.StartNewSequence = MFX_CODINGOPTION_OFF;
        mfxExtBuffer* extra[] = {&reset.Header};
        p.ExtParam = extra;
        p.NumExtParam = 1;
        return MFXVideoENCODE_Reset(session_, &p) >= MFX_ERR_NONE;
    }

    const char* name() const override { return "Quick Sync"; }
    const char* kind() const override { return "qsv"; }

private:
    EncoderConfig cfg_{};
    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    mfxMemoryInterface* memory_ = nullptr;
    std::vector<mfxU8> output_;
};

}  // namespace

std::unique_ptr<Encoder> createQsvEncoder(const EncoderConfig& cfg) {
    if (adapterVendor(cfg.device) != kVendorIntel) return nullptr;
    auto e = std::make_unique<QsvEncoder>();
    if (!e->init(cfg)) return nullptr;
    return e;
}

}  // namespace sd
