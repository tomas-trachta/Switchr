#include "Overlay.h"
#include "Renderer.h"
#include "WindowList.h"
#include "MruTracker.h"
#include "Namespaces.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

namespace {

template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

constexpr wchar_t kClassName[] = L"SwitchrOverlay";

// Layout constants (DIPs, scaled by DPI at use).
constexpr float kTileW          = 280.f;   // reference width for the thumb aspect ratio
constexpr float kTileThumbH     = 158.f;
constexpr float kTileTitleH     = 56.f;
constexpr float kTileGap        = 20.f;
constexpr float kTileCorner     = 12.f;
constexpr float kPreferredTileW = 440.f;
constexpr float kMaxTileW       = 600.f;
constexpr float kSearchW        = 760.f;
constexpr float kSearchH        = 56.f;
constexpr float kSearchCorner   = 14.f;
constexpr float kSearchPadX     = 22.f;
constexpr float kListRowH       = 60.f;
constexpr float kListW          = 760.f;
constexpr int   kMaxTileCols    = 8;
constexpr int   kMaxListRows    = 10;
constexpr float kIconSizeTile   = 22.f;
constexpr float kIconSizeList   = 32.f;
constexpr float kIconPadX       = 12.f;
constexpr float kScrollStepPx   = 80.f;
constexpr float kScrollbarW     = 8.f;
constexpr float kScrollbarMarginX  = 10.f;
constexpr float kScrollbarMinThumb = 40.f;
constexpr float kScrollbarCorner   = 4.f;
constexpr double kFadeInSec     = 0.05;
constexpr UINT  kCaretTimerId   = 0x517A;
constexpr UINT  kCaretBlinkMs   = 530;
constexpr float kNsBarH         = 38.f;
constexpr float kNsPillGap      = 10.f;
constexpr float kNsPillPadX     = 18.f;
constexpr UINT  kNsStatusMs     = 2500;

// Order matches the `action` parameter of DoNsFileAction.
constexpr const wchar_t* kNsBtnLabels[3] = { L"Save", L"Load", L"Load apps" };

D2D1_COLOR_F Premul(float r, float g, float b, float a) {
    return D2D1::ColorF(r * a, g * a, b * a, a);
}

bool IsPrintableChar(wchar_t c) {
    return c >= 0x20 && c != 0x7F;
}

bool PointIn(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::vector<std::wstring> SplitTerms(const std::wstring& q) {
    std::vector<std::wstring> out;
    std::wstring cur;

    for (wchar_t c : q) {
        if (iswspace(c)) {
            if (!cur.empty()) { out.push_back(ToLower(cur)); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(ToLower(cur));

    return out;
}

// -1 = no match.
int ScoreEntry(const WindowEntry& w, const std::vector<std::wstring>& terms) {
    if (terms.empty()) return 0;

    std::wstring title = ToLower(w.title);
    std::wstring exe   = ToLower(w.exe);

    int score = 0;
    for (const auto& t : terms) {
        size_t titlePos = title.find(t);
        size_t exePos   = exe.find(t);
        if (titlePos == std::wstring::npos && exePos == std::wstring::npos) return -1;

        int termScore = 0;
        if (titlePos == 0) termScore += 100;
        else if (titlePos != std::wstring::npos) termScore += 60 - std::min<int>(50, (int)titlePos);
        if (exePos == 0) termScore += 30;
        else if (exePos != std::wstring::npos) termScore += 15;
        score += termScore;
    }

    score += std::max(0, 40 - (int)title.size() / 4);
    return score;
}

size_t WordLeft(const std::wstring& t, size_t p) {
    while (p > 0 && iswspace(t[p - 1])) --p;
    while (p > 0 && !iswspace(t[p - 1])) --p;
    return p;
}

size_t WordRight(const std::wstring& t, size_t p) {
    size_t n = t.size();
    while (p < n && !iswspace(t[p])) ++p;
    while (p < n &&  iswspace(t[p])) ++p;
    return p;
}

size_t WordStartAt(const std::wstring& t, size_t p) {
    while (p > 0 && !iswspace(t[p - 1])) --p;
    return p;
}

size_t WordEndAt(const std::wstring& t, size_t p) {
    while (p < t.size() && !iswspace(t[p])) ++p;
    return p;
}

std::wstring ReadClipboardText(HWND owner) {
    if (!OpenClipboard(owner)) return {};

    std::wstring out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out.assign(p);
            GlobalUnlock(h);
        }
    }

    CloseClipboard();
    return out;
}

void WriteClipboardText(HWND owner, const std::wstring& s) {
    if (s.empty()) return;
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();

    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        if (auto* p = static_cast<wchar_t*>(GlobalLock(h))) {
            memcpy(p, s.c_str(), bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }

    CloseClipboard();
}

} // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Overlay::State {
    std::unique_ptr<Renderer> renderer;

    std::vector<WindowEntry> all;
    std::vector<int>         filtered;  // indices into `all`

    std::wstring query;
    size_t       caret  = 0;
    size_t       anchor = 0;            // == caret means no selection
    bool         draggingSel = false;

    bool nsNaming       = false;        // search box doubles as namespace-name input
    int  nsRenameTarget = -1;           // namespace being renamed; -1 = creating

    enum class Mode { Tiles, List };
    Mode mode = Mode::Tiles;

    int   selected = 0;                 // index into `filtered`
    int   hovered  = -1;
    bool  mouseTracking    = false;
    bool  draggingScroll   = false;
    float scrollDragOffset = 0.f;       // y-offset within the thumb at drag start
    bool  scrollbarHover   = false;

    struct TileRect {
        D2D1_RECT_F outer;
        D2D1_RECT_F thumb;
        D2D1_RECT_F title;
    };
    std::vector<TileRect>    tileRects;
    std::vector<HTHUMBNAIL>  thumbs;
    std::vector<D2D1_RECT_F> listRects;

    std::vector<D2D1_RECT_F> nsRects;   // pills, indexed like Ns namespaces
    D2D1_RECT_F  nsBtnRects[3]{};
    bool         nsBtnsValid   = false;
    int          nsPillHovered = -1;
    int          nsBtnHovered  = -1;
    std::wstring statusText;
    ULONGLONG    statusUntil = 0;

    D2D1_RECT_F searchRect{};

    int   monitorW = 0;
    int   monitorH = 0;
    UINT  dpi      = 96;
    float scale    = 1.f;

    float viewportTop = 0.f;
    float viewportBot = 0.f;
    float scrollY     = 0.f;
    float maxScroll   = 0.f;

    ComPtr<ID2D1SolidColorBrush>   bBackdrop;
    ComPtr<ID2D1SolidColorBrush>   bTile;
    ComPtr<ID2D1SolidColorBrush>   bTileSel;
    ComPtr<ID2D1SolidColorBrush>   bAccent;
    ComPtr<ID2D1SolidColorBrush>   bText;
    ComPtr<ID2D1SolidColorBrush>   bTextDim;
    ComPtr<ID2D1SolidColorBrush>   bSearch;
    ComPtr<ID2D1SolidColorBrush>   bSearchBorder;
    ComPtr<ID2D1SolidColorBrush>   bCaret;
    ComPtr<ID2D1SolidColorBrush>   bListSel;
    ComPtr<ID2D1SolidColorBrush>   bTileHover;
    ComPtr<ID2D1SolidColorBrush>   bListHover;
    ComPtr<ID2D1SolidColorBrush>   bSelection;
    ComPtr<ID2D1SolidColorBrush>   bScrollTrack;
    ComPtr<ID2D1SolidColorBrush>   bScrollThumb;
    ComPtr<ID2D1SolidColorBrush>   bScrollThumbHover;
    bool                           brushesValid = false;

    ComPtr<IDWriteTextFormat>      fmtSearch;
    ComPtr<IDWriteTextFormat>      fmtSearchPlaceholder;
    ComPtr<IDWriteTextFormat>      fmtTitle;
    ComPtr<IDWriteTextFormat>      fmtMuted;
    ComPtr<IDWriteTextFormat>      fmtTileTitle;
    bool                           formatsValid = false;

    bool     visible    = false;
    bool     caretOn    = true;
    UINT_PTR caretTimer = 0;
};

using Mode = Overlay::State::Mode;

// ---------------------------------------------------------------------------
// Construction / window registration
// ---------------------------------------------------------------------------

Overlay::Overlay() : s_(std::make_unique<State>()) {}

Overlay::~Overlay() {
    if (s_) {
        if (s_->caretTimer) KillTimer(hwnd_, s_->caretTimer);
        for (auto h : s_->thumbs) if (h) DwmUnregisterThumbnail(h);
    }
    if (hwnd_) DestroyWindow(hwnd_);
}

std::unique_ptr<Overlay> Overlay::Create(HINSTANCE hInst) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc   = &Overlay::StaticWndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) {
            throw std::runtime_error("RegisterClassExW failed");
        }
        classRegistered = true;
    }

    auto overlay = std::unique_ptr<Overlay>(new Overlay());

    // WS_EX_NOREDIRECTIONBITMAP must be set at creation — DComp owns the visuals.
    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"Switchr",
        WS_POPUP,
        0, 0, 800, 600,
        nullptr, nullptr, hInst,
        overlay.get());
    if (!hwnd) throw std::runtime_error("CreateWindowExW failed");
    overlay->hwnd_ = hwnd;

    overlay->s_->dpi   = GetDpiForWindow(hwnd);
    overlay->s_->scale = overlay->s_->dpi / 96.0f;
    overlay->s_->renderer = std::make_unique<Renderer>(hwnd, 800, 600);

    return overlay;
}

