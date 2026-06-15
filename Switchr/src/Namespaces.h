#pragma once
#include <windows.h>
#include <string>

// Groups top-level windows into named workspaces. Only the active namespace's
// windows stay visible (and on the taskbar); the rest are hidden until their
// namespace becomes active again. While enabled, a WinEvent hook auto-assigns
// every newly shown top-level window to the active namespace.
// Single-threaded: all calls must come from the thread that called Init().
namespace Ns {

// WM_HOTKEY ids (Ctrl+Alt+Left/Right) delivered to the Init() owner window
// while the mode is enabled.
constexpr UINT kHotkeyPrev = 0x4F8B;
constexpr UINT kHotkeyNext = 0x4F8C;

void Init(HWND hotkeyOwner);
void Shutdown();   // un-hides every managed window and removes hooks

bool Enabled();
void Enable();     // first call of a session restores the saved structure
void Disable();    // un-hides every managed window; assignments survive

// The overlay suspends the global Ctrl+Alt+arrow hotkeys while it is visible so
// its own key handling sees the arrows.
void SuspendHotkeys();
void ResumeHotkeys();

int                 Count();
int                 Active();
const std::wstring& Name(int idx);
int                 WindowCount(int idx);

int  Create(const std::wstring& name);
bool Rename(int idx, const std::wstring& name);
bool Delete(int idx);       // refuses the last namespace; its windows join the
                            // active one, higher indices shift down
void SwitchTo(int idx);
bool CycleActive(int dir);  // SwitchTo(active ± 1) + focus its MRU-top window
void Assign(HWND hwnd, int idx);
int  Of(HWND hwnd);         // -1 if untracked

// Persistence — %APPDATA%\Switchr\namespaces.txt.
bool SaveToFile();
bool LoadStructure(bool assignApps);  // restore saved names/order; with
                                      // assignApps also re-assign running apps
                                      // by exe match, launching nothing
int  LoadApps();                      // LoadStructure(true) + launch missing
                                      // apps; returns #launched, -1 if no file

} // namespace Ns
