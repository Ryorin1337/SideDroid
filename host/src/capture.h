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

enum class CaptureResult { NewFrame, Timeout, Lost };

class DesktopCapture {
public:
    ~DesktopCapture();
    bool open(const std::wstring& deviceName);
    // On NewFrame the composed picture (with cursor) is in texture().
    CaptureResult acquire(int timeoutMs);
    ID3D11Texture2D* texture() const { return composed_.Get(); }
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return ctx_.Get(); }
    LUID adapterLuid() const { return luid_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
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
