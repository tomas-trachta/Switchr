#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct WindowEntry {
    HWND         hwnd;
    std::wstring title;
    std::wstring exe;      // basename, for display
    std::wstring exePath;  // full path, for icon lookup
};

// Top-level alt-tab-eligible windows; `exclude` is skipped.
std::vector<WindowEntry> EnumerateAltTabWindows(HWND exclude);

// Restores `target` if minimized and brings it to the foreground, attaching
// thread input so SetForegroundWindow isn't refused.
void ForceForeground(HWND target);

// Full path of the exe owning `hwnd`; empty on failure.
std::wstring GetWindowExePath(HWND hwnd);
