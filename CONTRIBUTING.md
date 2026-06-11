# Contributing to Switchr

Thanks for being here. Switchr is small and young; the codebase is uniform
and deliberately simple, so a focused PR lands easily. This document is the
map: how the pieces fit together, the conventions that keep the code
readable, and what a PR needs to look like.

If anything here is unclear or out of date, that itself is a fixable bug —
open an issue or a PR against this file.

---

## Ground rules

- One feature or fix per PR.
- Match the existing style. The cheatsheet below is the project standard;
  where it doesn't cover something, match the file you're editing.
- Keep functions small. Every logical step is its own function; a
  top-level function should read top-to-bottom as the list of steps in the
  process. Prefer extracting a named helper over growing a function.
- Builds must stay `/W4` warning-clean.
- No commented-out code. Delete it; git remembers.

---

## Architecture in 5 minutes

Switchr is a tray app: `wWinMain` creates a message-only background window
that owns the global hotkeys and the tray icon, then everything else is
driven by window messages. Read the files in this order to get the shape:

| Module | File(s) | What it does |
|---|---|---|
| Entry | `src/main.cpp` | Single-instance mutex, COM init, background window, the summon hotkey, tray icon + menu, message loop, ordered shutdown. |
| Overlay | `src/Overlay.cpp/h` | The switcher UI. Owns the window list snapshot, the search query/caret, the tile/list layout, the namespace pill bar, and all keyboard + mouse handling. By far the largest TU. |
| Renderer | `src/Renderer.cpp/h` | The D3D11 → DXGI → DirectComposition → D2D/DWrite stack behind a composition window. One per overlay-like window; sized to a single monitor, `Resize()`d before each show. Also caches shell icons as D2D bitmaps. |
| Window list | `src/WindowList.cpp/h` | Enumerates alt-tab-eligible top-level windows (title, exe basename, exe path) and force-foregrounds a target with thread-input attachment. |
| MRU | `src/MruTracker.cpp/h` | WinEvent hook on foreground changes; hands out a monotonic rank per HWND. Seeds from the z-order at startup. |
| Namespaces | `src/Namespaces.cpp/h` | Workspaces. Tracks HWND→namespace assignments, hides/shows windows on switch, registers the global `Ctrl+←/→` hotkeys, auto-assigns new windows via an `EVENT_OBJECT_SHOW` hook, persists structure to `%APPDATA%\Switchr\namespaces.txt`. |
| OSD | `src/NsOsd.cpp/h` | Transient click-through toast shown on headless namespace switches. A miniature Overlay: same Renderer, hold + fade-out timer. |

Cross-cutting decisions worth knowing up-front:

- **Everything is single-threaded.** All modules are called from the main
  thread; the WinEvent hooks are in-process and deliver on the message
  loop. There are no locks anywhere — keep it that way.
- **Visibility changes are batched.** `Ns::ApplyVisibility` puts all
  shows + hides of a switch into one `BeginDeferWindowPos` transaction
  with `DWMWA_TRANSITIONS_FORCEDISABLED` set around it, so a namespace
  switch is one atomic flip with no per-window fade. Don't add
  `ShowWindow` calls outside it.
- **The overlay suspends the global hotkeys while visible.**
  `Ns::SuspendHotkeys()` / `ResumeHotkeys()` — the overlay's own
  `Ctrl+←/→` handling (word jump while editing, namespace switch
  otherwise) needs to see the arrows. If you add a global hotkey, decide
  what it means while the overlay is open.

### The two structural patterns

**Module pattern** — system-wide services (`Mru::`, `Ns::`) are plain
namespaces with free functions declared in the header and all state
TU-local in an anonymous namespace. `Init()`/`Shutdown()` pairs, no
classes, no singletons-with-accessors.

**Overlay pattern** — window classes (`Overlay`, `NsOsd`) keep a minimal
public surface plus a nested `struct State` defined in the .cpp. All logic
lives in `static` free functions taking `(Overlay::State& s, ...)`,
forward-declared in a block near the top of the TU. `WndProc` is a pure
dispatcher to per-message `On*` functions; keyboard handling splits into
`HandleCtrlKey` / `HandlePlainKey` returning a needs-render bool; mouse
clicks are `Click*` predicates tried in order. Free handlers that must
dismiss the overlay take `Overlay&` and call the public `Hide()`.

New code should fit one of these two shapes.

---

## Build

