#pragma once
#include <windows.h>
#include <memory>
#include <string>

// Transient click-through on-screen indicator ("Work · 2 / 3") shown when the
// active namespace changes without the overlay. Holds briefly, then fades out.
class NsOsd {
public:
    // nullptr if the D2D/DComp stack can't be created — the indicator is
    // cosmetic, so callers just skip it.
    static std::unique_ptr<NsOsd> Create(HINSTANCE hInst);
    ~NsOsd();

    NsOsd(const NsOsd&)            = delete;
    NsOsd& operator=(const NsOsd&) = delete;

    // (Re)shows the indicator on the foreground window's monitor and restarts
    // the fade-out countdown.
    void Show(const std::wstring& name, const std::wstring& sub);

    struct State;  // defined in NsOsd.cpp; free helpers there need access

private:
    NsOsd();

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    std::unique_ptr<State> s_;
};
