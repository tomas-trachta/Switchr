#include "Namespaces.h"
#include "WindowList.h"
#include "MruTracker.h"

#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

namespace {

bool                           g_enabled = false;
int                            g_active  = 0;
HWND                           g_owner   = nullptr;
bool                           g_hotkeysSuspended = false;
std::vector<std::wstring>      g_names;
std::unordered_map<HWND, int>  g_assign;
HWINEVENTHOOK                  g_showHook    = nullptr;
HWINEVENTHOOK                  g_destroyHook = nullptr;

void RegisterCycleHotkeys() {
    if (!g_owner || g_hotkeysSuspended) return;

    RegisterHotKey(g_owner, Ns::kHotkeyPrev, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_LEFT);
    RegisterHotKey(g_owner, Ns::kHotkeyNext, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_RIGHT);
}

void UnregisterCycleHotkeys() {
    if (!g_owner) return;

    UnregisterHotKey(g_owner, Ns::kHotkeyPrev);
    UnregisterHotKey(g_owner, Ns::kHotkeyNext);
}

// The taskbar animates its button reflow whenever buttons appear/disappear and
// offers no per-app opt-out — the only knob is the user's "Animations in the
// taskbar" setting. It is suppressed for the duration of the mode and restored
// afterwards; Explorer applies the change on WM_SETTINGCHANGE "TraySettings".
constexpr wchar_t kAdvancedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr wchar_t kTaskbarAnimations[] = L"TaskbarAnimations";

DWORD g_savedTaskbarAnim = 1;
bool  g_animSuppressed   = false;

DWORD ReadTaskbarAnimations() {
    DWORD val = 1, size = sizeof(val), type = 0;

    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, KEY_QUERY_VALUE, &key)
            != ERROR_SUCCESS) {
        return val;
    }

    if (RegQueryValueExW(key, kTaskbarAnimations, nullptr, &type,
            reinterpret_cast<BYTE*>(&val), &size) != ERROR_SUCCESS ||
        type != REG_DWORD) {
        val = 1;
    }

    RegCloseKey(key);
    return val;
}

void WriteTaskbarAnimations(DWORD val) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvancedKey, 0, KEY_SET_VALUE, &key)
            == ERROR_SUCCESS) {
        RegSetValueExW(key, kTaskbarAnimations, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(key);
    }

    DWORD_PTR ignored;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
        reinterpret_cast<LPARAM>(L"TraySettings"),
        SMTO_ABORTIFHUNG, 100, &ignored);
}

void SuppressTaskbarAnimations() {
    if (g_animSuppressed) return;

    g_savedTaskbarAnim = ReadTaskbarAnimations();
    if (g_savedTaskbarAnim != 0) WriteTaskbarAnimations(0);
    g_animSuppressed = true;
}

void RestoreTaskbarAnimations() {
    if (!g_animSuppressed) return;

    if (g_savedTaskbarAnim != 0) WriteTaskbarAnimations(g_savedTaskbarAnim);
    g_animSuppressed = false;
}

std::unordered_set<HWND> g_noTrans;

void SuppressTransitions(HWND h) {
    if (!g_noTrans.insert(h).second) return;

    BOOL on = TRUE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on));
}

void RestoreTransitions() {
    BOOL off = FALSE;
    for (HWND h : g_noTrans) {
        if (IsWindow(h))
            DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    }
    g_noTrans.clear();
}

void ApplyVisibility(const std::vector<HWND>& show, const std::vector<HWND>& hide) {
    for (HWND h : show) SuppressTransitions(h);
    for (HWND h : hide) SuppressTransitions(h);

    constexpr UINT kCommon = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
    HDWP hdwp = BeginDeferWindowPos((int)(show.size() + hide.size()));
    for (HWND h : show) { if (hdwp) hdwp = DeferWindowPos(hdwp, h, nullptr, 0, 0, 0, 0, kCommon | SWP_SHOWWINDOW); }
    for (HWND h : hide) { if (hdwp) hdwp = DeferWindowPos(hdwp, h, nullptr, 0, 0, 0, 0, kCommon | SWP_HIDEWINDOW); }

    if (hdwp) {
        EndDeferWindowPos(hdwp);
    } else {
        for (HWND h : show) ShowWindowAsync(h, SW_SHOWNA);
        for (HWND h : hide) ShowWindowAsync(h, SW_HIDE);
    }
}

