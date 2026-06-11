#pragma once
#include <windows.h>
#include <memory>

class Overlay {
public:
    // Throws std::runtime_error on failure.
    static std::unique_ptr<Overlay> Create(HINSTANCE hInst);
    ~Overlay();

    Overlay(const Overlay&)            = delete;
    Overlay& operator=(const Overlay&) = delete;

    HWND Hwnd() const { return hwnd_; }
    bool IsVisible() const;
    void Toggle();
    void Show();
    void Hide();

    struct State;  // defined in Overlay.cpp; free helpers there need access

private:
    Overlay();

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    std::unique_ptr<State> s_;
};
