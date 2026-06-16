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

// Builds entries (title, exe basename, full path) for the given windows
// without the eligibility/visibility filtering EnumerateAltTabWindows applies.
// For window sets whose membership is already decided elsewhere — e.g. a
// namespace's assigned windows, some of which may be momentarily hidden while
// an asynchronous show/hide of a switch settles. Dead HWNDs are dropped.
std::vector<WindowEntry> DescribeWindows(const std::vector<HWND>& hwnds);

// Restores `target` if minimized and brings it to the foreground, attaching
// thread input so SetForegroundWindow isn't refused.
void ForceForeground(HWND target);

// Full path of the exe owning `hwnd`; empty on failure.
std::wstring GetWindowExePath(HWND hwnd);
