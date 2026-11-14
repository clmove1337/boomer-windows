![boomer for Windows](assets/banner.png)

A Windows take on [tsoding/boomer](https://github.com/tsoding/boomer): zoom into your screen, pixelated, with a flashlight.
It's one C file using Win32, GDI and DXGI, with no dependencies, and builds to a ~170 KB exe.

![boomer zooming into its own source code: wheel zoom, drag, flashlight, Esc](assets/demo.gif)

## Build

Needs Visual Studio 2022 with the C++ workload:

```bat
build.bat
```

The exe lands in `build\boomer.exe`.

Run `build\boomer.exe --self-test` to check geometry, cursor-anchored zoom, zoom limits, animation convergence at different frame rates, drawing and undo, and buffer cleanup without opening the overlay. Exit code 0 means success; a nonzero code identifies the failing source line. These checks also run when assertions are disabled.

`test.bat` builds and runs the checks normally, then repeats with `NDEBUG` and MSVC static analysis enabled. Checks also cover flashlight color arithmetic and one-shot timer firing/cancellation. This does not replace interactive testing on different monitor layouts.

## Run

```bat
boomer.exe            :: open once, Esc quits
boomer.exe --daemon   :: stay in the background, pop up on Win+Shift+Z
```

Boomer zooms a screenshot taken when it opens, like the original, and shows up in screen sharing (Discord, OBS). Each popup takes a fresh screenshot.

To start the daemon on login, put a shortcut to `boomer.exe --daemon` into `shell:startup`.

## Controls

| Input | Action |
|---|---|
| Mouse wheel | Zoom toward the cursor |
| Left drag | Pan |
| Alt + left drag | Draw red freehand lines on the screenshot |
| Ctrl + Z | Undo the latest line (hold to keep undoing) |
| `F` | Toggle flashlight (fades in/out) |
| Ctrl + wheel | Flashlight radius |
| `0` | Reset view |
| `Q` / `Esc` | Zoom back to 1:1 and close (press again to skip) |

Hold Alt when starting a stroke. Drawing stays attached to the screenshot while zooming or panning and clears on the next popup. Release the mouse before zooming; `0` resets the view but keeps your marks. Each Alt + drag is one line for Ctrl + Z, and you can undo back to the clean screenshot.

## How it works

- **Capture:** DXGI Desktop Duplication on every monitor, with a GDI fallback. Frame acquisition shares a 250 ms wait budget across monitors; capture resources are released before showing the screenshot.
- **Pixelated zoom:** `StretchBlt` with `COLORONCOLOR` (nearest neighbour).
- **Frame pacing:** A one-shot timer coalesces mouse-driven redraws and animation updates using the compositor refresh interval. Fast mouse input updates the latest view without redrawing the entire screen for every event. Painting never waits for a refresh, and the timer stops while idle. Older Windows versions use a standard timer if high-resolution timers are unavailable.
- **Monitors:** The layout is re-read on every popup, so plugging monitors in needs no restart.
- **Memory:** Two 32-bit screen buffers, plus a darkened buffer allocated only when the flashlight is first used and a copy of the unmarked screenshot allocated on the first drawn line for undo (about 63 MiB at 3840 × 2160 with neither, plus 32 MiB for each, excluding GPU resources). Undo replays the remaining lines from a small list of recorded segments.
- **Idle cost:** While hidden, the daemon uses no timer and frees its screen buffers. It keeps the D3D device so popup takes ~20–30 ms.

Known limits: rotated monitors fall back to a static GDI capture. A monitor added while boomer is open shows up on the next popup. While a screen share with cursor is running, Windows may draw the cursor into the screenshot, so it gets zoomed too.