void ReapplyVisibility() {
    if (!g_enabled) return;

    std::vector<HWND> show, hide;
    for (auto it = g_assign.begin(); it != g_assign.end(); ) {
        if (!IsWindow(it->first)) { it = g_assign.erase(it); continue; }

        bool visible = IsWindowVisible(it->first);
        if (it->second == g_active) { if (!visible) show.push_back(it->first); }
        else                        { if (visible)  hide.push_back(it->first); }
        ++it;
    }

    ApplyVisibility(show, hide);
}

void ShowAllManaged() {
    std::vector<HWND> show;
    for (auto& [hwnd, ns] : g_assign) {
        if (IsWindow(hwnd) && !IsWindowVisible(hwnd)) show.push_back(hwnd);
    }
    ApplyVisibility(show, {});
}

// ---------------------------------------------------------------------------
// Persistence (%APPDATA%\Switchr\namespaces.txt). Line-based UTF-8:
//   ns<TAB><name>      starts a namespace (file order = namespace order)
//   app<TAB><exe path> an app saved under the namespace above
// ---------------------------------------------------------------------------

struct SavedNs {
    std::wstring              name;
    std::vector<std::wstring> apps;
};

// Apps launched by LoadApps join their saved namespace (not the active one)
// when their window finally appears; entries expire in case a launch dies.
struct PendingAssign {
    std::wstring exeLower;
    int          ns;
    ULONGLONG    tick;
};
std::vector<PendingAssign> g_pending;
constexpr ULONGLONG        kPendingTtlMs = 120 * 1000;

void PrunePending() {
    ULONGLONG now = GetTickCount64();
    g_pending.erase(
        std::remove_if(g_pending.begin(), g_pending.end(),
            [now](const PendingAssign& p) { return now - p.tick > kPendingTtlMs; }),
        g_pending.end());
}

std::wstring ToLowerW(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};

    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const char* p, int len) {
    if (len <= 0) return {};

    int n = MultiByteToWideChar(CP_UTF8, 0, p, len, nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, p, len, w.data(), n);
    return w;
}

std::wstring ConfigFilePath() {
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)))
        return {};

    std::wstring dir(roaming);
    CoTaskMemFree(roaming);

    dir += L"\\Switchr";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\namespaces.txt";
}

bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    BOOL ok = out.empty() ||
              ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    return ok && read == out.size();
}

void ParseLine(const char* line, size_t len, std::vector<SavedNs>& out) {
    if (len > 3 && memcmp(line, "ns\t", 3) == 0) {
        out.push_back({ FromUtf8(line + 3, (int)len - 3), {} });
    } else if (len > 4 && memcmp(line, "app\t", 4) == 0 && !out.empty()) {
        out.back().apps.push_back(FromUtf8(line + 4, (int)len - 4));
    }
}

bool ParseSaved(std::vector<SavedNs>& out) {
    std::wstring path = ConfigFilePath();
    if (path.empty()) return false;

    std::string data;
    if (!ReadFileBytes(path, data)) return false;

    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();

        size_t len = end - pos;
        if (len > 0 && data[pos + len - 1] == '\r') --len;
        ParseLine(data.data() + pos, len, out);

        pos = end + 1;
    }
    return !out.empty();
}

// Replace the namespace list with the saved one. Window assignments survive
// by index; anything now out of range falls back to the active namespace.
void ApplyStructure(const std::vector<SavedNs>& saved) {
    g_names.clear();
    for (const auto& n : saved) g_names.push_back(n.name);

    if (g_active >= (int)g_names.size()) g_active = 0;
    for (auto& [hwnd, ns] : g_assign) {
        if (ns < 0 || ns >= (int)g_names.size()) ns = g_active;
    }

    ReapplyVisibility();
}

// Every claimable window by lowercased exe path: managed windows (visible or
// hidden) plus visible windows nobody tracks yet.
std::unordered_map<std::wstring, std::vector<HWND>> BuildExeWindowPool() {
    std::unordered_map<std::wstring, std::vector<HWND>> pool;

    for (auto& [hwnd, ns] : g_assign) {
        if (!IsWindow(hwnd)) continue;
        std::wstring exe = ToLowerW(GetWindowExePath(hwnd));
        if (!exe.empty()) pool[exe].push_back(hwnd);
    }

    for (const auto& w : EnumerateAltTabWindows(nullptr)) {
        if (g_assign.count(w.hwnd) || w.exePath.empty()) continue;
        pool[ToLowerW(w.exePath)].push_back(w.hwnd);
    }

    return pool;
}