LRESULT CALLBACK Overlay::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<Overlay*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) return self->WndProc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Forward declarations for the impl
// ---------------------------------------------------------------------------

static void EnsureBrushes(Overlay::State& s, Renderer& r);
static void EnsureFormats(Overlay::State& s, Renderer& r);
static void ApplyFilter(Overlay::State& s);
static void LayoutTiles(Overlay::State& s);
static void LayoutList(Overlay::State& s);
static void LayoutNsBar(Overlay::State& s, Renderer& r);
static void RegisterThumbnails(Overlay::State& s, HWND overlayHwnd);
static void UpdateThumbnailLayout(Overlay::State& s);
static void UnregisterThumbnails(Overlay::State& s);
static void Render(Overlay::State& s, Renderer& r);
static void ActivateSelected(Overlay::State& s, HWND overlayHwnd);
static void NavigateTile(Overlay::State& s, int dx, int dy);
static void EnsureSelectedVisible(Overlay::State& s);
static void ClampScroll(Overlay::State& s);
static void RefreshWindows(Overlay::State& s, HWND hwnd);
static void SwitchToNamespace(Overlay::State& s, HWND hwnd, int target);
static void MoveSelectedToNamespace(Overlay::State& s, HWND hwnd, int target);
static void CommitNamespaceName(Overlay::State& s, HWND hwnd);
static void SetStatus(Overlay::State& s, std::wstring text);
static void DoNsFileAction(Overlay::State& s, HWND hwnd, int action);

static void RenderNow(Overlay::State& s) {
    if (s.renderer) Render(s, *s.renderer);
}

// ---------------------------------------------------------------------------
// Query buffer editing
// ---------------------------------------------------------------------------

static bool TextEditMode(const Overlay::State& s) {
    return s.nsNaming || s.mode == Mode::List;
}

static bool   HasSel(const Overlay::State& s) { return s.caret != s.anchor; }
static size_t SelMin(const Overlay::State& s) { return std::min(s.caret, s.anchor); }
static size_t SelMax(const Overlay::State& s) { return std::max(s.caret, s.anchor); }

static void PlaceCaret(Overlay::State& s, size_t pos, bool keepAnchor) {
    s.caret = pos;
    if (!keepAnchor) s.anchor = pos;
    s.caretOn = true;
}

static void DeleteSelection(Overlay::State& s) {
    if (!HasSel(s)) return;

    size_t a = SelMin(s), b = SelMax(s);
    s.query.erase(s.query.begin() + a, s.query.begin() + b);
    s.caret  = a;
    s.anchor = a;
}

static void InsertText(Overlay::State& s, std::wstring text) {
    for (auto& c : text) if (c < 0x20 || c == 0x7F) c = L' ';

    if (HasSel(s)) DeleteSelection(s);
    s.query.insert(s.caret, text);
    s.caret  += text.size();
    s.anchor  = s.caret;
}

static void CancelNaming(Overlay::State& s) {
    s.nsNaming       = false;
    s.nsRenameTarget = -1;
    s.query.clear();
    s.caret  = 0;
    s.anchor = 0;
}

// Prefilled and fully selected so typing replaces the old name.
static void BeginRename(Overlay::State& s, int idx) {
    s.nsNaming       = true;
    s.nsRenameTarget = idx;
    s.query   = Ns::Name(idx);
    s.anchor  = 0;
    s.caret   = s.query.size();
    s.caretOn = true;
}

static void UpdateAfterQueryChange(Overlay::State& s, HWND hwnd) {
    if (s.caret  > s.query.size()) s.caret  = s.query.size();
    if (s.anchor > s.query.size()) s.anchor = s.query.size();

    // Namespace-name input reuses the query buffer but never filters windows
    // or switches between Tiles/List.
    if (s.nsNaming) return;

    Mode prevMode = s.mode;
    s.mode = s.query.empty() ? Mode::Tiles : Mode::List;
    if (prevMode != s.mode) {
        s.selected = 0;
        if (s.mode == Mode::Tiles) s.scrollY = 0.f;
    }

    ApplyFilter(s);

    if (s.mode == Mode::Tiles) {
        LayoutTiles(s);
        if (prevMode != s.mode) RegisterThumbnails(s, hwnd);
        else                    UpdateThumbnailLayout(s);
    } else {
        if (prevMode != s.mode) UnregisterThumbnails(s);
        LayoutList(s);
    }
}

// ---------------------------------------------------------------------------
// Geometry + hit-testing
// ---------------------------------------------------------------------------

static float SearchTop(const Overlay::State& s) {
    return s.monitorH * 0.18f;
}

static D2D1_RECT_F SearchBoxRect(const Overlay::State& s) {
    float w = kSearchW * s.scale;
    float h = kSearchH * s.scale;
    float x = (s.monitorW - w) * 0.5f;
    float y = SearchTop(s);
    return D2D1::RectF(x, y, x + w, y + h);
}

static float NsBarTop(const Overlay::State& s) {
    return SearchTop(s) + (kSearchH + 16.f) * s.scale;
}

static float ContentTop(const Overlay::State& s) {
    float searchBottom = SearchTop(s) + kSearchH * s.scale;
    if (!Ns::Enabled()) return searchBottom + 24.f * s.scale;
    return NsBarTop(s) + (kNsBarH + 18.f) * s.scale;
}

struct ScrollbarRects {
    bool        present = false;
    D2D1_RECT_F track   {};
    D2D1_RECT_F thumb   {};
};

// `present` is false when the grid fits without scrolling.
static ScrollbarRects ComputeScrollbar(const Overlay::State& s) {
    ScrollbarRects out;
    if (s.maxScroll <= 0.5f) return out;

    float trackH = s.viewportBot - s.viewportTop;
    if (trackH <= 0.f) return out;

    float w = kScrollbarW * s.scale;
    float x = s.monitorW - kScrollbarMarginX * s.scale - w;
    out.track = D2D1::RectF(x, s.viewportTop, x + w, s.viewportBot);

    float contentH = trackH + s.maxScroll;
    float thumbH   = std::max(kScrollbarMinThumb * s.scale, trackH * trackH / contentH);
    if (thumbH > trackH) thumbH = trackH;
    float thumbY   = s.viewportTop + (s.scrollY / s.maxScroll) * (trackH - thumbH);
    out.thumb      = D2D1::RectF(x, thumbY, x + w, thumbY + thumbH);

    out.present = true;
    return out;
}

static int HitTestItem(const Overlay::State& s, int x, int y) {
    if (s.mode == Mode::Tiles) {
        if (y < (int)s.viewportTop || y > (int)s.viewportBot) return -1;

        float yv = (float)y + s.scrollY;
        for (int i = 0; i < (int)s.tileRects.size(); ++i) {
            if (PointIn(s.tileRects[i].outer, (float)x, yv)) return i;
        }
    } else {
        for (int i = 0; i < (int)s.listRects.size(); ++i) {
            if (PointIn(s.listRects[i], (float)x, (float)y)) return i;
        }
    }
    return -1;
}

static int HitTestNsPills(const Overlay::State& s, int x, int y) {
    for (int i = 0; i < (int)s.nsRects.size(); ++i) {
        if (PointIn(s.nsRects[i], (float)x, (float)y)) return i;
    }
    return -1;
}

static int HitTestNsButtons(const Overlay::State& s, int x, int y) {
    if (!s.nsBtnsValid) return -1;

    for (int i = 0; i < 3; ++i) {
        if (PointIn(s.nsBtnRects[i], (float)x, (float)y)) return i;
    }
    return -1;
}

static size_t HitTestSearchText(Overlay::State& s, IDWriteFactory* dw, float mouseX) {
    if (s.query.empty()) return 0;

    float padX    = kSearchPadX * s.scale;
    float layoutX = mouseX - (s.searchRect.left + padX);
    if (layoutX <= 0.f) return 0;

    float layoutW = (kSearchW - 2.f * kSearchPadX) * s.scale;
    float layoutH = kSearchH * s.scale;
    ComPtr<IDWriteTextLayout> layout;
    dw->CreateTextLayout(s.query.c_str(), (UINT32)s.query.size(),
        s.fmtSearch.Get(), layoutW, layoutH, &layout);
    if (!layout) return s.query.size();

    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS hm{};
    if (FAILED(layout->HitTestPoint(layoutX, layoutH * 0.5f, &trailing, &inside, &hm)))
        return s.query.size();

    size_t pos = (size_t)hm.textPosition + (trailing ? 1u : 0u);
    return std::min(pos, s.query.size());
}

// ---------------------------------------------------------------------------
// Show / Hide / Toggle
// ---------------------------------------------------------------------------

static RECT MonitorRectForForeground() {
    HWND fg = GetForegroundWindow();
    HMONITOR mon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY)
                      : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    return mi.rcMonitor;
}

