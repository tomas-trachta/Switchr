#include "NsOsd.h"
#include "Renderer.h"

#include <wrl/client.h>
#include <algorithm>

namespace {

template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

constexpr wchar_t kOsdClassName[] = L"SwitchrNsOsd";

constexpr float kOsdMinW     = 240.f;
constexpr float kOsdH        = 96.f;
constexpr float kOsdPadX     = 40.f;
constexpr float kOsdCorner   = 18.f;
constexpr float kOsdMargin   = 16.f;
constexpr UINT  kHoldTimerId = 1;
constexpr UINT  kFadeTimerId = 2;
constexpr UINT  kHoldMs      = 750;
constexpr UINT  kFadeMs      = 180;

D2D1_COLOR_F Premul(float r, float g, float b, float a) {
    return D2D1::ColorF(r * a, g * a, b * a, a);
}

float MeasureTextWidth(IDWriteFactory* dw, IDWriteTextFormat* fmt,
                       const std::wstring& text) {
    ComPtr<IDWriteTextLayout> layout;
    dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 10000.f, 100.f, &layout);
    if (!layout) return 0.f;

    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.width;
}

MONITORINFO ForegroundMonitorInfo() {
    HWND fg = GetForegroundWindow();
    HMONITOR mon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY)
                      : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    return mi;
}

} // namespace

struct NsOsd::State {
    std::unique_ptr<Renderer>     renderer;
    ComPtr<ID2D1SolidColorBrush>  bBg;
    ComPtr<ID2D1SolidColorBrush>  bText;
    ComPtr<ID2D1SolidColorBrush>  bTextDim;
    ComPtr<IDWriteTextFormat>     fmtName;
    ComPtr<IDWriteTextFormat>     fmtSub;
    float                         fmtScale = 0.f;
};

NsOsd::NsOsd() : s_(std::make_unique<State>()) {}

NsOsd::~NsOsd() {
    if (hwnd_) {
        KillTimer(hwnd_, kHoldTimerId);
        KillTimer(hwnd_, kFadeTimerId);
        DestroyWindow(hwnd_);
    }
}

std::unique_ptr<NsOsd> NsOsd::Create(HINSTANCE hInst) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = &NsOsd::StaticWndProc;
        wc.hInstance     = hInst;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kOsdClassName;
        if (!RegisterClassExW(&wc)) return nullptr;
        classRegistered = true;
    }

    auto osd = std::unique_ptr<NsOsd>(new NsOsd());

    // NOACTIVATE + TRANSPARENT: never steals focus, never eats clicks.
    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kOsdClassName, L"", WS_POPUP,
        0, 0, 400, 110,
        nullptr, nullptr, hInst,
        osd.get());
    if (!hwnd) return nullptr;
    osd->hwnd_ = hwnd;

    try {
        osd->s_->renderer = std::make_unique<Renderer>(hwnd, 400, 110);
    } catch (const std::exception&) {
        return nullptr;
    }
    return osd;
}