bool LaunchApp(const std::wstring& path) {
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open",
        path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

// Re-assign open windows to their saved namespaces; each window satisfies at
// most one saved slot, so nothing gets doubled. With `launchMissing`, saved
// apps that had no window left to claim are started and join their namespace
// when their window appears. Returns the number launched.
int MatchSavedApps(const std::vector<SavedNs>& saved, bool launchMissing) {
    auto pool = BuildExeWindowPool();

    int launched = 0;
    for (int i = 0; i < (int)saved.size(); ++i) {
        for (const auto& app : saved[i].apps) {
            std::wstring key = ToLowerW(app);

            auto it = pool.find(key);
            if (it != pool.end() && !it->second.empty()) {
                Ns::Assign(it->second.back(), i);
                it->second.pop_back();
            } else if (launchMissing && LaunchApp(app)) {
                g_pending.push_back({ key, i, GetTickCount64() });
                ++launched;
            }
        }
    }
    return launched;
}

// ---------------------------------------------------------------------------
// WinEvent hooks
// ---------------------------------------------------------------------------

// Loose alt-tab-style eligibility for auto-assignment. Stricter checks (owner
// chain, cloaking) are skipped on purpose: windows mid-creation often fail
// them and would escape tracking. The overlay sweeps up anything missed here,
// so under-matching is safe; over-matching (hiding another app's popups) is not.
bool IsAssignable(HWND hwnd) {
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;
    return true;
}

bool ConsumePendingAssign(HWND hwnd) {
    if (g_pending.empty()) return false;
    PrunePending();

    std::wstring exe = ToLowerW(GetWindowExePath(hwnd));
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        if (it->exeLower != exe) continue;

        int ns = it->ns;
        g_pending.erase(it);
        if (ns < 0 || ns >= (int)g_names.size()) return false;

        g_assign[hwnd] = ns;
        if (ns != g_active && IsWindowVisible(hwnd)) ApplyVisibility({}, { hwnd });
        return true;
    }
    return false;
}

void CALLBACK ShowHookProc(HWINEVENTHOOK, DWORD, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_enabled || !hwnd) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (g_assign.count(hwnd)) return;
    if (!IsAssignable(hwnd)) return;

    if (ConsumePendingAssign(hwnd)) return;
    g_assign[hwnd] = g_active;
}

void CALLBACK DestroyHookProc(HWINEVENTHOOK, DWORD, HWND hwnd,
                              LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    // Erase eagerly — HWNDs are recycled, and a stale entry would dump a
    // future unrelated window into an old namespace.
    g_assign.erase(hwnd);
    g_noTrans.erase(hwnd);
}

void SweepUntrackedWindows() {
    for (const auto& w : EnumerateAltTabWindows(nullptr)) {
        if (!g_assign.count(w.hwnd)) g_assign[w.hwnd] = g_active;
    }
}

} // namespace

