#include "capture.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#include "log.h"

namespace sd {

namespace {

const char kShader[] = R"(
cbuffer Rect : register(b0) { float4 rect; };  // left, top, right, bottom in clip space
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut vsmain(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2(id & 1, id >> 1);
    o.uv = uv;
    o.pos = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.y, rect.w, uv.y), 0, 1);
    return o;
}
float4 psmain(VsOut i) : SV_Target { return tex.Sample(samp, i.uv); }
)";

bool sameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

}  // namespace

ComPtr<ID3D11Device> createDeviceOnAdapter(LUID luid, ComPtr<ID3D11DeviceContext>* ctx) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (!sameLuid(d.AdapterLuid, luid)) continue;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        ComPtr<ID3D11Device> dev;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                       D3D11_SDK_VERSION, &dev, nullptr, ctx ? ctx->ReleaseAndGetAddressOf() : nullptr);
        if (FAILED(hr)) {
            logf("D3D11CreateDevice failed: 0x%08lx", static_cast<unsigned long>(hr));
            return nullptr;
        }
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        return dev;
    }
    return nullptr;
}

bool findAdapterOfOutput(const std::wstring& deviceName, LUID* luid, std::wstring* desc, UINT* vendor) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC od;
            if (FAILED(output->GetDesc(&od)) || deviceName != od.DeviceName) continue;
            DXGI_ADAPTER_DESC1 ad;
            adapter->GetDesc1(&ad);
            if (luid) *luid = ad.AdapterLuid;
            if (desc) *desc = ad.Description;
            if (vendor) *vendor = ad.VendorId;
            return true;
        }
    }
    return false;
}

std::vector<std::pair<LUID, UINT>> listHardwareAdapters() {
    std::vector<std::pair<LUID, UINT>> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (d.VendorId == 0x1414) continue;  // Microsoft Basic Render
        out.emplace_back(d.AdapterLuid, d.VendorId);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return (a.second == 0x10DE) > (b.second == 0x10DE);
    });
    return out;
}

DesktopCapture::~DesktopCapture() {
    if (frameHeld_ && dup_) dup_->ReleaseFrame();
}

bool DesktopCapture::open(const std::wstring& deviceName) {
    UINT vendor = 0;
    std::wstring adapterDesc;
    if (!findAdapterOfOutput(deviceName, &luid_, &adapterDesc, &vendor)) {
        logf("output %s not found in DXGI", narrow(deviceName).c_str());
        return false;
    }
    device_ = createDeviceOnAdapter(luid_, &ctx_);
    if (!device_) return false;

    ComPtr<IDXGIDevice> dxgiDev;
    device_.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIOutput> output;
    for (UINT j = 0; adapter->EnumOutputs(j, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++j) {
        DXGI_OUTPUT_DESC od;
        output->GetDesc(&od);
        if (deviceName == od.DeviceName) {
            width_ = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
            height_ = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
            break;
        }
        output.Reset();
    }
    if (!output) return false;

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) return false;
    HRESULT hr = output1->DuplicateOutput(device_.Get(), &dup_);
    if (FAILED(hr)) {
        // Happens during desktop switches and when the limit of duplications is reached.
        logf("DuplicateOutput failed: 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }
    DXGI_OUTDUPL_DESC dd;
    dup_->GetDesc(&dd);
    width_ = dd.ModeDesc.Width;
    height_ = dd.ModeDesc.Height;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &composed_))) return false;
    if (FAILED(device_->CreateRenderTargetView(composed_.Get(), nullptr, &rtv_))) return false;
    if (!initCursorPipeline()) logf("cursor drawing unavailable");
    logf("capture opened: %s %dx%d on %s", narrow(deviceName).c_str(), width_, height_, narrow(adapterDesc).c_str());
    return true;
}

bool DesktopCapture::initCursorPipeline() {
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "vsmain", "vs_5_0", 0, 0, &vsb, &err)))
        return false;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "psmain", "ps_5_0", 0, 0, &psb, &err)))
        return false;
    if (FAILED(device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_))) return false;
    if (FAILED(device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) return false;

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, &blend_))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(device_->CreateBuffer(&cbd, nullptr, &cb_));
}

