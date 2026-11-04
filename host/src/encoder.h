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
};

std::unique_ptr<Encoder> createNvencEncoder(const EncoderConfig& cfg);
std::unique_ptr<Encoder> createMfEncoder(const EncoderConfig& cfg);

}  // namespace sd