Open `Switchr.sln` in Visual Studio 2022 and build, or:

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
& $msbuild .\Switchr.sln -p:Configuration=Release -p:Platform=x64
```

Output goes to `build\x64\<Config>\Switchr.exe`. x64 only. For a PR check,
always build `Release|x64` — it must compile with zero warnings (`/W4`,
conformance mode, `/utf-8`).

---

## Testing

There is no automated test suite — the app is a thin layer over live Win32
and DWM state, so verification is manual. Before opening a PR, run the
Release build and walk the smoke checks that touch your change, plus the
core loop:

- Summon the overlay, pick a window with arrows + `Enter`, with `Tab`,
  with a mouse click, and via `Ctrl+digit`. The target window comes to the
  foreground even if it was minimized.
- Type a query — grid becomes list, ranking looks sane, clearing the query
  returns to the grid. Caret movement, selection, word jump, and clipboard
  shortcuts work in the search box.
- Scroll the grid with the wheel, `PgUp`/`PgDn`, and the scrollbar.
- Enable namespaces mode: create (`Ctrl+N`), rename (`Ctrl+R`), move a
  window (`Ctrl+Shift+→`), switch headlessly with `Ctrl+→` (toast
  appears, MRU window focused), delete (`Ctrl+D`).
- Save, load, and load-apps round-trip via `Ctrl+S` / `Ctrl+L` /
  `Ctrl+Shift+L`.
- **Disable the mode and exit Switchr: every hidden window must come back
  and the taskbar-animation setting must be restored.** This is the one
  check that is never optional for a namespaces-touching change.
- If your change affects layout or rendering, check a second monitor
  and/or a non-100% DPI scale.

If a change is hard to verify by hand, say how you verified it in the PR
description.

---

## Style cheatsheet

The project compiles as C++17, `/W4`, conformance mode, `/utf-8`, with
`UNICODE`, `NOMINMAX`, and `WIN32_LEAN_AND_MEAN` defined. The points you'll
bump into most often:

- **Indentation** is four spaces, everywhere. `switch` case bodies are
  indented one level under their `case` label.
- **Vertical whitespace.** Steps inside a function are separated by blank
  lines into small bundles of logic — dense unbroken blocks are
  unreadable. Larger TUs are divided by `// ----` section banners.
- **Functions.** Every logical step is its own function, even at the cost
  of a few more calls. Free helpers are `static`; TU-local state and
  helpers live in an anonymous namespace.
- **Naming.** `PascalCase` functions and types, `camelCase` locals and
  parameters, `g_` globals, trailing-underscore members, `k`-prefixed
  `constexpr` constants grouped at the top of the TU.
- **Layout constants** are DIP values scaled by the per-monitor factor at
  the point of use (`kTileGap * s.scale`) — never pre-scaled.
- **Win32.** Wide strings everywhere; call the `W`-suffixed APIs
  explicitly. Zero-init API structs with `{}` and set `cbSize`-style
  fields right after. Aligned member/initializer columns in those blocks.
- **COM** via `Microsoft::WRL::ComPtr`, through the
  `template <class T> using ComPtr` alias in the anonymous namespace.
  No raw owning pointers, no manual `AddRef`/`Release`.
- **Errors.** Exceptions only for unrecoverable initialization (the
  `Renderer` constructor, `Overlay::Create`), caught once in `wWinMain`.
  Everything else degrades silently in kind: failed icon extraction →
  `nullptr` and no icon, failed thumbnail registration → tile without a
  preview, failed OSD creation → no toast. Don't add error dialogs for
  cosmetic failures.
- **Comments.** Avoid them — write self-explanatory code instead. A
  comment is acceptable only when the code cannot express the fact
  itself, which in this codebase means Win32/DWM gotchas (HWND recycling,
  DWM not clipping thumbnail rects, apartment-threading requirements).
  Never narrate what the next line does.
- **Threading.** Everything on the main thread. If you think you need a
  worker thread, open an issue first.

When in doubt, find a recent function in the same file that does something
similar and copy its shape.

---

## PR checklist

Before opening the PR:

- [ ] Builds clean as `Release|x64` with zero warnings.
- [ ] The manual smoke checks above pass, including the
      disable-and-exit restore check if namespaces code was touched.
- [ ] Functions stay small and step-shaped; no dense unbroken blocks.
- [ ] No new comments that merely narrate code; no commented-out code.
- [ ] Hotkey or input changes state what happens both with the overlay
      open and closed.

PR description should call out:

- What user-visible behavior changes, if any.
- How you verified it (which smoke checks, what monitor/DPI setup).
- Any new global state — hooks, hotkeys, registry values — and where it
  is cleaned up on `Disable`/`Shutdown`.

---

## Reporting bugs

Open an issue with:

- Windows build (`winver`) and monitor/DPI setup (count, scales).
- Whether namespaces mode was enabled.
- The smallest sequence of steps that reproduces the issue.
- For rendering issues: GPU and driver version, and whether the overlay
  appears at all (a black or missing overlay is usually the D2D/DComp
  stack failing to initialize).

For crashes, a stack trace from a Debug build (`build\x64\Debug\Switchr.exe`)
is worth several rounds of guessing.

---

## Areas that especially want help

The roadmap in the README lists the bigger bets. Things that would land
cleanly today:

- **Crash recovery** — persist the hidden-window list while namespaces
  mode is on and restore it on the next launch. Self-contained inside
  `Namespaces.cpp`; the highest-value fix in the project.
- **Cross-namespace search** — let list mode match windows in inactive
  namespaces and switch on activate. Touches `ApplyFilter` and
  `ActivateSelected` in `Overlay.cpp`.
- **A real tray icon** — an .ico resource instead of the stock
  `IDI_APPLICATION`.
- **Configurable hotkey** — even a registry value would do; the
  registration is one call in `main.cpp`.
- **CI** — a workflow that builds `Release|x64` on every push would catch
  warning regressions.

If you want to take on something bigger, open an issue first so we can
align on scope before you sink time into it.
