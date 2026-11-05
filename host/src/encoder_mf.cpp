// Hardware encoding through Media Foundation transforms. Covers Intel and AMD,
// and NVIDIA when the NVENC path is unavailable.
#include <windows.h>

#include <codecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>

#include "encoder.h"
#include "log.h"
#include "sidedroid/wire.h"

#pragma comment(lib, "wmcodecdspuuid.lib")

namespace sd {

namespace {

using Microsoft::WRL::ComPtr;

class MfEncoder final : public Encoder {
public:
    ~MfEncoder() override {
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
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                             &outInfo, &list, &count)) || count == 0) {
            return false;
        }

        // Each GPU registers its own MFT, and only the matching one accepts our device.
        const UINT vendor = adapterVendor(cfg.device);
        bool ok = false;
        for (UINT i = 0; i < count && !ok; ++i) {
            UINT32 mftVendor = 0;
            WCHAR vendorId[16]{};
            if (SUCCEEDED(list[i]->GetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, vendorId, 16, nullptr))) {
                // Format is "VEN_10DE".
                mftVendor = wcstoul(vendorId + 4, nullptr, 16);
                if (mftVendor != 0 && mftVendor != vendor) continue;
            }
            ok = tryActivate(list[i]);
            if (!ok) transform_.Reset();
        }
        for (UINT i = 0; i < count; ++i) list[i]->Release();
        CoTaskMemFree(list);
        return ok;
    }

    bool encode(ID3D11Texture2D* nv12, bool forceKey, const Output& out) override {
        if (forceKey && codecApi_) {
            VARIANT v{};
            v.vt = VT_UI4;
            v.ulVal = 1;
            codecApi_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer))) return false;
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        LONGLONG interval = 10'000'000 / cfg_.fps;
        sample->SetSampleTime(pts_);
        sample->SetSampleDuration(interval);
        pts_ += interval;

        // Async MFT protocol: wait for NeedInput, submit, wait for HaveOutput.
        bool submitted = false;
        if (pendingNeedInput_ > 0) {
            if (FAILED(transform_->ProcessInput(0, sample.Get(), 0))) return false;
            --pendingNeedInput_;
            submitted = true;
        }
        const ULONGLONG deadline = GetTickCount64() + 500;
        while (GetTickCount64() < deadline) {
            ComPtr<IMFMediaEvent> ev;
            HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                Sleep(0);
                continue;
            }
            if (FAILED(hr)) return false;
            MediaEventType t = MEUnknown;
            ev->GetType(&t);
            if (t == METransformNeedInput) {
                if (!submitted) {
                    if (FAILED(transform_->ProcessInput(0, sample.Get(), 0))) return false;
                    submitted = true;
                } else {
                    ++pendingNeedInput_;
                }
            } else if (t == METransformHaveOutput) {
                if (!drainOne(out)) return false;
                if (submitted) return true;
            }
        }
        logf("MF encoder: timeout (submitted=%d)", submitted ? 1 : 0);
        return false;
    }

    bool setBitrate(uint32_t kbps) override {
        if (!codecApi_) return false;
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = kbps * 1000;
        return SUCCEEDED(codecApi_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v));
    }

    const char* name() const override { return "Media Foundation"; }
    const char* kind() const override { return "mf"; }

