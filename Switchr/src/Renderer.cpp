// d2d1_1.h (pulled in by Renderer.h) includes d2d1effects.h itself, which
// would otherwise trip its include guard before INITGUID takes effect and
// leave the built-in effect CLSIDs as unresolved extern declarations.
#include <initguid.h>
#include <d2d1effects.h>

#include "Renderer.h"

#include <shellapi.h>
#include <cwctype>
#include <stdexcept>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")

namespace {

void ThrowIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(what);
}

// Grabs the given screen region into a top-down 32bpp DIB section. Returns
// the pixel buffer pointer (owned by `dib`, valid until it is deleted) or
// nullptr on failure.
void* CaptureScreenDib(int x, int y, int w, int h, HBITMAP& dib, HDC& memDC) {
    HDC screenDC = GetDC(nullptr);
    if (!screenDC) return nullptr;

    memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;  // negative = top-down, matches D2D's expected row order
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    dib = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        HGDIOBJ old = SelectObject(memDC, dib);
        BitBlt(memDC, 0, 0, w, h, screenDC, x, y, SRCCOPY);
        SelectObject(memDC, old);
    }

    ReleaseDC(nullptr, screenDC);
    return bits;
}

} // namespace

Renderer::Renderer(HWND hwnd, int width, int height)
    : hwnd_(hwnd), width_(width), height_(height) {
    CreateDeviceResources();
    CreateSizeDependentResources();
}

void Renderer::CreateDeviceResources() {
    CreateD3DDevice();
    CreateCompositionTarget();
    CreateD2D();
    CreateTextAndImaging();
}

void Renderer::CreateD3DDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
        &d3d_, &got, &d3dCtx_);

    if (FAILED(hr)) {
        // The debug layer is absent on clean Windows installs — retry without it.
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        ThrowIfFailed(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
            &d3d_, &got, &d3dCtx_), "D3D11CreateDevice");
    }

    ThrowIfFailed(d3d_.As(&dxgi_), "Query IDXGIDevice");

    ComPtr<IDXGIAdapter> adapter;
    ThrowIfFailed(dxgi_->GetAdapter(&adapter), "DXGIDevice GetAdapter");
    ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory_)), "DXGIAdapter GetParent");
}

void Renderer::CreateCompositionTarget() {
    ThrowIfFailed(DCompositionCreateDevice(dxgi_.Get(), IID_PPV_ARGS(&dcomp_)),
                  "DCompositionCreateDevice");
    ThrowIfFailed(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_),
                  "CreateTargetForHwnd");
    ThrowIfFailed(dcomp_->CreateVisual(&dcompVisual_), "DComp CreateVisual");
    ThrowIfFailed(dcompTarget_->SetRoot(dcompVisual_.Get()), "DComp SetRoot");

    // IDCompositionVisual has no SetOpacity — opacity (static and animated)
    // goes through an effect group attached to the visual.
    ThrowIfFailed(dcomp_->CreateEffectGroup(&opacityEffect_), "DComp CreateEffectGroup");
    opacityEffect_->SetOpacity(1.0f);
    ThrowIfFailed(dcompVisual_->SetEffect(opacityEffect_.Get()), "Visual SetEffect");
}

void Renderer::CreateD2D() {
    ThrowIfFailed(D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2dFactory_)),
        "D2D1CreateFactory");
    ThrowIfFailed(d2dFactory_->CreateDevice(dxgi_.Get(), &d2dDevice_), "D2D CreateDevice");
    ThrowIfFailed(d2dDevice_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx_), "D2D CreateDeviceContext");
}

void Renderer::CreateTextAndImaging() {
    ThrowIfFailed(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())),
        "DWriteCreateFactory");

    // WIC is optional — without it, icons just don't show.
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic_));
}

void Renderer::CreateSizeDependentResources() {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = width_;
    sd.Height           = height_;
    sd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED;
    ThrowIfFailed(dxgiFactory_->CreateSwapChainForComposition(
        dxgi_.Get(), &sd, nullptr, &swap_),
        "CreateSwapChainForComposition");

    ThrowIfFailed(dcompVisual_->SetContent(swap_.Get()), "DComp SetContent");
    ThrowIfFailed(dcomp_->Commit(), "DComp Commit (init)");

    ComPtr<IDXGISurface> back;
    ThrowIfFailed(swap_->GetBuffer(0, IID_PPV_ARGS(&back)), "SwapChain GetBuffer");

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ThrowIfFailed(d2dCtx_->CreateBitmapFromDxgiSurface(back.Get(), &bp, &target_),
                  "D2D CreateBitmapFromDxgiSurface");

    d2dCtx_->SetTarget(target_.Get());
}

void Renderer::ReleaseSizeDependentResources() {
    d2dCtx_->SetTarget(nullptr);
    target_.Reset();
}

