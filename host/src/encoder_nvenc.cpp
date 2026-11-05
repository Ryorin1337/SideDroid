// NVENC through the driver's API, loaded at runtime, so that the host still
// starts on machines without an NVIDIA GPU.
#include <windows.h>

#include <d3d11.h>

#include "encoder.h"
#include "log.h"
#include "nvEncodeAPI.h"
#include "sidedroid/wire.h"

namespace sd {

namespace {

using CreateInstanceFn = NVENCSTATUS(NTAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using MaxVersionFn = NVENCSTATUS(NTAPI*)(uint32_t*);

class NvencEncoder final : public Encoder {
public:
    ~NvencEncoder() override { shutdown(); }

    const char* kind() const override { return "nvenc"; }

    bool init(const EncoderConfig& cfg) {
        cfg_ = cfg;
        dll_ = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll_) return false;

        auto maxVersion = reinterpret_cast<MaxVersionFn>(GetProcAddress(dll_, "NvEncodeAPIGetMaxSupportedVersion"));
        auto create = reinterpret_cast<CreateInstanceFn>(GetProcAddress(dll_, "NvEncodeAPICreateInstance"));
        if (!maxVersion || !create) return false;
        uint32_t driverVersion = 0;
        if (maxVersion(&driverVersion) != NV_ENC_SUCCESS) return false;
        const uint32_t ours = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (driverVersion < ours) {
            logf("NVENC: driver API %u.%u is older than %u.%u", driverVersion >> 4, driverVersion & 15,
                 NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
            return false;
        }

        api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (create(&api_) != NV_ENC_SUCCESS) return false;

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.device = cfg.device;
        open.apiVersion = NVENCAPI_VERSION;
        if (NVENCSTATUS s = api_.nvEncOpenEncodeSessionEx(&open, &enc_); s != NV_ENC_SUCCESS) {
            logf("NVENC: open session failed: %d", s);
            enc_ = nullptr;
            return false;
        }

        const GUID codec = cfg.codec == kCodecHevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
        const GUID preset = NV_ENC_PRESET_P3_GUID;

        NV_ENC_PRESET_CONFIG pc{};
        pc.version = NV_ENC_PRESET_CONFIG_VER;
        pc.presetCfg.version = NV_ENC_CONFIG_VER;
        if (NVENCSTATUS s = api_.nvEncGetEncodePresetConfigEx(enc_, codec, preset, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &pc);
            s != NV_ENC_SUCCESS) {
            logf("NVENC: preset query failed: %d", s);
            return false;
        }
        config_ = pc.presetCfg;
        config_.version = NV_ENC_CONFIG_VER;
        config_.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
        config_.gopLength = NVENC_INFINITE_GOPLENGTH;
        config_.frameIntervalP = 1;  // no B-frames
        config_.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        applyRate(cfg.kbps);

        if (cfg.codec == kCodecHevc) {
            auto& h = config_.encodeCodecConfig.hevcConfig;
            h.repeatSPSPPS = 1;  // every IDR is self-contained
            h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            h.chromaFormatIDC = 1;
        } else {
            auto& h = config_.encodeCodecConfig.h264Config;
            h.repeatSPSPPS = 1;
            h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            h.chromaFormatIDC = 1;
        }

        init_ = NV_ENC_INITIALIZE_PARAMS{};
        init_.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init_.encodeGUID = codec;
        init_.presetGUID = preset;
        init_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        init_.encodeWidth = init_.darWidth = init_.maxEncodeWidth = cfg.width;
        init_.encodeHeight = init_.darHeight = init_.maxEncodeHeight = cfg.height;
        init_.frameRateNum = cfg.fps;
        init_.frameRateDen = 1;
        init_.enablePTD = 1;
        init_.encodeConfig = &config_;
        if (NVENCSTATUS s = api_.nvEncInitializeEncoder(enc_, &init_); s != NV_ENC_SUCCESS) {
            logf("NVENC: initialize failed: %d", s);
            return false;
        }

        NV_ENC_CREATE_BITSTREAM_BUFFER bb{};
        bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (api_.nvEncCreateBitstreamBuffer(enc_, &bb) != NV_ENC_SUCCESS) return false;
        bitstream_ = bb.bitstreamBuffer;
        return true;
    }

    bool encode(ID3D11Texture2D* nv12, bool forceKey, const Output& out) override {
        if (nv12 != registeredTexture_) {
            unregister();
            NV_ENC_REGISTER_RESOURCE reg{};
            reg.version = NV_ENC_REGISTER_RESOURCE_VER;
            reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
            reg.resourceToRegister = nv12;
            reg.width = cfg_.width;
            reg.height = cfg_.height;
            reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
            reg.bufferUsage = NV_ENC_INPUT_IMAGE;
            if (NVENCSTATUS s = api_.nvEncRegisterResource(enc_, &reg); s != NV_ENC_SUCCESS) {
                logf("NVENC: register failed: %d", s);
                return false;
            }
            registered_ = reg.registeredResource;
            registeredTexture_ = nv12;
        }

        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = registered_;
        if (api_.nvEncMapInputResource(enc_, &map) != NV_ENC_SUCCESS) return false;

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = map.mappedBufferFmt;
        pic.inputWidth = cfg_.width;
        pic.inputHeight = cfg_.height;
        pic.outputBitstream = bitstream_;
        pic.inputTimeStamp = ++pts_;
        if (forceKey) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

        NVENCSTATUS status = api_.nvEncEncodePicture(enc_, &pic);
        bool ok = false;
        if (status == NV_ENC_SUCCESS) {
            NV_ENC_LOCK_BITSTREAM lock{};
            lock.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock.outputBitstream = bitstream_;
            if (api_.nvEncLockBitstream(enc_, &lock) == NV_ENC_SUCCESS) {
                bool key = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
                out(static_cast<const uint8_t*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes, key);
                api_.nvEncUnlockBitstream(enc_, bitstream_);
                ok = true;
            }
        } else {
            logf("NVENC: encode failed: %d", status);
        }
        api_.nvEncUnmapInputResource(enc_, map.mappedResource);
        return ok;
    }

    bool setBitrate(uint32_t kbps) override {
        applyRate(kbps);
        NV_ENC_RECONFIGURE_PARAMS re{};
        re.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        re.reInitEncodeParams = init_;
        re.reInitEncodeParams.encodeConfig = &config_;
        return api_.nvEncReconfigureEncoder(enc_, &re) == NV_ENC_SUCCESS;
    }

    const char* name() const override { return "NVENC"; }

private:
    void applyRate(uint32_t kbps) {
        auto& rc = config_.rcParams;
        rc.averageBitRate = kbps * 1000;
        rc.maxBitRate = kbps * 1000;
        // One frame of VBV keeps frame sizes flat, which is what keeps latency flat.
        rc.vbvBufferSize = kbps * 1000 / cfg_.fps;
        rc.vbvInitialDelay = rc.vbvBufferSize;
    }

    void unregister() {
        if (registered_) api_.nvEncUnregisterResource(enc_, registered_);
        registered_ = nullptr;
        registeredTexture_ = nullptr;
    }

    void shutdown() {
        if (enc_) {
            NV_ENC_PIC_PARAMS eos{};
            eos.version = NV_ENC_PIC_PARAMS_VER;
            eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
            api_.nvEncEncodePicture(enc_, &eos);
            unregister();
            if (bitstream_) api_.nvEncDestroyBitstreamBuffer(enc_, bitstream_);
            api_.nvEncDestroyEncoder(enc_);
            enc_ = nullptr;
        }
        if (dll_) FreeLibrary(dll_);
        dll_ = nullptr;
    }

    EncoderConfig cfg_{};
    HMODULE dll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* enc_ = nullptr;
    NV_ENC_CONFIG config_{};
    NV_ENC_INITIALIZE_PARAMS init_{};
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    NV_ENC_REGISTERED_PTR registered_ = nullptr;
    ID3D11Texture2D* registeredTexture_ = nullptr;
    uint64_t pts_ = 0;
};

}  // namespace

std::unique_ptr<Encoder> createNvencEncoder(const EncoderConfig& cfg) {
    auto e = std::make_unique<NvencEncoder>();
    if (!e->init(cfg)) return nullptr;
    return e;
}

}  // namespace sd