private:
    bool tryActivate(IMFActivate* a) {
        if (FAILED(a->ActivateObject(IID_PPV_ARGS(&transform_)))) return false;
        ComPtr<IMFAttributes> attrs;
        if (FAILED(transform_->GetAttributes(&attrs))) return false;
        UINT32 isAsync = 0;
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        if (!isAsync) return false;
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        if (FAILED(transform_.As(&events_))) return false;

        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &manager_))) return false;
        if (FAILED(manager_->ResetDevice(cfg_.device, token))) return false;
        if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.Get()))))
            return false;

        transform_.As(&codecApi_);
        if (codecApi_) {
            VARIANT v{};
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            codecApi_->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            v.vt = VT_UI4;
            v.ulVal = eAVEncCommonRateControlMode_CBR;
            codecApi_->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
            v.ulVal = cfg_.kbps * 1000;
            codecApi_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
            v.ulVal = 0;
            codecApi_->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
            v.ulVal = 0xFFFFFFFF;  // effectively no periodic keyframes; loss triggers them instead
            codecApi_->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        }

        const GUID subtype = cfg_.codec == kCodecHevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        ComPtr<IMFMediaType> outType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, subtype);
        outType->SetUINT32(MF_MT_AVG_BITRATE, cfg_.kbps * 1000);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, cfg_.fps, 1);
        MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(transform_->SetOutputType(0, outType.Get(), 0))) return false;

        ComPtr<IMFMediaType> inType;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, cfg_.fps, 1);
        MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (FAILED(transform_->SetInputType(0, inType.Get(), 0))) return false;

        // Some encoders publish parameter sets only here, not in the stream.
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(transform_->GetOutputCurrentType(0, &current))) {
            UINT32 size = 0;
            if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0) {
                sequenceHeader_.resize(size);
                current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader_.data(), size, nullptr);
            }
        }

        if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0))) return false;
        if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))) return false;
        if (FAILED(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) return false;
        return true;
    }

    bool drainOne(const Output& out) {
        MFT_OUTPUT_STREAM_INFO info{};
        transform_->GetOutputStreamInfo(0, &info);

        MFT_OUTPUT_DATA_BUFFER db{};
        ComPtr<IMFSample> own;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            ComPtr<IMFMediaBuffer> buf;
            MFCreateMemoryBuffer(info.cbSize ? info.cbSize : 8 * 1024 * 1024, &buf);
            MFCreateSample(&own);
            own->AddBuffer(buf.Get());
            db.pSample = own.Get();
        }
        DWORD status = 0;
        HRESULT hr = transform_->ProcessOutput(0, 1, &db, &status);
        if (db.pEvents) db.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Renegotiation: accept what the encoder now offers.
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(transform_->GetOutputAvailableType(0, 0, &t))) transform_->SetOutputType(0, t.Get(), 0);
            return true;
        }
        if (FAILED(hr)) {
            logf("MF ProcessOutput failed: 0x%08lx", static_cast<unsigned long>(hr));
            return false;
        }
        IMFSample* sample = db.pSample;
        ComPtr<IMFMediaBuffer> flat;
        bool ok = SUCCEEDED(sample->ConvertToContiguousBuffer(&flat));
        if (ok) {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(flat->Lock(&p, nullptr, &len))) {
                UINT32 clean = 0;
                sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
                if (clean && !sequenceHeader_.empty() && !annexBHasParameterSets(p, len, cfg_.codec == kCodecHevc)) {
                    scratch_.assign(sequenceHeader_.begin(), sequenceHeader_.end());
                    scratch_.insert(scratch_.end(), p, p + len);
                    out(scratch_.data(), scratch_.size(), true);
                } else {
                    out(p, len, clean != 0);
                }
                flat->Unlock();
            }
        }
        if (!own && sample) sample->Release();
        return ok;
    }

    EncoderConfig cfg_{};
    ComPtr<IMFTransform> transform_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<ICodecAPI> codecApi_;
    std::vector<uint8_t> sequenceHeader_, scratch_;
    LONGLONG pts_ = 0;
    int pendingNeedInput_ = 0;
};

}  // namespace

void ensureMediaFoundationStarted() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
}

UINT adapterVendor(ID3D11Device* dev) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC d{};
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
        adapter->GetDesc(&d);
    return d.VendorId;
}

bool annexBHasParameterSets(const uint8_t* p, size_t len, bool hevc) {
    for (size_t i = 0; i + 4 < len && i < 256; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            const int type = hevc ? (p[i + 3] >> 1) & 0x3F : p[i + 3] & 0x1F;
            if (hevc ? (type == 32 || type == 33) : (type == 7)) return true;
        }
    }
    return false;
}

std::unique_ptr<Encoder> createMfEncoder(const EncoderConfig& cfg) {
    auto e = std::make_unique<MfEncoder>();
    if (!e->init(cfg)) return nullptr;
    return e;
}

}  // namespace sd