void DesktopCapture::updateCursorShape(const DXGI_OUTDUPL_FRAME_INFO& info) {
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        cursorVisible_ = info.PointerPosition.Visible != FALSE;
        cursorX_ = info.PointerPosition.Position.x;
        cursorY_ = info.PointerPosition.Position.y;
    }
    if (info.PointerShapeBufferSize == 0) return;

    shapeBuf_.resize(info.PointerShapeBufferSize);
    UINT needed = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
    if (FAILED(dup_->GetFramePointerShape(info.PointerShapeBufferSize, shapeBuf_.data(), &needed, &si))) return;

    const int w = si.Width;
    const int h = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? si.Height / 2 : si.Height;
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);

    switch (si.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        for (int y = 0; y < h; ++y)
            std::memcpy(&px[static_cast<size_t>(y) * w], shapeBuf_.data() + static_cast<size_t>(y) * si.Pitch, w * 4);
        break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        // Alpha 0 replaces the screen, alpha 255 XORs. True XOR needs the pixels
        // below, so inverting areas are shown as solid white, which stays visible.
        for (int y = 0; y < h; ++y) {
            auto row = reinterpret_cast<const uint32_t*>(shapeBuf_.data() + static_cast<size_t>(y) * si.Pitch);
            for (int x = 0; x < w; ++x) {
                uint32_t c = row[x];
                bool xorMode = (c >> 24) != 0;
                uint32_t rgb = c & 0x00FFFFFF;
                px[static_cast<size_t>(y) * w + x] = xorMode ? (rgb ? 0xFFFFFFFFu : 0u) : (0xFF000000u | rgb);
            }
        }
        break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        for (int y = 0; y < h; ++y) {
            const uint8_t* andRow = shapeBuf_.data() + static_cast<size_t>(y) * si.Pitch;
            const uint8_t* xorRow = shapeBuf_.data() + static_cast<size_t>(y + h) * si.Pitch;
            for (int x = 0; x < w; ++x) {
                bool a = (andRow[x / 8] >> (7 - x % 8)) & 1;
                bool b = (xorRow[x / 8] >> (7 - x % 8)) & 1;
                uint32_t c = 0;
                if (!a && !b) c = 0xFF000000u;
                else if (!a && b) c = 0xFFFFFFFFu;
                else if (a && b) c = 0xFFFFFFFFu;  // inverting
                px[static_cast<size_t>(y) * w + x] = c;
            }
        }
        break;
    default:
        return;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), static_cast<UINT>(w * 4), 0};
    cursorTex_.Reset();
    cursorSrv_.Reset();
    if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &cursorTex_)))
        device_->CreateShaderResourceView(cursorTex_.Get(), nullptr, &cursorSrv_);
    cursorW_ = w;
    cursorH_ = h;
}

void DesktopCapture::drawCursor() {
    if (!cursorVisible_ || !cursorSrv_ || !vs_ || !ps_ || !cb_) return;

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    auto r = static_cast<float*>(m.pData);
    r[0] = 2.0f * cursorX_ / width_ - 1.0f;
    r[1] = 1.0f - 2.0f * cursorY_ / height_;
    r[2] = 2.0f * (cursorX_ + cursorW_) / width_ - 1.0f;
    r[3] = 1.0f - 2.0f * (cursorY_ + cursorH_) / height_;
    ctx_->Unmap(cb_.Get(), 0);

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    const float factor[4] = {};
    ctx_->OMSetBlendState(blend_.Get(), factor, 0xFFFFFFFF);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, cursorSrv_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->Draw(4, 0);
    ID3D11RenderTargetView* none = nullptr;
    ctx_->OMSetRenderTargets(1, &none, nullptr);
}