static void ResetInteraction(Overlay::State& s) {
    s.query.clear();
    s.caret  = 0;
    s.anchor = 0;

    s.nsNaming       = false;
    s.nsRenameTarget = -1;
    s.draggingSel    = false;
    s.draggingScroll = false;
    s.scrollbarHover = false;

    s.mode          = Mode::Tiles;
    s.selected      = 0;
    s.hovered       = -1;
    s.nsPillHovered = -1;
    s.nsBtnHovered  = -1;
    s.scrollY       = 0.f;
    s.caretOn       = true;
}

static void RestartCaretBlink(Overlay::State& s, HWND hwnd) {
    if (s.caretTimer) KillTimer(hwnd, s.caretTimer);
    s.caretTimer = SetTimer(hwnd, kCaretTimerId, kCaretBlinkMs, nullptr);
}

static void StopCaretBlink(Overlay::State& s, HWND hwnd) {
    if (s.caretTimer) { KillTimer(hwnd, s.caretTimer); s.caretTimer = 0; }
}

static void ReleaseDrags(Overlay::State& s, HWND hwnd) {
    if (!s.draggingSel && !s.draggingScroll) return;

    s.draggingSel    = false;
    s.draggingScroll = false;
    if (GetCapture() == hwnd) ReleaseCapture();
}

// A namespace switch can leave the old foreground window hidden; park focus
// on the namespace's MRU-top window so keystrokes go somewhere.
static void ParkFocusInNamespace(const Overlay::State& s, HWND overlayHwnd) {
    if (!Ns::Enabled()) return;

    HWND fg = GetForegroundWindow();
    if (fg && fg != overlayHwnd && IsWindowVisible(fg)) return;

    for (const auto& w : s.all) {
        if (IsWindow(w.hwnd) && IsWindowVisible(w.hwnd)) {
            ForceForeground(w.hwnd);
            return;
        }
    }
}

bool Overlay::IsVisible() const { return s_->visible; }

void Overlay::Toggle() { if (s_->visible) Hide(); else Show(); }

