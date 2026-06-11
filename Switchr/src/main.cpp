// Switchr — fast window switcher (D2D + DComp UI).
// Alt + key-above-Tab (VK_OEM_3) toggles the overlay.

#include "Overlay.h"
#include "MruTracker.h"
#include "Namespaces.h"
#include "NsOsd.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <memory>
#include <string>

namespace {

constexpr UINT    kHotkeyId    = 0x4F8A;
constexpr UINT    kTrayId      = 1;
constexpr UINT    kMenuExitId  = 2001;
constexpr UINT    kMenuNsId    = 2002;
constexpr UINT    WM_TRAYICON  = WM_APP + 1;
constexpr wchar_t kBgClassName[] = L"SwitchrBackground";

Overlay* g_overlay = nullptr;
NsOsd*   g_osd     = nullptr;
HWND     g_bg      = nullptr;

void ShowNamespaceOsd() {
    if (!g_osd) return;

    g_osd->Show(Ns::Name(Ns::Active()),
        std::to_wstring(Ns::Active() + 1) + L" / " + std::to_wstring(Ns::Count()));
}

void HandleHotkey(WPARAM wp) {
    if (wp == kHotkeyId) {
        if (g_overlay) g_overlay->Toggle();
        return;
    }

    if (wp != Ns::kHotkeyPrev && wp != Ns::kHotkeyNext) return;

    // Fires only with the overlay closed — it suspends these keys while visible.
    int dir = (wp == Ns::kHotkeyNext) ? +1 : -1;
    if (Ns::CycleActive(dir)) ShowNamespaceOsd();
}

void ToggleNamespacesMode() {
    if (Ns::Enabled()) Ns::Disable();
    else               Ns::Enable();
}

void ShowTrayMenu(HWND owner) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (Ns::Enabled() ? MF_CHECKED : MF_UNCHECKED),
        kMenuNsId, L"Namespaces mode");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExitId, L"Exit Switchr");

    SetForegroundWindow(owner);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(menu);
}

void AddTrayIcon(HWND owner) {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = owner;
    nid.uID              = kTrayId;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Switchr — Alt+; to summon");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon(HWND owner) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = owner;
    nid.uID    = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

LRESULT CALLBACK BgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_HOTKEY:
            HandleHotkey(wp);
            return 0;

        case WM_TRAYICON:
            if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu(hwnd);
            else if (LOWORD(lp) == WM_LBUTTONDBLCLK && g_overlay) g_overlay->Show();
            return 0;

        case WM_COMMAND:
            if (LOWORD(wp) == kMenuExitId) {
                RemoveTrayIcon(hwnd);
                PostQuitMessage(0);
            } else if (LOWORD(wp) == kMenuNsId) {
                ToggleNamespacesMode();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateBackgroundWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = BgWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kBgClassName;
    if (!RegisterClassExW(&wc)) return nullptr;

    return CreateWindowExW(0, kBgClassName, L"Switchr",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInst, nullptr);
}

int RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Switchr.SingleInstance.{A1B2C3D4}");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // SHGetFileInfo and WIC require apartment-threaded COM.
    HRESULT coInit = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Before any of our own windows exist, so the seed z-order sweep skips them.
    Mru::Init();

    g_bg = CreateBackgroundWindow(hInst);
    if (!g_bg) {
        MessageBoxW(nullptr, L"Failed to create background window.", L"Switchr", MB_ICONERROR);
        return 1;
    }

    Ns::Init(g_bg);

    std::unique_ptr<Overlay> overlay;
    try {
        overlay = Overlay::Create(hInst);
    } catch (const std::exception&) {
        MessageBoxW(nullptr,
            L"Failed to initialise the D2D/DComp overlay. Update graphics drivers?",
            L"Switchr", MB_ICONERROR);
        return 1;
    }
    g_overlay = overlay.get();

    std::unique_ptr<NsOsd> osd = NsOsd::Create(hInst);
    g_osd = osd.get();

    if (!RegisterHotKey(g_bg, kHotkeyId, MOD_ALT | MOD_NOREPEAT, VK_OEM_3)) {
        MessageBoxW(nullptr,
            L"Failed to register Alt+; hotkey. Is it already in use?",
            L"Switchr", MB_ICONERROR);
        return 1;
    }

    AddTrayIcon(g_bg);

    int exitCode = RunMessageLoop();

    UnregisterHotKey(g_bg, kHotkeyId);
    RemoveTrayIcon(g_bg);

    g_osd = nullptr;
    osd.reset();
    g_overlay = nullptr;
    overlay.reset();

    Ns::Shutdown();
    Mru::Shutdown();
    if (SUCCEEDED(coInit)) CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return exitCode;
}
