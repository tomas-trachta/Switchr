# Switchr

A fast, keyboard-driven window switcher for Windows with live thumbnails,
search, and workspaces. Built from scratch in C++17 on Direct2D /
DirectComposition — a single small exe that sits in the tray and summons a
GPU-rendered overlay on a hotkey.

> Status: **early development.** No releases yet — build it yourself (see
> [Building from source](#building-from-source)). Behavior may change at any
> time.

---

## Why Switchr

Alt-Tab is fine until you have thirty windows. Switchr keeps the good part
(most-recently-used ordering, live previews) and adds what's missing:

- **Type to find.** Start typing and the thumbnail grid becomes a ranked
  list filtered across window titles and exe names, with multi-term
  matching and prefix bonuses.
- **Workspaces ("namespaces").** Group windows into named workspaces;
  only the active one's windows are visible and on the taskbar. Switch
  with `Ctrl+Alt+←/→` from anywhere, save the layout to disk, and
  relaunch a whole workspace of apps in one keystroke.
- **Real rendering.** DirectComposition + Direct2D with live DWM
  thumbnails, server-side fade animation, and per-monitor-V2 DPI
  awareness. The overlay opens on the monitor of the foreground window.
- **No installer, no admin.** One exe, runs `asInvoker`, single-instance.
  Exit from the tray and everything is restored.

---

## Quick start

1. Build (or grab) `Switchr.exe` and run it. An icon appears in the tray.
2. Press **Alt + the key above Tab** (`` ` `` on a US layout, `;` on
   Czech — the physical key, whatever it types) to summon the overlay.
3. Arrows / `Tab` to pick a window, `Enter` or click to activate it,
   `Esc` to dismiss. Or just start typing to search.

The tray icon's right-click menu toggles **Namespaces mode** and exits;
double-click opens the overlay.

---

## The overlay

Two modes, switched automatically: with an empty search box you get a
scrollable **tile grid** of live thumbnails, MRU-first; type anything and
it becomes a **list** ranked by match score. Clearing the query returns to
the grid.

### Navigation

| Keys | Action |
|------|--------|
| `← / → / ↑ / ↓`        | move the selection (grid) / move caret + selection (list) |
| `Tab` / `Shift+Tab`    | cycle through windows |
| `Enter` or click       | activate the selected window |
| `Esc`                  | dismiss the overlay |
| `Home / End`           | first / last window |
| `PgUp / PgDn`          | scroll the grid by a page |
| `Ctrl+1` … `Ctrl+9`    | activate the 1st … 9th window directly |
| wheel / scrollbar drag | scroll the grid |

### Search box editing

The search box is a full text editor: click or drag to place the caret and
select, `Shift+arrows` extend the selection, `Ctrl+←/→` jump by word,
`Ctrl+Backspace` / `Ctrl+Delete` delete by word, and `Ctrl+A` / `Ctrl+C` /
`Ctrl+X` / `Ctrl+V` work with the system clipboard.

---

## Namespaces

Namespaces group top-level windows into named workspaces. Only the active
namespace's windows stay visible (and on the taskbar); the rest are hidden
until their namespace becomes active again. New windows are auto-assigned
to the active namespace as they appear.

Enable the mode from the tray menu or with `Ctrl+M` inside the overlay. A
pill bar appears at the top of the overlay showing every namespace and its
window count, with Save / Load / Load apps buttons at the right end.

| Keys | Action |
|------|--------|
| `Ctrl+Alt+←/→` (global, overlay closed) | switch namespace from anywhere |
| `Ctrl+Alt+←/→` (overlay open, grid)     | switch namespace |
| `Ctrl+Shift+←/→`                        | move the selected window to the neighbor namespace |
| `Ctrl+Shift+1` … `Ctrl+Shift+9`         | move the selected window to namespace 1 … 9 |
| `Ctrl+N`                                | create a namespace (does **not** switch to it) |
| `Ctrl+R` / double-click a pill          | rename the active / clicked namespace |
| `Ctrl+D`                                | delete the active namespace (its windows join the next active one; the last namespace is refused) |
| `Ctrl+M`                                | exit namespaces mode |
| `Ctrl+S`                                | save the layout |
| `Ctrl+L`                                | load the layout and re-assign running apps |
| `Ctrl+Shift+L`                          | load the layout and also launch the missing apps |

Switching headlessly (overlay closed) focuses the target namespace's
most-recently-used window and flashes a small click-through toast
("Work · 2 / 3") in the corner of the screen.

### Persistence

`Ctrl+S` writes the namespace names, their order, and each one's apps (exe
paths) to `%APPDATA%\Switchr\namespaces.txt` — a plain line-based UTF-8
file. `Ctrl+L` restores the structure and re-assigns currently running
apps to their saved namespace by exe match; `Ctrl+Shift+L` additionally
launches whatever isn't running, and the launched windows land in their
saved namespace automatically.

The first time the mode is enabled in a session, the saved names are
loaded automatically — names only, on purpose: hiding your running
windows the moment the mode turns on would be surprising. Press `Ctrl+L`
when you want the full restore.

### Caveats

- While the mode is enabled, Switchr temporarily disables the taskbar
  button slide animation (`HKCU\...\Explorer\Advanced\TaskbarAnimations`,
  previous value saved) — there is no per-app opt-out and the reflow
  animation on every switch is distracting. The value is restored when the
  mode is disabled or Switchr exits cleanly.
- Window assignments live on HWNDs, so they are session-only; the saved
  file maps namespaces to *apps*, not to individual windows.
- If Switchr hard-crashes while a namespace is active, the other
  namespaces' windows stay hidden and the taskbar-animation setting stays
  off (a normal exit restores both). Crash-recovery hardening is the top
  roadmap item.

---

## Building from source

### Requirements

- Windows 10 or later, x64.
- Visual Studio 2022 (or Build Tools 2022) with the **Desktop development
  with C++** workload — MSVC v143 + a Windows 10 SDK.

### Compile

Open `Switchr.sln` and build `Release|x64`, or from a shell:

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
& $msbuild .\Switchr.sln -p:Configuration=Release -p:Platform=x64
```

The binary lands at `build\x64\Release\Switchr.exe`. No runtime payload,
no installer — the exe is the product.

---

## Project layout

```
Switchr.sln
Switchr/
  app.manifest           asInvoker, per-monitor-V2 DPI awareness
  Switchr.vcxproj
  src/
    main.cpp             Entry point: single-instance guard, tray icon,
                         Alt+` hotkey, message loop
    Overlay.cpp/h        The switcher UI: tile grid / list, search box,
                         namespace pill bar, keyboard + mouse input
    Renderer.cpp/h       D3D11 / DXGI / DComp / D2D / DWrite stack,
                         opacity animation, shell-icon bitmap cache
    WindowList.cpp/h     Alt-tab-eligible window enumeration, exe paths,
                         ForceForeground
    MruTracker.cpp/h     System-wide foreground-activation ranking
    Namespaces.cpp/h     Workspaces: hide/show batching, global hotkeys,
                         auto-assign hooks, persistence
    NsOsd.cpp/h          Click-through namespace-switch toast
```

---

## Roadmap

- Crash recovery: persist the hidden-window list and restore it on the
  next launch.
- Cross-namespace search in list mode (today the list only searches the
  visible windows).
- A real tray icon (currently the stock application icon).
- Configurable hotkey.
- CI: build `Release|x64` on every push.

---

## Contributing

Pull requests welcome. See [CONTRIBUTING.md](./CONTRIBUTING.md) for the
architecture tour, the coding conventions, the manual test checklist, and
the PR checklist. The short version:

1. Match the existing style — it is specific and the codebase is uniform.
2. Keep changes focused: one feature or fix per PR.
3. Build `Release|x64` warning-clean (`/W4`) and run the manual smoke
   checks before opening the PR.

Bug reports should include your Windows build, monitor/DPI setup, and the
smallest sequence of steps that reproduces the issue.

---

## License

[MIT](./LICENSE) © 2026 Tomáš Trachta.
