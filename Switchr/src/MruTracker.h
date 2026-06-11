#pragma once
#include <windows.h>
#include <cstdint>

// Tracks foreground activations system-wide for MRU-first sorting.
// Single-threaded: all calls must come from the thread that called Init().
namespace Mru {

void Init();
void Shutdown();

// Higher = more recently focused; 0 = never seen.
uint64_t Rank(HWND hwnd);

} // namespace Mru