void Ns::Init(HWND hotkeyOwner) {
    if (g_showHook) return;

    g_owner = hotkeyOwner;
    g_showHook = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
        nullptr, ShowHookProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_destroyHook = SetWinEventHook(
        EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY,
        nullptr, DestroyHookProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void Ns::Shutdown() {
    ShowAllManaged();
    RestoreTransitions();
    RestoreTaskbarAnimations();
    UnregisterCycleHotkeys();

    if (g_showHook)    { UnhookWinEvent(g_showHook);    g_showHook    = nullptr; }
    if (g_destroyHook) { UnhookWinEvent(g_destroyHook); g_destroyHook = nullptr; }

    g_assign.clear();
    g_names.clear();
    g_pending.clear();
    g_enabled = false;
    g_active  = 0;
    g_owner   = nullptr;
}

bool Ns::Enabled() { return g_enabled; }

void Ns::Enable() {
    if (g_enabled) return;

    g_enabled = true;
    RegisterCycleHotkeys();
    SuppressTaskbarAnimations();

    // First enable of a session restores the saved names only — sorting the
    // running apps away (hiding them) the moment the mode turns on would be
    // surprising; that's the Load button's job.
    if (g_names.empty()) {
        if (!LoadStructure(false)) g_names.push_back(L"Main");
        g_active = 0;
    }

    SweepUntrackedWindows();
}

void Ns::Disable() {
    if (!g_enabled) return;

    g_enabled = false;
    UnregisterCycleHotkeys();
    ShowAllManaged();
    RestoreTransitions();
    RestoreTaskbarAnimations();
}

int Ns::Count()  { return (int)g_names.size(); }
int Ns::Active() { return g_active; }

const std::wstring& Ns::Name(int idx) {
    static const std::wstring kEmpty;
    if (idx < 0 || idx >= (int)g_names.size()) return kEmpty;
    return g_names[idx];
}

int Ns::WindowCount(int idx) {
    int n = 0;
    for (auto& [hwnd, ns] : g_assign) {
        if (ns == idx && IsWindow(hwnd)) ++n;
    }
    return n;
}

int Ns::Create(const std::wstring& name) {
    g_names.push_back(name);
    return (int)g_names.size() - 1;
}

bool Ns::Rename(int idx, const std::wstring& name) {
    if (idx < 0 || idx >= (int)g_names.size() || name.empty()) return false;

    g_names[idx] = name;
    return true;
}

bool Ns::Delete(int idx) {
    if (idx < 0 || idx >= (int)g_names.size() || g_names.size() <= 1) return false;

    g_names.erase(g_names.begin() + idx);

    if (g_active > idx)       --g_active;
    else if (g_active == idx) g_active = std::min(idx, (int)g_names.size() - 1);

    for (auto& [hwnd, ns] : g_assign) {
        if (ns == idx)     ns = g_active;
        else if (ns > idx) --ns;
    }
    for (auto& p : g_pending) {
        if (p.ns == idx)     p.ns = g_active;
        else if (p.ns > idx) --p.ns;
    }

    if (g_enabled) ReapplyVisibility();
    return true;
}

void Ns::SwitchTo(int idx) {
    if (!g_enabled || idx < 0 || idx >= (int)g_names.size() || idx == g_active) return;

    g_active = idx;
    ReapplyVisibility();
}

bool Ns::CycleActive(int dir) {
    if (!g_enabled) return false;

    int count = (int)g_names.size();
    if (count <= 1) return false;

    SwitchTo((g_active + dir + count) % count);

    // Focus the new namespace's MRU-top window; without this, keystrokes keep
    // going to the window that was just hidden.
    HWND best = nullptr;
    uint64_t bestRank = 0;
    for (const auto& [hwnd, ns] : g_assign) {
        if (ns != g_active || !IsWindow(hwnd)) continue;
        uint64_t r = Mru::Rank(hwnd);
        if (!best || r > bestRank) { best = hwnd; bestRank = r; }
    }
    if (best) ForceForeground(best);

    return true;
}

void Ns::SuspendHotkeys() {
    g_hotkeysSuspended = true;
    UnregisterCycleHotkeys();
}

void Ns::ResumeHotkeys() {
    g_hotkeysSuspended = false;
    if (g_enabled) RegisterCycleHotkeys();
}

void Ns::Assign(HWND hwnd, int idx) {
    if (idx < 0 || idx >= (int)g_names.size() || !IsWindow(hwnd)) return;

    g_assign[hwnd] = idx;
    if (!g_enabled) return;

    if (idx == g_active && !IsWindowVisible(hwnd)) ApplyVisibility({ hwnd }, {});
    if (idx != g_active &&  IsWindowVisible(hwnd)) ApplyVisibility({}, { hwnd });
}

int Ns::Of(HWND hwnd) {
    auto it = g_assign.find(hwnd);
    return it == g_assign.end() ? -1 : it->second;
}

bool Ns::SaveToFile() {
    if (g_names.empty()) return false;

    std::vector<std::vector<std::wstring>> apps(g_names.size());
    for (auto& [hwnd, ns] : g_assign) {
        if (ns < 0 || ns >= (int)g_names.size() || !IsWindow(hwnd)) continue;
        std::wstring exe = GetWindowExePath(hwnd);
        if (!exe.empty()) apps[(size_t)ns].push_back(exe);
    }

    std::string data;
    for (size_t i = 0; i < g_names.size(); ++i) {
        data += "ns\t" + ToUtf8(g_names[i]) + "\n";
        for (const auto& a : apps[i]) data += "app\t" + ToUtf8(a) + "\n";
    }

    std::wstring path = ConfigFilePath();
    return !path.empty() && WriteFileBytes(path, data);
}

bool Ns::LoadStructure(bool assignApps) {
    std::vector<SavedNs> saved;
    if (!ParseSaved(saved)) return false;

    ApplyStructure(saved);
    if (assignApps) MatchSavedApps(saved, false);
    return true;
}

int Ns::LoadApps() {
    std::vector<SavedNs> saved;
    if (!ParseSaved(saved)) return -1;

    ApplyStructure(saved);
    PrunePending();
    return MatchSavedApps(saved, true);
}