CaptureResult DesktopCapture::acquire(int timeoutMs) {
    if (frameHeld_) {
        dup_->ReleaseFrame();
        frameHeld_ = false;
    }
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return CaptureResult::Timeout;
    if (FAILED(hr)) return CaptureResult::Lost;
    frameHeld_ = true;

    updateCursorShape(info);
    // A pure mouse move still needs a new picture, composed over the last desktop.
    if (info.LastPresentTime.QuadPart == 0 && info.LastMouseUpdateTime.QuadPart == 0)
        return CaptureResult::Timeout;

    if (info.LastPresentTime.QuadPart != 0) {
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(res.As(&tex))) return CaptureResult::Lost;
        ctx_->CopyResource(composed_.Get(), tex.Get());
        lastDesktopValid_ = true;
        // The clean desktop is kept so that an old cursor image is not left behind.
        if (!cleanDesktop_) {
            D3D11_TEXTURE2D_DESC td;
            composed_->GetDesc(&td);
            td.BindFlags = 0;
            device_->CreateTexture2D(&td, nullptr, &cleanDesktop_);
        }
        if (cleanDesktop_) ctx_->CopyResource(cleanDesktop_.Get(), tex.Get());
    } else {
        if (!lastDesktopValid_) return CaptureResult::Timeout;
        if (cleanDesktop_) ctx_->CopyResource(composed_.Get(), cleanDesktop_.Get());
    }
    drawCursor();
    return CaptureResult::NewFrame;
}

bool Nv12Converter::init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int srcW, int srcH, int dstW, int dstH) {
    dev_ = dev;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&vdev_)))) return false;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&vctx_)))) return false;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = srcW;
    cd.InputHeight = srcH;
    cd.OutputWidth = dstW;
    cd.OutputHeight = dstH;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(vdev_->CreateVideoProcessorEnumerator(&cd, &enum_))) return false;
    if (FAILED(vdev_->CreateVideoProcessor(enum_.Get(), 0, &proc_))) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = dstW;
    td.Height = dstH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &nv12_))) return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(vdev_->CreateVideoProcessorOutputView(nv12_.Get(), enum_.Get(), &ovd, &outView_))) return false;

    // Full-range RGB in, limited-range BT.709 out: what phone decoders assume for HD video.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
    in.RGB_Range = 0;
    in.YCbCr_Matrix = 1;
    out.RGB_Range = 0;
    out.YCbCr_Matrix = 1;
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vctx_->VideoProcessorSetStreamColorSpace(proc_.Get(), 0, &in);
    vctx_->VideoProcessorSetOutputColorSpace(proc_.Get(), &out);
    vctx_->VideoProcessorSetStreamFrameFormat(proc_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(proc_.Get(), 0, FALSE);
    return true;
}

bool Nv12Converter::convert(ID3D11Texture2D* bgra) {
    if (bgra != lastInput_) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inView_.Reset();
        if (FAILED(vdev_->CreateVideoProcessorInputView(bgra, enum_.Get(), &ivd, &inView_))) return false;
        lastInput_ = bgra;
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView_.Get();
    return SUCCEEDED(vctx_->VideoProcessorBlt(proc_.Get(), outView_.Get(), 0, 1, &stream));
}

bool CrossAdapterCopy::init(ID3D11Device* srcDev, ID3D11DeviceContext* srcCtx, ID3D11Device* dstDev,
                            ID3D11DeviceContext* dstCtx, int w, int h) {
    srcCtx_ = srcCtx;
    dstCtx_ = dstCtx;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(srcDev->CreateTexture2D(&td, nullptr, &staging_))) return false;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(dstDev->CreateTexture2D(&td, nullptr, &dst_));
}

ID3D11Texture2D* CrossAdapterCopy::copy(ID3D11Texture2D* src) {
    srcCtx_->CopyResource(staging_.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(srcCtx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return nullptr;
    dstCtx_->UpdateSubresource(dst_.Get(), 0, nullptr, m.pData, m.RowPitch, 0);
    srcCtx_->Unmap(staging_.Get(), 0);
    return dst_.Get();
}

}  // namespace sd
