#pragma once
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sd {

struct EncoderConfig {
    ID3D11Device* device = nullptr;
    int width = 0;
    int height = 0;
    int fps = 60;
    uint32_t kbps = 12000;
    uint8_t codec = 2;  // sd::Codec
};

class Encoder {
public:
    using Output = std::function<void(const uint8_t* annexB, size_t size, bool keyframe)>;
    virtual ~Encoder() = default;
    // Blocks until output for this NV12 picture is available.
    virtual bool encode(ID3D11Texture2D* nv12, bool forceKeyframe, const Output& out) = 0;
    virtual bool setBitrate(uint32_t kbps) = 0;
    virtual const char* name() const = 0;
    // "nvenc", "amf", "qsv", "mf" or "sw": what the settings call it.
    virtual const char* kind() const = 0;
    // True for an encoder that takes its picture from memory instead, through encodeMemory.
    virtual bool wantsMemoryInput() const { return false; }
    // True if it can produce far more than the target, so that the caller has to hold it back.
    virtual bool overshootsTarget() const { return false; }
    virtual bool encodeMemory(const uint8_t* /*nv12*/, bool /*forceKeyframe*/, const Output&) { return false; }
};

std::unique_ptr<Encoder> createNvencEncoder(const EncoderConfig& cfg);
std::unique_ptr<Encoder> createAmfEncoder(const EncoderConfig& cfg);
std::unique_ptr<Encoder> createQsvEncoder(const EncoderConfig& cfg);
std::unique_ptr<Encoder> createMfEncoder(const EncoderConfig& cfg);
// On the CPU. cfg.device is not used.
std::unique_ptr<Encoder> createSoftwareEncoder(const EncoderConfig& cfg);

// PCI vendor id of the GPU behind a device.
UINT adapterVendor(ID3D11Device* device);
void ensureMediaFoundationStarted();
// Whether a keyframe already starts with what a decoder needs in order to begin there.
bool annexBHasParameterSets(const uint8_t* data, size_t size, bool hevc);

}  // namespace sd