LRESULT CALLBACK NsOsd::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    NsOsd* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<NsOsd*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    self = reinterpret_cast<NsOsd*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    auto& s = *self->s_;

    switch (msg) {
        case WM_TIMER:
            if (wp == kHoldTimerId) {
                KillTimer(hwnd, kHoldTimerId);
                if (s.renderer) s.renderer->AnimateOpacity(1.f, 0.f, kFadeMs / 1000.0);
                SetTimer(hwnd, kFadeTimerId, kFadeMs + 40, nullptr);
            } else if (wp == kFadeTimerId) {
                KillTimer(hwnd, kFadeTimerId);
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;

        case WM_NCHITTEST:
            return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureOsdBrushes(NsOsd::State& s) {
    if (s.bBg) return;

    auto* ctx = s.renderer->Ctx();
    ctx->CreateSolidColorBrush(Premul(0.09f, 0.10f, 0.13f, 0.94f), &s.bBg);
    ctx->CreateSolidColorBrush(Premul(0.94f, 0.95f, 0.97f, 1.0f),  &s.bText);
    ctx->CreateSolidColorBrush(Premul(0.74f, 0.76f, 0.82f, 1.0f),  &s.bTextDim);
}

static void EnsureOsdFormats(NsOsd::State& s, float scale) {
    // Formats bake the font size in, so rebuild when DPI changes.
    if (s.fmtScale == scale) return;

    auto* dw = s.renderer->DWrite();
    auto mk = [&](ComPtr<IDWriteTextFormat>& out, float size, DWRITE_FONT_WEIGHT weight) {
        out.Reset();
        dw->CreateTextFormat(L"Segoe UI", nullptr,
            weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size * scale, L"", &out);
        if (out) {
            out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    };

    mk(s.fmtName, 26.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(s.fmtSub,  14.f, DWRITE_FONT_WEIGHT_REGULAR);
    s.fmtScale = scale;
}

static SIZE MeasureOsd(NsOsd::State& s, float scale,
                       const std::wstring& name, const std::wstring& sub) {
    auto* dw = s.renderer->DWrite();
    float nameW = MeasureTextWidth(dw, s.fmtName.Get(), name);
    float subW  = sub.empty() ? 0.f : MeasureTextWidth(dw, s.fmtSub.Get(), sub);

    int w = (int)(std::max({ kOsdMinW * scale, nameW, subW }) + 2.f * kOsdPadX * scale);
    int h = (int)(kOsdH * scale);
    return { w, h };
}

static void RenderOsd(NsOsd::State& s, float scale, SIZE size,
                      const std::wstring& name, const std::wstring& sub) {
    s.renderer->BeginDraw();
    auto* ctx = s.renderer->Ctx();
    float w = (float)size.cx;
    float h = (float)size.cy;

    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_ROUNDED_RECT rr{
        D2D1::RectF(0.f, 0.f, w, h), kOsdCorner * scale, kOsdCorner * scale };
    ctx->FillRoundedRectangle(rr, s.bBg.Get());

    if (sub.empty()) {
        D2D1_RECT_F nameRect{ 0.f, 0.f, w, h };
        ctx->DrawTextW(name.c_str(), (UINT32)name.size(), s.fmtName.Get(),
            nameRect, s.bText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else {
        D2D1_RECT_F nameRect{ 0.f, 8.f * scale, w, h * 0.62f };
        D2D1_RECT_F subRect { 0.f, h * 0.58f,   w, h - 10.f * scale };
        ctx->DrawTextW(name.c_str(), (UINT32)name.size(), s.fmtName.Get(),
            nameRect, s.bText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        ctx->DrawTextW(sub.c_str(), (UINT32)sub.size(), s.fmtSub.Get(),
            subRect, s.bTextDim.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    s.renderer->EndDraw();
}

static void PlaceTopRight(HWND hwnd, const MONITORINFO& mi, SIZE size, float scale) {
    int margin = (int)(kOsdMargin * scale);
    int x = mi.rcWork.right - size.cx - margin;
    int y = mi.rcWork.top + margin;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, size.cx, size.cy,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void RestartFadeCountdown(HWND hwnd) {
    KillTimer(hwnd, kHoldTimerId);
    KillTimer(hwnd, kFadeTimerId);
    SetTimer(hwnd, kHoldTimerId, kHoldMs, nullptr);
}

void NsOsd::Show(const std::wstring& name, const std::wstring& sub) {
    auto& s = *s_;
    if (!s.renderer) return;

    // Park on the target monitor first so GetDpiForWindow reports its DPI.
    MONITORINFO mi = ForegroundMonitorInfo();
    SetWindowPos(hwnd_, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;

    EnsureOsdBrushes(s);
    EnsureOsdFormats(s, scale);

    SIZE size = MeasureOsd(s, scale, name, sub);
    s.renderer->Resize(size.cx, size.cy);
    RenderOsd(s, scale, size, name, sub);

    PlaceTopRight(hwnd_, mi, size, scale);
    s.renderer->SetOpacity(1.f);
    RestartFadeCountdown(hwnd_);
}