void Overlay::Show() {
    auto& s = *s_;

    RECT mon = MonitorRectForForeground();
    s.monitorW = mon.right - mon.left;
    s.monitorH = mon.bottom - mon.top;
    s.dpi      = GetDpiForWindow(hwnd_);
    s.scale    = s.dpi / 96.0f;

    s.renderer->Resize(s.monitorW, s.monitorH);
    s.renderer->SetOpacity(0.f);  // SWP_SHOWWINDOW must not flash the previous frame
    SetWindowPos(hwnd_, HWND_TOPMOST, mon.left, mon.top, s.monitorW, s.monitorH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    ResetInteraction(s);
    s.searchRect = SearchBoxRect(s);

    Ns::SuspendHotkeys();
    RefreshWindows(s, hwnd_);

    s.visible = true;
    ForceForeground(hwnd_);
    RestartCaretBlink(s, hwnd_);

    Render(s, *s.renderer);
    s.renderer->AnimateOpacity(0.f, 1.f, kFadeInSec);
}

void Overlay::Hide() {
    auto& s = *s_;
    if (!s.visible) return;

    ReleaseDrags(s, hwnd_);
    StopCaretBlink(s, hwnd_);
    UnregisterThumbnails(s);

    ShowWindow(hwnd_, SW_HIDE);
    s.visible = false;
    Ns::ResumeHotkeys();

    ParkFocusInNamespace(s, hwnd_);
}

// ---------------------------------------------------------------------------
// Brushes + text formats
// ---------------------------------------------------------------------------

static void EnsureBrushes(Overlay::State& s, Renderer& r) {
    if (s.brushesValid) return;

    auto* ctx = r.Ctx();
    auto mk = [&](ComPtr<ID2D1SolidColorBrush>& b, D2D1_COLOR_F c) {
        ctx->CreateSolidColorBrush(c, &b);
    };

    mk(s.bBackdrop,     Premul(0.03f, 0.04f, 0.07f, 0.84f));
    mk(s.bTile,         Premul(0.13f, 0.14f, 0.18f, 0.94f));
    mk(s.bTileHover,    Premul(0.19f, 0.21f, 0.27f, 0.96f));
    mk(s.bTileSel,      Premul(0.20f, 0.27f, 0.42f, 0.98f));
    mk(s.bAccent,       Premul(0.46f, 0.70f, 1.00f, 1.0f));
    mk(s.bText,         Premul(0.94f, 0.95f, 0.97f, 1.0f));
    mk(s.bTextDim,      Premul(0.74f, 0.76f, 0.82f, 1.0f));
    mk(s.bSearch,       Premul(0.09f, 0.10f, 0.13f, 0.96f));
    mk(s.bSearchBorder, Premul(0.34f, 0.38f, 0.48f, 0.85f));
    mk(s.bCaret,        Premul(0.94f, 0.95f, 0.97f, 1.0f));
    mk(s.bListSel,      Premul(0.22f, 0.34f, 0.58f, 0.92f));
    mk(s.bListHover,    Premul(0.18f, 0.20f, 0.27f, 0.94f));
    mk(s.bSelection,    Premul(0.46f, 0.70f, 1.00f, 0.42f));
    mk(s.bScrollTrack,      Premul(0.30f, 0.32f, 0.38f, 0.28f));
    mk(s.bScrollThumb,      Premul(0.72f, 0.76f, 0.84f, 0.60f));
    mk(s.bScrollThumbHover, Premul(0.88f, 0.91f, 0.97f, 0.85f));

    s.brushesValid = true;
}

static void EnsureFormats(Overlay::State& s, Renderer& r) {
    if (s.formatsValid) return;

    auto* dw = r.DWrite();
    auto mk = [&](ComPtr<IDWriteTextFormat>& out, float size,
                  DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_REGULAR) {
        dw->CreateTextFormat(L"Segoe UI", nullptr,
            weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"", &out);
    };

    mk(s.fmtSearch,            26.f);
    mk(s.fmtSearchPlaceholder, 26.f);
    mk(s.fmtTitle,             18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(s.fmtMuted,             14.f);
    mk(s.fmtTileTitle,         15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    s.fmtSearch->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s.fmtSearchPlaceholder->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s.fmtTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s.fmtMuted->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s.fmtTileTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s.fmtTileTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    s.formatsValid = true;
}

// ---------------------------------------------------------------------------
// Filtering + layout
// ---------------------------------------------------------------------------

static void ApplyFilter(Overlay::State& s) {
    auto terms = SplitTerms(s.query);
    s.filtered.clear();

    if (terms.empty()) {
        for (int i = 0; i < (int)s.all.size(); ++i) s.filtered.push_back(i);
    } else {
        struct Scored { int idx; int score; uint64_t mru; };
        std::vector<Scored> hits;
        for (int i = 0; i < (int)s.all.size(); ++i) {
            int sc = ScoreEntry(s.all[i], terms);
            if (sc >= 0) hits.push_back({ i, sc, Mru::Rank(s.all[i].hwnd) });
        }

        std::sort(hits.begin(), hits.end(),
            [](const Scored& a, const Scored& b) {
                if (a.score != b.score) return a.score > b.score;
                return a.mru > b.mru;
            });

        for (auto& h : hits) s.filtered.push_back(h.idx);
        if ((int)s.filtered.size() > kMaxListRows) s.filtered.resize(kMaxListRows);
    }

    if (s.selected >= (int)s.filtered.size()) s.selected = std::max(0, (int)s.filtered.size() - 1);
}

static void ClampScroll(Overlay::State& s) {
    if (s.scrollY < 0.f) s.scrollY = 0.f;
    if (s.scrollY > s.maxScroll) s.scrollY = s.maxScroll;
}

static int PickColumnCount(const Overlay::State& s, float availW, float gap, int items) {
    int cols = (int)((availW + gap) / (kPreferredTileW * s.scale + gap));
    cols = std::clamp(cols, 1, kMaxTileCols);
    return std::min(cols, items);
}

static void LayoutTiles(Overlay::State& s) {
    s.tileRects.clear();
    s.maxScroll   = 0.f;
    s.viewportTop = ContentTop(s);
    s.viewportBot = s.monitorH - 40.f * s.scale;

    int n = (int)s.filtered.size();
    if (n == 0) return;

    float scale       = s.scale;
    float gap         = kTileGap * scale;
    float thumbInset  = 8.f * scale;
    float titleStripH = (kTileTitleH - 16.f) * scale;
    float availW      = s.monitorW * 0.96f;

    int cols = PickColumnCount(s, availW, gap, n);
    float tw = (availW - (cols - 1) * gap) / cols;
    if (tw > kMaxTileW * scale) tw = kMaxTileW * scale;

    // Keep the reference thumb aspect ratio (~264:158) as tiles stretch.
    constexpr float kThumbHOverInnerW = kTileThumbH / (kTileW - 16.f);
    float thumbVisualH = (tw - 2.f * thumbInset) * kThumbHOverInnerW;
    float th = 2.f * thumbInset + thumbVisualH + titleStripH;

    int rows = (n + cols - 1) / cols;
    float gridW   = cols * tw + (cols - 1) * gap;
    float originX = (s.monitorW - gridW) * 0.5f;
    float originY = s.viewportTop;

    for (int i = 0; i < n; ++i) {
        int r = i / cols, c = i % cols;
        float x = originX + c * (tw + gap);
        float y = originY + r * (th + gap);

        Overlay::State::TileRect tr;
        tr.outer = D2D1::RectF(x, y, x + tw, y + th);
        tr.thumb = D2D1::RectF(
            x + thumbInset, y + thumbInset,
            x + tw - thumbInset, y + thumbInset + thumbVisualH);
        tr.title = D2D1::RectF(
            x + thumbInset, y + thumbInset + thumbVisualH,
            x + tw - thumbInset, y + th - thumbInset);
        s.tileRects.push_back(tr);
    }

    float gridH = rows * th + (rows - 1) * gap;
    float viewH = s.viewportBot - s.viewportTop;
    s.maxScroll = std::max(0.f, gridH - viewH);
    ClampScroll(s);
}

static void EnsureSelectedVisible(Overlay::State& s) {
    if (s.mode != Mode::Tiles) return;
    if (s.selected < 0 || s.selected >= (int)s.tileRects.size()) return;

    const auto& tr = s.tileRects[s.selected];
    float top = tr.outer.top    - s.scrollY;
    float bot = tr.outer.bottom - s.scrollY;
    if (top < s.viewportTop)      s.scrollY -= (s.viewportTop - top);
    else if (bot > s.viewportBot) s.scrollY += (bot - s.viewportBot);

    ClampScroll(s);
}

static void LayoutList(Overlay::State& s) {
    s.listRects.clear();

    int n = (int)s.filtered.size();
    if (n == 0) return;

    float scale = s.scale;
    float w  = kListW * scale;
    float h  = kListRowH * scale;
    float x  = (s.monitorW - w) * 0.5f;
    float y0 = ContentTop(s);

    for (int i = 0; i < n; ++i) {
        float y = y0 + i * (h + 6.f * scale);
        s.listRects.push_back(D2D1::RectF(x, y, x + w, y + h));
    }
}

// ---------------------------------------------------------------------------
// Thumbnails
// ---------------------------------------------------------------------------

static void RegisterThumbnails(Overlay::State& s, HWND overlayHwnd) {
    UnregisterThumbnails(s);
    if (s.mode != Mode::Tiles) return;

    int n = (int)s.filtered.size();
    s.thumbs.assign(n, nullptr);
    for (int i = 0; i < n; ++i) {
        HWND src = s.all[s.filtered[i]].hwnd;
        HTHUMBNAIL th = nullptr;
        if (FAILED(DwmRegisterThumbnail(overlayHwnd, src, &th)) || !th) continue;
        s.thumbs[i] = th;
    }

    UpdateThumbnailLayout(s);
}

// Thumbs whose tile is not fully inside the viewport are hidden — DWM doesn't
// clip destination rects, so a half-scrolled thumb would overlap the search box.
static void UpdateThumbnailLayout(Overlay::State& s) {
    int n = std::min((int)s.thumbs.size(), (int)s.tileRects.size());
    for (int i = 0; i < n; ++i) {
        if (!s.thumbs[i]) continue;

        const auto& tr = s.tileRects[i];
        RECT shifted{
            (LONG)tr.thumb.left,
            (LONG)(tr.thumb.top    - s.scrollY),
            (LONG)tr.thumb.right,
            (LONG)(tr.thumb.bottom - s.scrollY),
        };
        bool fullyVisible = ((float)shifted.top    >= s.viewportTop) &&
                            ((float)shifted.bottom <= s.viewportBot);

        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE |
                    DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination         = shifted;
        p.opacity               = 255;
        p.fVisible              = fullyVisible ? TRUE : FALSE;
        p.fSourceClientAreaOnly = TRUE;
        DwmUpdateThumbnailProperties(s.thumbs[i], &p);
    }
}

static void UnregisterThumbnails(Overlay::State& s) {
    for (auto h : s.thumbs) if (h) DwmUnregisterThumbnail(h);
    s.thumbs.clear();
}

// ---------------------------------------------------------------------------
// Namespaces
// ---------------------------------------------------------------------------

static float MeasureTextWidth(IDWriteFactory* dw, IDWriteTextFormat* fmt,
                              const std::wstring& text) {
    ComPtr<IDWriteTextLayout> layout;
    dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 10000.f, 100.f, &layout);
    if (!layout) return 0.f;

    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.width;
}

static std::wstring NsPillLabel(int idx) {
    return Ns::Name(idx) + L"  ·  " + std::to_wstring(Ns::WindowCount(idx));
}

static void LayoutNsBar(Overlay::State& s, Renderer& r) {
    s.nsRects.clear();
    s.nsBtnsValid = false;
    if (!Ns::Enabled()) return;

    EnsureFormats(s, r);
    float scale = s.scale;
    float h     = kNsBarH * scale;
    float gap   = kNsPillGap * scale;
    float padX  = kNsPillPadX * scale;
    float y     = NsBarTop(s);

    int count = Ns::Count();
    std::vector<float> widths(count);
    float total = 0.f;
    for (int i = 0; i < count; ++i) {
        widths[i] = MeasureTextWidth(r.DWrite(), s.fmtTileTitle.Get(), NsPillLabel(i))
                  + 2.f * padX;
        total += widths[i];
    }
    total += gap * (count - 1);

    float x = (s.monitorW - total) * 0.5f;
    for (int i = 0; i < count; ++i) {
        s.nsRects.push_back(D2D1::RectF(x, y, x + widths[i], y + h));
        x += widths[i] + gap;
    }

    float bx = s.monitorW - 24.f * scale;
    for (int i = 2; i >= 0; --i) {
        float bw = MeasureTextWidth(r.DWrite(), s.fmtMuted.Get(), kNsBtnLabels[i])
                 + 2.f * 14.f * scale;
        s.nsBtnRects[i] = D2D1::RectF(bx - bw, y, bx, y + h);
        bx -= bw + 8.f * scale;
    }
    s.nsBtnsValid = true;
}

static void RefreshWindows(Overlay::State& s, HWND hwnd) {
    s.all = EnumerateAltTabWindows(hwnd);

    // Safety net: a visible-but-untracked window belongs to the active
    // namespace (the show hook can miss windows that gain a title late).
    if (Ns::Enabled()) {
        for (const auto& w : s.all) {
            if (Ns::Of(w.hwnd) < 0) Ns::Assign(w.hwnd, Ns::Active());
        }
    }

    std::sort(s.all.begin(), s.all.end(),
        [](const WindowEntry& a, const WindowEntry& b) {
            return Mru::Rank(a.hwnd) > Mru::Rank(b.hwnd);
        });

    ApplyFilter(s);
    if (s.mode == Mode::Tiles) {
        LayoutTiles(s);
        RegisterThumbnails(s, hwnd);
    } else {
        LayoutList(s);
    }

    if (s.renderer) LayoutNsBar(s, *s.renderer);
}

static void ResetView(Overlay::State& s) {
    s.selected = 0;
    s.hovered  = -1;
    s.scrollY  = 0.f;
}

static void SwitchToNamespace(Overlay::State& s, HWND hwnd, int target) {
    if (target == Ns::Active() || target < 0 || target >= Ns::Count()) return;

    Ns::SwitchTo(target);
    ResetView(s);
    RefreshWindows(s, hwnd);
}

static void MoveSelectedToNamespace(Overlay::State& s, HWND hwnd, int target) {
    if (target == Ns::Active() || target < 0 || target >= Ns::Count()) return;
    if (s.selected < 0 || s.selected >= (int)s.filtered.size()) return;

    Ns::Assign(s.all[s.filtered[s.selected]].hwnd, target);
    RefreshWindows(s, hwnd);
}

static void SetStatus(Overlay::State& s, std::wstring text) {
    s.statusText  = std::move(text);
    s.statusUntil = GetTickCount64() + kNsStatusMs;
}

// action: 0 = save, 1 = load structure + assign, 2 = load apps. Caller renders.
static void DoNsFileAction(Overlay::State& s, HWND hwnd, int action) {
    if (!Ns::Enabled()) return;

    if (action == 0) {
        SetStatus(s, Ns::SaveToFile() ? L"Namespaces saved" : L"Save failed");
        return;
    }

    if (action == 1) {
        if (!Ns::LoadStructure(true)) {
            SetStatus(s, L"Nothing saved yet");
            return;
        }

        ResetView(s);
        RefreshWindows(s, hwnd);
        SetStatus(s, L"Namespaces loaded, running apps assigned — "
                     L"“Load apps” launches the missing ones");
        return;
    }

    int launched = Ns::LoadApps();
    if (launched < 0) {
        SetStatus(s, L"Nothing saved yet");
        return;
    }

    ResetView(s);
    RefreshWindows(s, hwnd);
    SetStatus(s, launched == 0
        ? L"All saved apps already open — re-assigned to their namespaces"
        : L"Launching " + std::to_wstring(launched) + L" app(s)…");
}

static std::wstring TrimmedQuery(const Overlay::State& s) {
    size_t b = s.query.find_first_not_of(L" \t");
    size_t e = s.query.find_last_not_of(L" \t");
    return (b == std::wstring::npos) ? L"" : s.query.substr(b, e - b + 1);
}

static void CommitNamespaceName(Overlay::State& s, HWND hwnd) {
    std::wstring name = TrimmedQuery(s);
    int target = s.nsRenameTarget;
    CancelNaming(s);

    if (target >= 0) {
        // An empty name just cancels the rename.
        if (!name.empty()) Ns::Rename(target, name);
    } else {
        if (name.empty()) name = L"Namespace " + std::to_wstring(Ns::Count() + 1);
        Ns::Create(name);
    }

    RefreshWindows(s, hwnd);
}

static bool DeleteActiveNamespace(Overlay::State& s, HWND hwnd) {
    if (Ns::Count() <= 1) {
        SetStatus(s, L"Can’t delete the last namespace");
        return true;
    }

    std::wstring old = Ns::Name(Ns::Active());
    Ns::Delete(Ns::Active());
    ResetView(s);
    RefreshWindows(s, hwnd);

    SetStatus(s, L"Deleted “" + old + L"” — its windows moved to “" +
                 Ns::Name(Ns::Active()) + L"”");
    return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void DrawTextSimple(ID2D1DeviceContext* ctx, IDWriteTextFormat* fmt,
                           const std::wstring& text, const D2D1_RECT_F& rect,
                           ID2D1Brush* brush) {
    if (text.empty()) return;
    ctx->DrawTextW(text.c_str(), (UINT32)text.size(), fmt, rect, brush,
                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static const wchar_t* SearchPlaceholder(const Overlay::State& s) {
    if (!s.nsNaming) return L"Type to search windows…";
    return s.nsRenameTarget >= 0 ? L"Rename namespace…" : L"New namespace name…";
}

static void RenderSearchSelection(Overlay::State& s, ID2D1DeviceContext* ctx,
                                  IDWriteTextLayout* layout,
                                  float originX, float originY) {
    size_t lo = SelMin(s), hi = SelMax(s);

    UINT32 count = 0;
    layout->HitTestTextRange((UINT32)lo, (UINT32)(hi - lo),
        originX, originY, nullptr, 0, &count);
    if (count == 0) return;

    std::vector<DWRITE_HIT_TEST_METRICS> hms(count);
    UINT32 actual = 0;
    if (FAILED(layout->HitTestTextRange((UINT32)lo, (UINT32)(hi - lo),
            originX, originY, hms.data(), count, &actual))) {
        return;
    }

    for (UINT32 i = 0; i < actual; ++i) {
        const auto& hm = hms[i];
        D2D1_RECT_F selRect{ hm.left, hm.top, hm.left + hm.width, hm.top + hm.height };
        ctx->FillRectangle(selRect, s.bSelection.Get());
    }
}

static void RenderSearchCaret(Overlay::State& s, ID2D1DeviceContext* ctx,
                              IDWriteTextLayout* layout,
                              float originX, float y, float h) {
    float caretLogicalX = 0.f, caretLogicalY = 0.f;
    DWRITE_HIT_TEST_METRICS hm{};
    if (FAILED(layout->HitTestTextPosition(
            (UINT32)s.caret, FALSE, &caretLogicalX, &caretLogicalY, &hm))) {
        return;
    }

    float caretH = 28.f * s.scale;
    float caretY = y + (h - caretH) * 0.5f;
    float caretX = originX + caretLogicalX;
    D2D1_RECT_F caretRect{ caretX, caretY, caretX + 2.f * s.scale, caretY + caretH };
    ctx->FillRectangle(caretRect, s.bCaret.Get());
}

static void RenderSearchBox(Overlay::State& s, Renderer& r) {
    auto* ctx = r.Ctx();
    s.searchRect = SearchBoxRect(s);
    float scale = s.scale;
    float x = s.searchRect.left;
    float y = s.searchRect.top;
    float w = s.searchRect.right - x;
    float h = s.searchRect.bottom - y;

    D2D1_ROUNDED_RECT rr{ s.searchRect, kSearchCorner * scale, kSearchCorner * scale };
    ctx->FillRoundedRectangle(rr, s.bSearch.Get());
    ctx->DrawRoundedRectangle(rr, s.bSearchBorder.Get(), 1.0f * scale);

    float padX    = kSearchPadX * scale;
    float originX = x + padX;
    float layoutW = w - 2.f * padX;

    if (s.query.empty()) {
        D2D1_RECT_F textRect{ originX, y, x + w - padX, y + h };
        DrawTextSimple(ctx, s.fmtSearchPlaceholder.Get(), SearchPlaceholder(s),
            textRect, s.bTextDim.Get());
        return;
    }

    // One layout drives selection highlight, text drawing, and caret position.
    ComPtr<IDWriteTextLayout> layout;
    r.DWrite()->CreateTextLayout(s.query.c_str(), (UINT32)s.query.size(),
        s.fmtSearch.Get(), layoutW, h, &layout);

    if (!layout) {
        D2D1_RECT_F textRect{ originX, y, x + w - padX, y + h };
        DrawTextSimple(ctx, s.fmtSearch.Get(), s.query, textRect, s.bText.Get());
        return;
    }

    if (HasSel(s)) RenderSearchSelection(s, ctx, layout.Get(), originX, y);

    ctx->DrawTextLayout(D2D1::Point2F(originX, y), layout.Get(),
        s.bText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

    if (s.caretOn && !HasSel(s)) {
        RenderSearchCaret(s, ctx, layout.Get(), originX, y, h);
    }
}

static void RenderNsBar(Overlay::State& s, Renderer& r) {
    if (!Ns::Enabled() || s.nsRects.empty()) return;

    auto* ctx = r.Ctx();
    float corner = kNsBarH * 0.5f * s.scale;

    s.fmtTileTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    for (int i = 0; i < (int)s.nsRects.size(); ++i) {
        const auto& rect = s.nsRects[i];
        bool active = (i == Ns::Active());
        bool hover  = (i == s.nsPillHovered);

        D2D1_ROUNDED_RECT rr{ rect, corner, corner };
        ctx->FillRoundedRectangle(rr,
            active ? s.bTileSel.Get() : hover ? s.bTileHover.Get() : s.bTile.Get());
        if (active) ctx->DrawRoundedRectangle(rr, s.bAccent.Get(), 2.f * s.scale);
        DrawTextSimple(ctx, s.fmtTileTitle.Get(), NsPillLabel(i), rect,
                       (active || hover) ? s.bText.Get() : s.bTextDim.Get());
    }

    if (!s.nsBtnsValid) return;

    s.fmtMuted->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    for (int i = 0; i < 3; ++i) {
        const auto& rect = s.nsBtnRects[i];
        bool hover = (i == s.nsBtnHovered);

        D2D1_ROUNDED_RECT rr{ rect, 8.f * s.scale, 8.f * s.scale };
        ctx->FillRoundedRectangle(rr, hover ? s.bTileHover.Get() : s.bTile.Get());
        ctx->DrawRoundedRectangle(rr,
            hover ? s.bAccent.Get() : s.bSearchBorder.Get(), 1.f * s.scale);
        DrawTextSimple(ctx, s.fmtMuted.Get(), kNsBtnLabels[i], rect,
                       hover ? s.bText.Get() : s.bTextDim.Get());
    }
}

static std::wstring CurrentHint(const Overlay::State& s, bool isStatus) {
    if (isStatus) return s.statusText;

    if (s.nsNaming) {
        return s.nsRenameTarget >= 0
            ? L"Enter — rename namespace   ·   Esc — cancel"
            : L"Enter — create namespace   ·   Esc — cancel";
    }

    return L"Ctrl+←/→ switch (works anywhere)   ·   Ctrl+N new   ·   "
           L"Ctrl+R rename   ·   Ctrl+D delete   ·   "
           L"Ctrl+Shift+←/→ or Ctrl+Shift+1–9 move window   ·   "
           L"Ctrl+S save   ·   Ctrl+L load   ·   Ctrl+Shift+L load apps   ·   "
           L"Ctrl+M exit";
}

static void RenderNsHints(Overlay::State& s, Renderer& r) {
    if (!Ns::Enabled()) return;

    bool isStatus = GetTickCount64() < s.statusUntil;
    s.fmtMuted->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    D2D1_RECT_F rect{ 0.f, s.monitorH - 36.f * s.scale,
                      (float)s.monitorW, s.monitorH - 8.f * s.scale };
    DrawTextSimple(r.Ctx(), s.fmtMuted.Get(), CurrentHint(s, isStatus), rect,
                   isStatus ? s.bText.Get() : s.bTextDim.Get());
}

static void RenderEmptyNamespaceNote(Overlay::State& s, ID2D1DeviceContext* ctx) {
    s.fmtTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    D2D1_RECT_F rect = D2D1::RectF(0.f, s.viewportTop, (float)s.monitorW,
                                   s.viewportTop + 80.f * s.scale);
    DrawTextSimple(ctx, s.fmtTitle.Get(),
        L"This namespace is empty — apps you open now will join it",
        rect, s.bTextDim.Get());
}

static void RenderTile(Overlay::State& s, Renderer& r, int i) {
    auto* ctx = r.Ctx();
    const auto& tr = s.tileRects[i];

    float top = tr.outer.top    - s.scrollY;
    float bot = tr.outer.bottom - s.scrollY;
    if (bot < s.viewportTop || top > s.viewportBot) return;

    bool sel   = (i == s.selected);
    bool hover = (i == s.hovered);
    D2D1_ROUNDED_RECT rr{ tr.outer, kTileCorner * s.scale, kTileCorner * s.scale };
    ID2D1Brush* fill = sel   ? s.bTileSel.Get()
                     : hover ? s.bTileHover.Get()
                             : s.bTile.Get();
    ctx->FillRoundedRectangle(rr, fill);
    if (sel) ctx->DrawRoundedRectangle(rr, s.bAccent.Get(), 3.0f * s.scale);

    const auto& w = s.all[s.filtered[i]];
    std::wstring title = w.title.empty() ? w.exe : w.title;
    float iconSz = kIconSizeTile * s.scale;
    float pad    = kIconPadX * s.scale;
    float hintW  = (i < 9) ? 56.f * s.scale : 0.f;  // room for the Ctrl+N quick-pick hint

    float titleX = tr.title.left;
    ID2D1Bitmap* icon = r.GetExeIcon(w.exePath);
    if (icon) {
        float cy = (tr.title.top + tr.title.bottom) * 0.5f;
        D2D1_RECT_F iconRect{
            tr.title.left + pad, cy - iconSz * 0.5f,
            tr.title.left + pad + iconSz, cy + iconSz * 0.5f };
        ctx->DrawBitmap(icon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        titleX = iconRect.right + 8.f * s.scale;
    } else {
        titleX += pad;
    }

    D2D1_RECT_F textRect{ titleX, tr.title.top,
                          tr.title.right - pad - hintW, tr.title.bottom };
    s.fmtTileTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawTextSimple(ctx, s.fmtTileTitle.Get(), title, textRect, s.bText.Get());

    if (i < 9) {
        std::wstring hint = L"Ctrl+" + std::to_wstring(i + 1);
        D2D1_RECT_F hintRect{ tr.title.right - pad - hintW, tr.title.top,
                              tr.title.right - pad,         tr.title.bottom };
        s.fmtMuted->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        DrawTextSimple(ctx, s.fmtMuted.Get(), hint, hintRect, s.bTextDim.Get());
    }
}

static void RenderScrollbar(Overlay::State& s, ID2D1DeviceContext* ctx) {
    ScrollbarRects sb = ComputeScrollbar(s);
    if (!sb.present) return;

    D2D1_ROUNDED_RECT trackRR{ sb.track, kScrollbarCorner * s.scale,
                                          kScrollbarCorner * s.scale };
    ctx->FillRoundedRectangle(trackRR, s.bScrollTrack.Get());

    ID2D1Brush* thumbBrush = (s.scrollbarHover || s.draggingScroll)
        ? s.bScrollThumbHover.Get() : s.bScrollThumb.Get();
    D2D1_ROUNDED_RECT thumbRR{ sb.thumb, kScrollbarCorner * s.scale,
                                          kScrollbarCorner * s.scale };
    ctx->FillRoundedRectangle(thumbRR, thumbBrush);
}

static void RenderTiles(Overlay::State& s, Renderer& r) {
    int n = (int)s.filtered.size();
    if (n == 0) {
        if (Ns::Enabled()) RenderEmptyNamespaceNote(s, r.Ctx());
        return;
    }

    auto* ctx = r.Ctx();
    D2D1_RECT_F viewport = D2D1::RectF(0.f, s.viewportTop, (float)s.monitorW, s.viewportBot);
    ctx->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(0.f, -s.scrollY));

    for (int i = 0; i < n; ++i) RenderTile(s, r, i);

    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->PopAxisAlignedClip();

    RenderScrollbar(s, ctx);
}

static void RenderListRow(Overlay::State& s, Renderer& r, int i) {
    auto* ctx = r.Ctx();
    const auto& rect = s.listRects[i];

    bool sel   = (i == s.selected);
    bool hover = (i == s.hovered);
    D2D1_ROUNDED_RECT rr{ rect, 8.f * s.scale, 8.f * s.scale };
    ID2D1Brush* fill = sel   ? s.bListSel.Get()
                     : hover ? s.bListHover.Get()
                             : s.bTile.Get();
    ctx->FillRoundedRectangle(rr, fill);

    if (sel) {
        ctx->DrawRoundedRectangle(rr, s.bAccent.Get(), 2.0f * s.scale);

        float stripeInset = 10.f * s.scale;
        float stripeW     = 3.f  * s.scale;
        D2D1_RECT_F stripe{
            rect.left + 6.f * s.scale, rect.top + stripeInset,
            rect.left + 6.f * s.scale + stripeW, rect.bottom - stripeInset };
        D2D1_ROUNDED_RECT srr{ stripe, stripeW * 0.5f, stripeW * 0.5f };
        ctx->FillRoundedRectangle(srr, s.bAccent.Get());
    }

    const auto& w = s.all[s.filtered[i]];
    float iconSz = kIconSizeList * s.scale;
    float pad    = 18.f * s.scale;

    float textLeft = rect.left + pad;
    ID2D1Bitmap* icon = r.GetExeIcon(w.exePath);
    if (icon) {
        float cy = (rect.top + rect.bottom) * 0.5f;
        D2D1_RECT_F iconRect{ rect.left + pad, cy - iconSz * 0.5f,
                              rect.left + pad + iconSz, cy + iconSz * 0.5f };
        ctx->DrawBitmap(icon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        textLeft = iconRect.right + 12.f * s.scale;
    }

    D2D1_RECT_F textRect{ textLeft, rect.top, rect.right - pad, rect.bottom };
    s.fmtTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    s.fmtMuted->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawTextSimple(ctx, s.fmtTitle.Get(), w.title, textRect, s.bText.Get());
    DrawTextSimple(ctx, s.fmtMuted.Get(), w.exe, textRect, s.bTextDim.Get());
}

static void RenderList(Overlay::State& s, Renderer& r) {
    int n = (int)s.filtered.size();
    if (n == 0) {
        D2D1_RECT_F rect = D2D1::RectF(0.f, s.monitorH * 0.30f,
                                       (float)s.monitorW, s.monitorH * 0.40f);
        DrawTextSimple(r.Ctx(), s.fmtTitle.Get(), L"No matches", rect, s.bTextDim.Get());
        return;
    }

    for (int i = 0; i < n; ++i) RenderListRow(s, r, i);
}

static void Render(Overlay::State& s, Renderer& r) {
    EnsureBrushes(s, r);
    EnsureFormats(s, r);

    r.BeginDraw();
    auto* ctx = r.Ctx();
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
    ctx->FillRectangle(
        D2D1::RectF(0.f, 0.f, (float)s.monitorW, (float)s.monitorH),
        s.bBackdrop.Get());

    RenderSearchBox(s, r);
    RenderNsBar(s, r);
    if (s.mode == Mode::Tiles) RenderTiles(s, r);
    else                       RenderList(s, r);
    RenderNsHints(s, r);

    r.EndDraw();
}

// ---------------------------------------------------------------------------
// Activation + navigation
// ---------------------------------------------------------------------------

static void ActivateSelected(Overlay::State& s, HWND overlayHwnd) {
    if (s.selected < 0 || s.selected >= (int)s.filtered.size()) return;

    HWND target = s.all[s.filtered[s.selected]].hwnd;
    UnregisterThumbnails(s);  // before yielding foreground — cleaner DWM teardown
    StopCaretBlink(s, overlayHwnd);

    ShowWindow(overlayHwnd, SW_HIDE);
    s.visible = false;
    Ns::ResumeHotkeys();
    ForceForeground(target);
}

static int LayoutColumnCount(const Overlay::State& s) {
    if (s.tileRects.empty()) return 1;

    float firstRowY = s.tileRects[0].outer.top;
    int cols = 0;
    for (const auto& tr : s.tileRects) {
        if (std::abs(tr.outer.top - firstRowY) >= 1.f) break;
        ++cols;
    }
    return std::max(cols, 1);
}

static void NavigateTile(Overlay::State& s, int dx, int dy) {
    int n = (int)s.filtered.size();
    if (n == 0 || s.tileRects.empty()) return;

    int cols = LayoutColumnCount(s);
    int rows = (n + cols - 1) / cols;
    int col  = s.selected % cols + dx;
    int row  = s.selected / cols + dy;

    if (col < 0)     col = cols - 1;
    if (col >= cols) col = 0;
    if (row < 0)     row = rows - 1;
    if (row >= rows) row = 0;

    int target = row * cols + col;
    if (target >= n) target = n - 1;  // last row may be partial
    s.selected = target;

    EnsureSelectedVisible(s);
    UpdateThumbnailLayout(s);
}

// ---------------------------------------------------------------------------
// Keyboard input — each handler returns true when a re-render is needed
// ---------------------------------------------------------------------------

static bool NsTilesIdle(const Overlay::State& s) {
    return Ns::Enabled() && !s.nsNaming && s.mode == Mode::Tiles;
}

static bool SelectAllQuery(Overlay::State& s) {
    if (s.query.empty()) return false;

    s.anchor  = 0;
    s.caret   = s.query.size();
    s.caretOn = true;
    return true;
}

static bool CopySelection(Overlay::State& s, HWND hwnd) {
    if (HasSel(s)) {
        WriteClipboardText(hwnd, s.query.substr(SelMin(s), SelMax(s) - SelMin(s)));
    }
    return false;
}

static bool CutSelection(Overlay::State& s, HWND hwnd) {
    if (!HasSel(s)) return false;

    WriteClipboardText(hwnd, s.query.substr(SelMin(s), SelMax(s) - SelMin(s)));
    DeleteSelection(s);
    UpdateAfterQueryChange(s, hwnd);
    s.caretOn = true;
    return true;
}

static bool PasteIntoQuery(Overlay::State& s, HWND hwnd) {
    std::wstring text = ReadClipboardText(hwnd);
    if (text.empty()) return false;

    InsertText(s, std::move(text));
    UpdateAfterQueryChange(s, hwnd);
    s.caretOn = true;
    return true;
}

static bool ToggleNamespacesMode(Overlay::State& s, HWND hwnd) {
    if (s.nsNaming) return false;

    if (Ns::Enabled()) Ns::Disable();
    else               Ns::Enable();

    ResetView(s);
    RefreshWindows(s, hwnd);
    return true;
}

static bool BeginCreateNamespace(Overlay::State& s) {
    s.nsNaming = true;
    s.query.clear();
    s.caret   = 0;
    s.anchor  = 0;
    s.caretOn = true;
    return true;
}

static bool CaretWordJump(Overlay::State& s, int dir, bool shift) {
    size_t p = dir < 0 ? WordLeft(s.query, s.caret) : WordRight(s.query, s.caret);
    PlaceCaret(s, p, shift);
    return true;
}

static bool CtrlArrow(Overlay::State& s, HWND hwnd, int dir, bool alt, bool shift) {
    if (TextEditMode(s)) return CaretWordJump(s, dir, shift);
    if (!alt && !shift) return false;
    if (!Ns::Enabled() || Ns::Count() <= 1) return false;

    int target = (Ns::Active() + dir + Ns::Count()) % Ns::Count();
    if (shift) MoveSelectedToNamespace(s, hwnd, target);
    else       SwitchToNamespace(s, hwnd, target);
    return true;
}

static bool DeleteWord(Overlay::State& s, HWND hwnd, int dir) {
    if (!TextEditMode(s)) return false;

    if (HasSel(s)) {
        DeleteSelection(s);
    } else if (dir < 0) {
        size_t p = WordLeft(s.query, s.caret);
        s.query.erase(p, s.caret - p);
        s.caret  = p;
        s.anchor = p;
    } else {
        size_t p = WordRight(s.query, s.caret);
        if (p > s.caret) s.query.erase(s.caret, p - s.caret);
    }

    UpdateAfterQueryChange(s, hwnd);
    s.caretOn = true;
    return true;
}

static bool CaretToEdge(Overlay::State& s, bool toEnd, bool shift) {
    PlaceCaret(s, toEnd ? s.query.size() : 0, shift);
    return true;
}

static bool HandleCtrlDigit(Overlay::State& s, HWND hwnd, int idx, bool shift) {
    if (s.nsNaming) return false;

    if (shift) {
        if (!Ns::Enabled() || s.mode != Mode::Tiles) return false;
        MoveSelectedToNamespace(s, hwnd, idx);
        return true;
    }

    if (idx < (int)s.filtered.size()) {
        s.selected = idx;
        ActivateSelected(s, hwnd);
    }
    return false;
}

static bool HandleCtrlKey(Overlay::State& s, HWND hwnd, WPARAM wp, bool alt, bool shift) {
    switch (wp) {
        case 'A': return SelectAllQuery(s);
        case 'C': return CopySelection(s, hwnd);
        case 'X': return CutSelection(s, hwnd);
        case 'V': return PasteIntoQuery(s, hwnd);

        case 'M': return ToggleNamespacesMode(s, hwnd);
        case 'N': return NsTilesIdle(s) && BeginCreateNamespace(s);
        case 'R':
            if (!NsTilesIdle(s)) return false;
            BeginRename(s, Ns::Active());
            return true;
        case 'D': return NsTilesIdle(s) && DeleteActiveNamespace(s, hwnd);
        case 'S':
            if (!NsTilesIdle(s)) return false;
            DoNsFileAction(s, hwnd, 0);
            return true;
        case 'L':
            if (!NsTilesIdle(s)) return false;
            DoNsFileAction(s, hwnd, shift ? 2 : 1);
            return true;

        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            return HandleCtrlDigit(s, hwnd, (int)wp - (int)'1', shift);

        case VK_LEFT:   return CtrlArrow(s, hwnd, -1, alt, shift);
        case VK_RIGHT:  return CtrlArrow(s, hwnd, +1, alt, shift);
        case VK_BACK:   return DeleteWord(s, hwnd, -1);
        case VK_DELETE: return DeleteWord(s, hwnd, +1);
        case VK_HOME:   return TextEditMode(s) && CaretToEdge(s, false, shift);
        case VK_END:    return TextEditMode(s) && CaretToEdge(s, true,  shift);
    }
    return false;
}

static bool EraseChar(Overlay::State& s, HWND hwnd, int dir) {
    if (!TextEditMode(s)) return false;

    if (HasSel(s)) {
        DeleteSelection(s);
    } else if (dir < 0 && s.caret > 0) {
        s.query.erase(s.query.begin() + (s.caret - 1));
        --s.caret;
        s.anchor = s.caret;
    } else if (dir > 0 && s.caret < s.query.size()) {
        s.query.erase(s.query.begin() + s.caret);
    } else {
        return false;
    }

    UpdateAfterQueryChange(s, hwnd);
    s.caretOn = true;
    return true;
}

static bool CaretStep(Overlay::State& s, int dir, bool shift) {
    size_t target;
    if (!shift && HasSel(s)) target = dir < 0 ? SelMin(s) : SelMax(s);
    else if (dir < 0)        target = s.caret > 0 ? s.caret - 1 : 0;
    else                     target = std::min(s.caret + 1, s.query.size());

    PlaceCaret(s, target, shift);
    return true;
}

static bool HorizontalNav(Overlay::State& s, int dir, bool shift) {
    if (TextEditMode(s)) return CaretStep(s, dir, shift);

    NavigateTile(s, dir, 0);
    return true;
}

static bool VerticalNav(Overlay::State& s, int dir) {
    if (s.mode == Mode::Tiles) {
        NavigateTile(s, 0, dir);
    } else if (dir < 0 && s.selected > 0) {
        --s.selected;
    } else if (dir > 0 && s.selected + 1 < (int)s.filtered.size()) {
        ++s.selected;
    }
    return true;
}

static bool HomeEndKey(Overlay::State& s, bool toEnd, bool shift) {
    if (TextEditMode(s)) {
        CaretToEdge(s, toEnd, shift);
    } else if (!toEnd) {
        s.selected = 0;
    } else if (!s.filtered.empty()) {
        s.selected = (int)s.filtered.size() - 1;
    }
    return true;
}

static bool CycleSelection(Overlay::State& s, bool backwards) {
    int n = (int)s.filtered.size();
    if (n == 0) return false;

    int step = backwards ? -1 : 1;
    s.selected = (s.selected + step + n) % n;
    if (s.mode == Mode::Tiles) {
        EnsureSelectedVisible(s);
        UpdateThumbnailLayout(s);
    }
    return true;
}

static bool ScrollByPage(Overlay::State& s, int dir) {
    if (s.mode != Mode::Tiles || s.maxScroll <= 0.f) return false;

    s.scrollY += dir * (s.viewportBot - s.viewportTop) * 0.85f;
    ClampScroll(s);
    UpdateThumbnailLayout(s);
    return true;
}

static bool HandlePlainKey(Overlay::State& s, HWND hwnd, WPARAM wp, bool shift) {
    switch (wp) {
        case VK_BACK:   return EraseChar(s, hwnd, -1);
        case VK_DELETE: return EraseChar(s, hwnd, +1);
        case VK_LEFT:   return HorizontalNav(s, -1, shift);
        case VK_RIGHT:  return HorizontalNav(s, +1, shift);
        case VK_UP:     return VerticalNav(s, -1);
        case VK_DOWN:   return VerticalNav(s, +1);
        case VK_HOME:   return HomeEndKey(s, false, shift);
        case VK_END:    return HomeEndKey(s, true, shift);
        case VK_TAB:    return CycleSelection(s, shift);
        case VK_NEXT:   return ScrollByPage(s, +1);
        case VK_PRIOR:  return ScrollByPage(s, -1);
    }
    return false;
}

static bool OnKeyDown(Overlay& overlay, Overlay::State& s, WPARAM wp) {
    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;

    if (wp == VK_ESCAPE) {
        if (!s.nsNaming) {
            overlay.Hide();
            return true;
        }
        CancelNaming(s);
        RenderNow(s);
        return true;
    }

    if (wp == VK_RETURN) {
        if (s.nsNaming) {
            CommitNamespaceName(s, overlay.Hwnd());
            RenderNow(s);
        } else {
            ActivateSelected(s, overlay.Hwnd());
        }
        return true;
    }

    bool dirty = ctrl ? HandleCtrlKey(s, overlay.Hwnd(), wp, alt, shift)
                      : HandlePlainKey(s, overlay.Hwnd(), wp, shift);
    if (dirty) RenderNow(s);
    return dirty;
}

static void OnChar(Overlay::State& s, HWND hwnd, WPARAM wp) {
    wchar_t ch = (wchar_t)wp;
    // Editing keys arrive via WM_KEYDOWN; Ctrl+letter chords arrive here as
    // control characters and are skipped too.
    if (!IsPrintableChar(ch)) return;

    InsertText(s, std::wstring(1, ch));
    UpdateAfterQueryChange(s, hwnd);
    s.caretOn = true;
    RenderNow(s);
}

// ---------------------------------------------------------------------------
// Mouse input
// ---------------------------------------------------------------------------

static bool ClickSearchBox(Overlay::State& s, HWND hwnd, int x, int y) {
    if (!PointIn(s.searchRect, (float)x, (float)y)) return false;

    size_t pos = HitTestSearchText(s, s.renderer->DWrite(), (float)x);
    s.caret       = pos;
    s.anchor      = pos;
    s.draggingSel = true;
    s.caretOn     = true;
    SetCapture(hwnd);
    RenderNow(s);
    return true;
}

static bool ClickNsButton(Overlay::State& s, HWND hwnd, int x, int y) {
    if (!Ns::Enabled()) return false;

    int i = HitTestNsButtons(s, x, y);
    if (i < 0) return false;

    if (s.nsNaming) CancelNaming(s);
    DoNsFileAction(s, hwnd, i);
    RenderNow(s);
    return true;
}

static bool ClickNsPill(Overlay::State& s, HWND hwnd, int x, int y) {
    int i = HitTestNsPills(s, x, y);
    if (i < 0) return false;

    if (s.nsNaming) CancelNaming(s);
    SwitchToNamespace(s, hwnd, i);
    RenderNow(s);
    return true;
}

static bool ClickScrollbar(Overlay::State& s, HWND hwnd, int x, int y) {
    if (s.mode != Mode::Tiles) return false;

    ScrollbarRects sb = ComputeScrollbar(s);
    if (!sb.present) return false;

    if (PointIn(sb.thumb, (float)x, (float)y)) {
        s.draggingScroll   = true;
        s.scrollDragOffset = (float)y - sb.thumb.top;
        SetCapture(hwnd);
        RenderNow(s);
        return true;
    }

    if (PointIn(sb.track, (float)x, (float)y)) {
        float pageH = s.viewportBot - s.viewportTop;
        float dir   = (y < sb.thumb.top) ? -1.f : 1.f;
        s.scrollY  += dir * pageH * 0.85f;
        ClampScroll(s);
        UpdateThumbnailLayout(s);
        RenderNow(s);
        return true;
    }

    return false;
}

static bool ClickItem(Overlay::State& s, HWND hwnd, int x, int y) {
    int i = HitTestItem(s, x, y);
    if (i < 0) return false;

    s.selected = i;
    ActivateSelected(s, hwnd);
    return true;
}

static void OnLButtonDown(Overlay& overlay, Overlay::State& s, int x, int y) {
    HWND hwnd = overlay.Hwnd();

    if (ClickSearchBox(s, hwnd, x, y)) return;
    if (ClickNsButton(s, hwnd, x, y)) return;
    if (ClickNsPill(s, hwnd, x, y)) return;
    if (ClickScrollbar(s, hwnd, x, y)) return;
    if (ClickItem(s, hwnd, x, y)) return;

    overlay.Hide();
}

static void EnsureMouseLeaveTracking(Overlay::State& s, HWND hwnd) {
    if (s.mouseTracking) return;

    TRACKMOUSEEVENT tme{};
    tme.cbSize    = sizeof(tme);
    tme.dwFlags   = TME_LEAVE;
    tme.hwndTrack = hwnd;
    if (TrackMouseEvent(&tme)) s.mouseTracking = true;
}

static void DragSearchSelection(Overlay::State& s, int x) {
    s.caret   = HitTestSearchText(s, s.renderer->DWrite(), (float)x);
    s.caretOn = true;
    RenderNow(s);
}

static void DragScrollThumb(Overlay::State& s, int y) {
    ScrollbarRects sb = ComputeScrollbar(s);
    if (!sb.present) return;

    float trackH = sb.track.bottom - sb.track.top;
    float thumbH = sb.thumb.bottom - sb.thumb.top;
    float freeH  = trackH - thumbH;
    if (freeH <= 0.f) return;

    float thumbTop = (float)y - s.scrollDragOffset;
    float t = std::clamp((thumbTop - sb.track.top) / freeH, 0.f, 1.f);

    s.scrollY = t * s.maxScroll;
    UpdateThumbnailLayout(s);
    RenderNow(s);
}

static bool ScrollbarHovered(const Overlay::State& s, int x, int y) {
    if (s.mode != Mode::Tiles) return false;

    ScrollbarRects sb = ComputeScrollbar(s);
    return sb.present && PointIn(sb.track, (float)x, (float)y);
}

static void UpdateHover(Overlay::State& s, int x, int y) {
    bool render = false;

    bool sbHover = ScrollbarHovered(s, x, y);
    if (sbHover != s.scrollbarHover) { s.scrollbarHover = sbHover; render = true; }

    int item = sbHover ? -1 : HitTestItem(s, x, y);
    if (item != s.hovered) { s.hovered = item; render = true; }

    int pill = HitTestNsPills(s, x, y);
    if (pill != s.nsPillHovered) { s.nsPillHovered = pill; render = true; }

    int btn = HitTestNsButtons(s, x, y);
    if (btn != s.nsBtnHovered) { s.nsBtnHovered = btn; render = true; }

    if (render) RenderNow(s);
}

static void OnMouseMove(Overlay::State& s, HWND hwnd, int x, int y) {
    EnsureMouseLeaveTracking(s, hwnd);

    if (s.draggingSel)    { DragSearchSelection(s, x); return; }
    if (s.draggingScroll) { DragScrollThumb(s, y); return; }

    UpdateHover(s, x, y);
}

static void OnMouseLeave(Overlay::State& s) {
    s.mouseTracking = false;
    if (s.hovered == -1 && s.nsPillHovered == -1 && s.nsBtnHovered == -1) return;

    s.hovered       = -1;
    s.nsPillHovered = -1;
    s.nsBtnHovered  = -1;
    if (s.visible) RenderNow(s);
}

static LRESULT OnSetCursor(Overlay::State& s, HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);

    bool inSearch  = PointIn(s.searchRect, (float)pt.x, (float)pt.y);
    bool clickable = !inSearch &&
        (HitTestNsPills(s, pt.x, pt.y) >= 0 || HitTestNsButtons(s, pt.x, pt.y) >= 0);

    SetCursor(LoadCursorW(nullptr, inSearch  ? IDC_IBEAM
                                 : clickable ? IDC_HAND
                                             : IDC_ARROW));
    return TRUE;
}

static void OnLButtonUp(Overlay::State& s, HWND hwnd) {
    if (!s.draggingSel && !s.draggingScroll) return;

    ReleaseDrags(s, hwnd);
    RenderNow(s);
}

static void OnLButtonDblClk(Overlay::State& s, int x, int y) {
    if (Ns::Enabled() && !s.nsNaming) {
        int pill = HitTestNsPills(s, x, y);
        if (pill >= 0) {
            BeginRename(s, pill);
            RenderNow(s);
            return;
        }
    }

    if (!PointIn(s.searchRect, (float)x, (float)y) || s.query.empty()) return;

    size_t pos = HitTestSearchText(s, s.renderer->DWrite(), (float)x);
    s.anchor  = WordStartAt(s.query, pos);
    s.caret   = WordEndAt(s.query, pos);
    s.caretOn = true;
    RenderNow(s);
}

static void OnMouseWheel(Overlay::State& s, WPARAM wp) {
    if (s.mode != Mode::Tiles || s.maxScroll <= 0.f) return;

    int delta = GET_WHEEL_DELTA_WPARAM(wp);
    s.scrollY -= kScrollStepPx * s.scale * ((float)delta / WHEEL_DELTA);
    ClampScroll(s);
    UpdateThumbnailLayout(s);
    RenderNow(s);
}

static void OnCaretTimer(Overlay::State& s) {
    s.caretOn = !s.caretOn;
    RenderNow(s);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT Overlay::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    auto& s = *s_;

    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && s.visible) Hide();
            return 0;

        case WM_TIMER:
            if (wp == kCaretTimerId && s.visible) OnCaretTimer(s);
            return 0;

        case WM_CHAR:
            if (s.visible) OnChar(s, hwnd_, wp);
            return 0;

        case WM_KEYDOWN:
            if (s.visible) OnKeyDown(*this, s, wp);
            return 0;

        case WM_SYSKEYDOWN:
            if (s.visible && OnKeyDown(*this, s, wp)) return 0;
            break;

        case WM_LBUTTONDOWN:
            if (s.visible) OnLButtonDown(*this, s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_MOUSEMOVE:
            if (s.visible) OnMouseMove(s, hwnd_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_MOUSELEAVE:
            OnMouseLeave(s);
            return 0;

        case WM_SETCURSOR:
            if (!s.visible) break;
            return OnSetCursor(s, hwnd_);

        case WM_LBUTTONUP:
            OnLButtonUp(s, hwnd_);
            return 0;

        case WM_LBUTTONDBLCLK:
            if (s.visible) OnLButtonDblClk(s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;

        case WM_MOUSEWHEEL:
            if (s.visible) OnMouseWheel(s, wp);
            return 0;

        case WM_DPICHANGED:
            s.dpi   = HIWORD(wp);
            s.scale = s.dpi / 96.0f;
            if (s.visible) RenderNow(s);
            return 0;

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
