# WM: remove autostart, add dock with icons — design

Date: 2026-08-03

## Problem

- The window manager auto-spawns `bin/term` and `bin/clock` at startup
  (`programs/wm.c:269-270`), so the desktop is never empty.
- There is no way to launch or manage GUI applications interactively; the only
  entry point is the hardcoded autostart.

## Goal

- Remove the autostart of `term` and `clock`.
- Add a centered, compact dock at the bottom of the screen.
- Add launcher icons (term, clock) plus icons for running windows.

## Scope

- Only `programs/wm.c` changes. No new syscalls, no new kernel code, no new
  programs.
- The kernel-side autostart of the WM itself (`kernel/kernel.c:113`) is kept;
  otherwise nothing would draw the dock or manage windows.

## Architecture

Everything lives in the WM, which already owns all on-screen rendering
(desktop, windows, cursor). The dock is drawn topmost (after windows, before
the cursor) and is hit-tested on mouse clicks before window hit-testing.

### Approach

Option A (chosen): all dock logic in `wm.c`.
Rejected: dock as a separate program (needs z-ordering between dock and WM
windows, extra focus plumbing, overkill for a panel). Dock drawn by the kernel
(breaks the ring-3 GUI split).

## Design

### Autostart removal

Delete the two `spawn(...)` calls in `wm.c main()`. Boot leaves an empty
desktop + dock; applications are launched from the dock.

### Dock layout

- Position: bottom-center of the screen.
- Height ~52 px (32 px icon + padding), width fits all items.
- Background: dark rounded rectangle with a border frame.
- Item order: the two launcher icons (term, clock), then one icon per running
  window in creation order.
- Each item: 32×32 icon with ~8 px padding, plus a small indicator dot below
  the icon for running windows.

### Icons

Procedural pixel-art, stored as static arrays in `wm.c`:

- Terminal: window frame with title bar and a `>_` prompt.
- Clock: clock face with hands.
- Unknown window: generic placeholder glyph.

Icons are 32×32, 1 bit per pixel (transparent where 0) so they composite over
the dock background.

### App tracking

```c
enum { APP_TERM, APP_CLOCK, APP_UNKNOWN };
struct app {
    const char *path;   // "bin/term", "bin/clock", NULL for none
    unsigned int pid;
    int running;        // has a live window?
    int type;
};
```

- Two static launcher slots (term, clock) at the start of the dock.
- When the WM spawns an app from the dock it records the returned `pid` and
  sets `running = 1`.
- Each window (`struct win`) gets an `app_type` field, set at `MSG_CREATE`:
  matching dock pid → its type, otherwise `APP_UNKNOWN`.
- On `MSG_EXIT` (window closed): if the pid is a dock launcher, clear
  `running = 0` and drop the `app_type` of freed windows.

### Interaction

- Left-click on a launcher icon:
  - app already running → raise its window and focus it (no duplicate spawn);
  - otherwise → `spawn(path, "", getpid())`, record pid, redraw dock.
- Left-click on a running-window icon → raise that window and focus it.
- Any dock hit consumes the click (window logic is skipped).
- Raising a window = move it to the end of `wins[]` (z-order is the array
  order; hit-test already scans top-down). Also triggers a full redraw.

### Rendering

- `draw_dock()` renders the dock into the framebuffer.
- Called at the end of `composite_rect()` (which `composite()` already wraps),
  so the dock is repainted on every redraw and stays on top of windows,
  including windows dragged over the dock area.
- The dock is repainted even on partial redraws; the region is small (one row
  of 32 px icons) so the cost is negligible.

### Error handling

- `spawn()` returning an error: ignore and log via `print()`; dock stays
  functional. No state is corrupted (pid only recorded on success).
- Click on a stale running-window icon (pid exited but `MSG_EXIT` not yet
  processed): raise logic finds no window, click is a no-op.

## Verification

- `make` builds cleanly (no new warnings).
- Boot in QEMU (see AGENTS.md test flow), dump screenshots with
  `/tmp/opencode/guitester.py` and check pixels:
  - dock centered at the bottom with two icons;
  - no windows auto-opened (empty desktop above the dock);
  - clicking the term icon opens a terminal window;
  - clicking the clock icon opens a clock window;
  - clicking a running-window icon raises/focuses it;
  - closing a window clears its running indicator.