void Renderer::Resize(int width, int height) {
    if (width == width_ && height == height_) return;

    width_  = width;
    height_ = height;
    ReleaseSizeDependentResources();

    if (!swap_) {
        CreateSizeDependentResources();
        return;
    }

    ThrowIfFailed(swap_->ResizeBuffers(
        0, width_, height_, DXGI_FORMAT_UNKNOWN, 0),
        "SwapChain ResizeBuffers");

    ComPtr<IDXGISurface> back;
    ThrowIfFailed(swap_->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer (resize)");

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ThrowIfFailed(d2dCtx_->CreateBitmapFromDxgiSurface(back.Get(), &bp, &target_),
                  "CreateBitmapFromDxgiSurface (resize)");
    d2dCtx_->SetTarget(target_.Get());
}

void Renderer::BeginDraw() {
    d2dCtx_->BeginDraw();
    d2dCtx_->SetTransform(D2D1::Matrix3x2F::Identity());
}

void Renderer::EndDraw() {
    HRESULT hr = d2dCtx_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // Lost device — surface the error; the caller can choose to recreate.
        throw std::runtime_error("D2DERR_RECREATE_TARGET");
    }
    ThrowIfFailed(hr, "EndDraw");

    ThrowIfFailed(swap_->Present(1, 0), "Present");
    ThrowIfFailed(dcomp_->Commit(), "DComp Commit (frame)");
}

void Renderer::SetOpacity(float v) {
    if (!opacityEffect_) return;

    opacityEffect_->SetOpacity(v);
    dcomp_->Commit();
}

void Renderer::AnimateOpacity(float from, float to, double seconds) {
    if (!opacityEffect_ || !dcomp_) return;

    ComPtr<IDCompositionAnimation> anim;
    if (FAILED(dcomp_->CreateAnimation(&anim)) || !anim) {
        opacityEffect_->SetOpacity(to);
        dcomp_->Commit();
        return;
    }

    // Linear ramp: f(t) = from + ((to - from) / seconds) * t, ending at `seconds`.
    double slope = (seconds > 0.0) ? ((to - from) / seconds) : 0.0;
    anim->AddCubic(0.0, from, (float)slope, 0.0f, 0.0f);
    anim->End(seconds, to);

    opacityEffect_->SetOpacity(anim.Get());
    dcomp_->Commit();
}

auto Renderer::CreateIconBitmap(const std::wstring& exePath) -> ComPtr<ID2D1Bitmap> {
    if (!wic_) return nullptr;

    SHFILEINFOW sfi{};
    DWORD_PTR ok = SHGetFileInfoW(exePath.c_str(), 0, &sfi, sizeof(sfi),
        SHGFI_ICON | SHGFI_LARGEICON);
    if (!ok || !sfi.hIcon) return nullptr;

    ComPtr<ID2D1Bitmap> result;
    ComPtr<IWICBitmap> wb;
    if (SUCCEEDED(wic_->CreateBitmapFromHICON(sfi.hIcon, &wb)) && wb) {
        ComPtr<IWICFormatConverter> fc;
        if (SUCCEEDED(wic_->CreateFormatConverter(&fc)) && fc &&
            SUCCEEDED(fc->Initialize(
                wb.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeMedianCut))) {
            d2dCtx_->CreateBitmapFromWicBitmap(fc.Get(), nullptr, &result);
        }
    }

    DestroyIcon(sfi.hIcon);
    return result;
}

ID2D1Bitmap* Renderer::GetExeIcon(const std::wstring& exePath) {
    if (exePath.empty()) return nullptr;

    std::wstring key = exePath;
    for (auto& c : key) c = (wchar_t)towlower(c);

    auto it = iconCache_.find(key);
    if (it != iconCache_.end()) return it->second.Get();

    // Failures are cached too, so missing icons aren't re-queried every frame.
    auto& slot = iconCache_[key];
    slot = CreateIconBitmap(exePath);
    return slot.Get();
}

void Renderer::CaptureBlurredBackdrop(int monitorX, int monitorY, float stdDeviation) {
    backdropBlur_.Reset();
    backdropBitmap_.Reset();

    HBITMAP dib = nullptr;
    HDC memDC = nullptr;
    void* bits = CaptureScreenDib(monitorX, monitorY, width_, height_, dib, memDC);

    if (bits) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        UINT32 pitch = (UINT32)width_ * 4;

        ComPtr<ID2D1Bitmap> bmp;
        if (SUCCEEDED(d2dCtx_->CreateBitmap(
                D2D1::SizeU((UINT32)width_, (UINT32)height_), bits, pitch, &bp, &bmp)) && bmp) {
            backdropBitmap_ = bmp;

            ComPtr<ID2D1Effect> blur;
            if (SUCCEEDED(d2dCtx_->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) && blur) {
                blur->SetInput(0, backdropBitmap_.Get());
                blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, stdDeviation);
                blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED);
                blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
                backdropBlur_ = blur;
            }
        }
    }

    if (dib) DeleteObject(dib);
    if (memDC) DeleteDC(memDC);
}

void Renderer::DrawBlurredBackdrop() {
    if (!backdropBlur_) return;
    d2dCtx_->DrawImage(backdropBlur_.Get());
}
