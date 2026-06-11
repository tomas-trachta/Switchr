#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wincodec.h>

#include <string>
#include <unordered_map>

// The D3D11/DXGI/DComp/D2D/DWrite stack behind a DirectComposition window.
// Single-monitor sized; call Resize() before each show if the target monitor
// changed. The constructor throws std::runtime_error on failure.
class Renderer {
public:
    Renderer(HWND hwnd, int width, int height);

    void Resize(int width, int height);

    // Begin/EndDraw bracket one frame; EndDraw presents and commits.
    void BeginDraw();
    void EndDraw();

    void SetOpacity(float v);
    void AnimateOpacity(float from, float to, double seconds);  // server-side (DComp)

    // Cached D2D bitmap of the file's shell icon; nullptr if extraction
    // failed. Owned by Renderer.
    ID2D1Bitmap* GetExeIcon(const std::wstring& exePath);

    ID2D1DeviceContext* Ctx()    const { return d2dCtx_.Get(); }
    IDWriteFactory*     DWrite() const { return dwrite_.Get(); }
    int                 Width()  const { return width_;  }
    int                 Height() const { return height_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    void CreateDeviceResources();
    void CreateD3DDevice();
    void CreateCompositionTarget();
    void CreateD2D();
    void CreateTextAndImaging();
    void CreateSizeDependentResources();
    void ReleaseSizeDependentResources();

    ComPtr<ID2D1Bitmap> CreateIconBitmap(const std::wstring& exePath);

    HWND hwnd_ = nullptr;
    int  width_ = 0;
    int  height_ = 0;

    ComPtr<ID3D11Device>          d3d_;
    ComPtr<ID3D11DeviceContext>   d3dCtx_;
    ComPtr<IDXGIDevice>           dxgi_;
    ComPtr<IDXGIFactory2>         dxgiFactory_;
    ComPtr<IDXGISwapChain1>       swap_;
    ComPtr<IDCompositionDevice>     dcomp_;
    ComPtr<IDCompositionTarget>     dcompTarget_;
    ComPtr<IDCompositionVisual>     dcompVisual_;
    ComPtr<IDCompositionEffectGroup> opacityEffect_;
    ComPtr<ID2D1Factory1>         d2dFactory_;
    ComPtr<ID2D1Device>           d2dDevice_;
    ComPtr<ID2D1DeviceContext>    d2dCtx_;
    ComPtr<ID2D1Bitmap1>          target_;
    ComPtr<IDWriteFactory>        dwrite_;
    ComPtr<IWICImagingFactory>    wic_;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> iconCache_;
};
