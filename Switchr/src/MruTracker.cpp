#include "MruTracker.h"

#include <unordered_map>
#include <vector>

namespace {

HWINEVENTHOOK                       g_hook    = nullptr;
uint64_t                            g_counter = 0;
std::unordered_map<HWND, uint64_t>  g_ranks;

void CALLBACK FgHookProc(HWINEVENTHOOK, DWORD, HWND hwnd,
                         LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return;

    g_ranks[hwnd] = ++g_counter;
}

BOOL CALLBACK CollectHwndProc(HWND hwnd, LPARAM lp) {
    reinterpret_cast<std::vector<HWND>*>(lp)->push_back(hwnd);
    return TRUE;
}

// EnumWindows yields top-to-bottom z-order; the topmost window is the most
// recently active one, so it gets the largest rank.
void SeedRanksFromZOrder() {
    std::vector<HWND> hwnds;
    hwnds.reserve(256);
    EnumWindows(CollectHwndProc, reinterpret_cast<LPARAM>(&hwnds));

    g_counter = static_cast<uint64_t>(hwnds.size()) + 1;
    uint64_t rank = g_counter;
    for (HWND h : hwnds) g_ranks[h] = rank--;
}

} // namespace

void Mru::Init() {
    if (g_hook) return;

    SeedRanksFromZOrder();

    g_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, FgHookProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void Mru::Shutdown() {
    if (g_hook) { UnhookWinEvent(g_hook); g_hook = nullptr; }
    g_ranks.clear();
    g_counter = 0;
}

uint64_t Mru::Rank(HWND hwnd) {
    auto it = g_ranks.find(hwnd);
    return it == g_ranks.end() ? 0 : it->second;
}
