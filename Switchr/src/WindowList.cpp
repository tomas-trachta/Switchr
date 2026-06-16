#include "WindowList.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace {

struct ExeInfo {
    std::wstring path;
    std::wstring base;
};

ExeInfo GetProcessExe(HWND hwnd) {
    ExeInfo res;
    res.path = GetWindowExePath(hwnd);

    if (!res.path.empty()) {
        auto pos = res.path.find_last_of(L"\\/");
        res.base = (pos == std::wstring::npos) ? res.path : res.path.substr(pos + 1);
    }
    return res;
}

// The classic alt-tab ownership rule: a window qualifies only if it is the
// last active visible popup of its own owner chain.
bool IsLastActivePopupOfOwnerChain(HWND hwnd) {
    HWND walk = nullptr;
    HWND tryW = GetAncestor(hwnd, GA_ROOTOWNER);
    while (tryW != walk) {
        walk = tryW;
        tryW = GetLastActivePopup(walk);
        if (IsWindowVisible(tryW)) break;
    }
    return walk == hwnd;
}

bool IsCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked;
}

bool IsAltTabWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (!IsLastActivePopupOfOwnerChain(hwnd)) return false;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (IsCloaked(hwnd)) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;
    return true;
}

std::wstring WindowTitle(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};

    std::wstring title(static_cast<size_t>(len), L'\0');
    GetWindowTextW(hwnd, title.data(), len + 1);
    return title;
}

struct EnumCtx {
    HWND exclude;
    std::vector<WindowEntry>* out;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (hwnd == ctx->exclude) return TRUE;
    if (!IsAltTabWindow(hwnd)) return TRUE;

    ExeInfo exe = GetProcessExe(hwnd);
    ctx->out->push_back(WindowEntry{
        hwnd, WindowTitle(hwnd), std::move(exe.base), std::move(exe.path) });
    return TRUE;
}

} // namespace

std::vector<WindowEntry> EnumerateAltTabWindows(HWND exclude) {
    std::vector<WindowEntry> out;
    EnumCtx ctx{ exclude, &out };
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

std::vector<WindowEntry> DescribeWindows(const std::vector<HWND>& hwnds) {
    std::vector<WindowEntry> out;
    out.reserve(hwnds.size());

    for (HWND hwnd : hwnds) {
        if (!IsWindow(hwnd)) continue;

        ExeInfo exe = GetProcessExe(hwnd);
        out.push_back(WindowEntry{
            hwnd, WindowTitle(hwnd), std::move(exe.base), std::move(exe.path) });
    }
    return out;
}

std::wstring GetWindowExePath(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};

    std::wstring out;
    wchar_t buf[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) out.assign(buf, size);

    CloseHandle(h);
    return out;
}

void ForceForeground(HWND target) {
    if (IsHungAppWindow(target)) {
        if (IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);
        return;
    }

    DWORD curThread = GetCurrentThreadId();
    HWND  fgWnd     = GetForegroundWindow();
    DWORD fgThread  = fgWnd ? GetWindowThreadProcessId(fgWnd, nullptr) : 0;

    bool attached = false;
    if (fgThread && fgThread != curThread && !IsHungAppWindow(fgWnd)) {
        attached = AttachThreadInput(curThread, fgThread, TRUE) != 0;
    }

    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    BringWindowToTop(target);
    SetForegroundWindow(target);
    SetFocus(target);

    if (attached) AttachThreadInput(curThread, fgThread, FALSE);
}
