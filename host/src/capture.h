// Desktop Duplication capture, cursor compositing, and BGRA to NV12 conversion.
#pragma once
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sd {

using Microsoft::WRL::ComPtr;

// Creates a device on the adapter with this LUID.
ComPtr<ID3D11Device> createDeviceOnAdapter(LUID luid, ComPtr<ID3D11DeviceContext>* ctx);
// Finds the adapter currently driving this output.
bool findAdapterOfOutput(const std::wstring& deviceName, LUID* luid, std::wstring* adapterDesc, UINT* vendor);
// Adapters by preference for encoding: discrete NVIDIA first.
std::vector<std::pair<LUID, UINT>> listHardwareAdapters();

constexpr UINT kVendorNvidia = 0x10DE;
constexpr UINT kVendorAmd = 0x1002;
constexpr UINT kVendorIntel = 0x8086;
constexpr UINT kVendorMicrosoft = 0x1414;  // WARP, the software rasterizer that comes with Windows
struct GpuInfo {
    std::wstring name;
    UINT vendor = 0;
    LUID luid{};
};
// Real GPUs, and Microsoft's software one. Windows can report one twice, under different
// LUIDs, with the same name.
std::vector<GpuInfo> listGpus();
// Which encoders can be asked for, as a value for the settings and a label for people.
// The value is "<kind>|<GPU name>", with the kind "nvenc", "amf", "qsv" or "mf", or "sw|" for
// the CPU.
struct EncoderChoice {
    std::string id, label;
};
std::vector<EncoderChoice> encoderChoices();

enum class CaptureResult { NewFrame, Timeout, Lost };

class CaptureHub;

class DesktopCapture {
public:
    ~DesktopCapture();
    // Windows lets a process duplicate an output only once. Pass shared for an output
    // that several devices may show at the same time: one thread then reads it for all.
    bool open(const std::wstring& deviceName, bool shared = false);
    // On NewFrame the composed picture (with cursor) is in texture().
    CaptureResult acquire(int timeoutMs);
    ID3D11Texture2D* texture() const { return composed_.Get(); }
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return ctx_.Get(); }
    LUID adapterLuid() const { return luid_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool openShared(const std::wstring& deviceName);
    CaptureResult acquireShared(int timeoutMs);
    bool initCursorPipeline();
    void updateCursorShape(const DXGI_OUTDUPL_FRAME_INFO& info);
    void drawCursor();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> composed_;
    ComPtr<ID3D11Texture2D> cleanDesktop_;  // last desktop without a cursor on it
    bool lastDesktopValid_ = false;
    ComPtr<ID3D11RenderTargetView> rtv_;
    LUID luid_{};
    int width_ = 0, height_ = 0;
    bool frameHeld_ = false;

    // Reading from a hub instead of from Windows.
    std::shared_ptr<CaptureHub> hub_;
    ComPtr<ID3D11Texture2D> hubTexture_;
    ComPtr<IDXGIKeyedMutex> hubMutex_;
    uint64_t hubSeen_ = 0;

    // Cursor
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11Texture2D> cursorTex_;
    ComPtr<ID3D11ShaderResourceView> cursorSrv_;
    std::vector<uint8_t> shapeBuf_;
    int cursorW_ = 0, cursorH_ = 0;
    int cursorX_ = 0, cursorY_ = 0;
    bool cursorVisible_ = false;
};

// Converts and scales on the encoder's device, with hardware video processing.
class Nv12Converter {
public:
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int srcW, int srcH, int dstW, int dstH);
    bool convert(ID3D11Texture2D* bgra);
    ID3D11Texture2D* output() const { return nv12_.Get(); }

private:
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> enum_;
    ComPtr<ID3D11VideoProcessor> proc_;
    ComPtr<ID3D11Texture2D> nv12_;
    ComPtr<ID3D11VideoProcessorOutputView> outView_;
    ID3D11Device* dev_ = nullptr;
    ID3D11Texture2D* lastInput_ = nullptr;
    ComPtr<ID3D11VideoProcessorInputView> inView_;
};

// Converts and scales in software into memory, for when the picture is to stay away from any
// GPU: WARP cannot do it, and neither can a computer without a video processor. BT.709,
// limited range, the same as Nv12Converter.
class CpuNv12Converter {
public:
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int srcW, int srcH, int dstW, int dstH);
    bool convert(ID3D11Texture2D* bgra);
    const uint8_t* data() const { return out_.data(); }

private:
    ID3D11DeviceContext* ctx_ = nullptr;
    ComPtr<ID3D11Texture2D> staging_;
    int srcW_ = 0, srcH_ = 0, dstW_ = 0, dstH_ = 0;
    std::vector<int> mapX_, mapY_;
    std::vector<uint8_t> out_;
};

// Moves a texture between adapters through system memory, for hybrid graphics.
class CrossAdapterCopy {
public:
    bool init(ID3D11Device* srcDev, ID3D11DeviceContext* srcCtx, ID3D11Device* dstDev, ID3D11DeviceContext* dstCtx, int w, int h);
    ID3D11Texture2D* copy(ID3D11Texture2D* src);

private:
    ComPtr<ID3D11Texture2D> staging_, dst_;
    ID3D11DeviceContext* srcCtx_ = nullptr;
    ID3D11DeviceContext* dstCtx_ = nullptr;
};

}  // namespace sd
