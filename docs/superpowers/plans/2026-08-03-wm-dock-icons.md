# WM Dock + Launcher Icons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the WM's auto-launch of `term`/`clock` and replace it with a centered bottom dock containing launcher icons (term, clock) plus icons for running windows.

**Architecture:** All changes live in `programs/wm.c`. The dock is drawn topmost by the WM (after windows, before the cursor) and is hit-tested before window hit-testing. Window z-order moves out of the array into a dedicated `zorder[]` index array so windows can be raised without breaking the `winid`↔array-index invariant that `MSG_UPDATE` relies on.

**Tech Stack:** C11 (`-std=c11 -m32 -ffreestanding -nostdlib`), compiled as a ring-3 program linked against `programs/libaos.o`. No libc. No test framework — verification is QEMU pixel checks driven through the QEMU monitor socket.

## Global Constraints

- Only `programs/wm.c` changes (plus new `scripts/guitester.py` test harness). No kernel changes, no new syscalls, no new programs, no `Makefile` changes.
- Screen is 1024×768, 32bpp, from GRUB2 (`-display none` keeps the same mode).
- Colors (24-bit RGB, big-endian 0xRRGGBB):
  - `COL_DESKTOP 0x1A2030` = RGB(26,32,48)
  - `COL_DOCK_BG 0x20283A` = RGB(32,40,58)
  - `COL_DOCK_BORDER 0x2E4E7B` = RGB(46,78,123)
  - `COL_ICON_FG 0xE8EEF8` = RGB(232,238,248)
  - `COL_DOCK_ACTIVE 0x4A7AB5` = RGB(74,122,181) (same value as `COL_TITLE_FOCUS`)
- QEMU monitor socket: `/tmp/aos-gui.sock`; serial log: `/tmp/aos-gui.log`.
- `make` must stay warning-free (`-Wall -Wextra`).
- Every task ends with a build, a QEMU run, pixel assertions via `guitester.py`, and a commit.
- Dock geometry (constant): `DOCK_H 52`, `DOCK_MARGIN 8`, `DOCK_PAD_X 12`, `DOCK_PAD_Y 10`, `DOCK_ICON 32`, `DOCK_STRIDE 40`. `dock_y0 = 768 - 52 - 8 = 708`. `dock_nitems() = 2 + nz`. `dock_x0 = 512 - (16 + dock_nitems()*40)/2`; `dock_width = 16 + dock_nitems()*40`. Item `i` left edge `= dock_x0 + 12 + i*40`; icon top `= 708 + 10 = 718`.
- Precomputed dock coordinates for the tests (screen center 512):
  - n=2 (no windows): dock_x0=464. term icon x=476, clock icon x=516. term click (492,734), clock click (532,734).
  - n=3 (1 window): dock_x0=444. term x=456 (click 472,734), clock x=496 (click 512,734), window icon x=536 (click 552,734), window icon indicator dot at x≈550..553.
  - n=4 (2 windows): dock_x0=424. term x=436 (click 452,734), clock x=476 (click 492,734), win1 x=516 (click 532,734), win2 x=556 (click 572,734).
- Window slots: term=slot0 at (20,20) content 640×416; clock=slot1 at (44,48) content 260×100. Title bar of a window starts at `(x+1, y+1)` with height `TITLE_H 18`; a focused window's title is `0x4A7AB5`, unfocused `0x2E4E7B`. Close button ("X") spans `x+1+cw-18 .. +16` horizontally and `y+1 .. +16` vertically (term close click center ≈ (650,28)).
- Test boot wait: after launching QEMU, wait ≥6 s before the first screenshot. After each mouse click, wait ≥1 s before screenshotting.

---

### Task 1: Test harness (`scripts/guitester.py`) + boot baseline

**Files:**
- Create: `scripts/guitester.py`
- Test: run QEMU, boot AOS, assert desktop pixels

**Interfaces:**
- Produces: `python3 scripts/guitester.py click <x> <y>` — moves the PS/2 cursor to (x,y) and left-clicks; `python3 scripts/guitester.py snap <file>` — writes a PPM screenshot; `python3 scripts/guitester.py pixel <x> <y>` — prints "r g b"; `python3 scripts/guitester.py check <x> <y> <r> <g> <b>` — exits 0 if the pixel matches, exits 1 with a message otherwise.

- [ ] **Step 1: Write the harness**

```python
#!/usr/bin/env python3
"""Minimal GUI tester for AOS WM tests (QEMU monitor + screendump).

Usage:
  guitester.py click <x> <y>          move cursor to (x,y) and left-click
  guitester.py snap <file.ppm>        screendump to file
  guitester.py pixel <x> <y>          print "r g b" of the pixel
  guitester.py check <x> <y> <r> <g> <b>   assert pixel color, exit 1 on mismatch
"""
import socket, sys, time

MON = "/tmp/aos-gui.sock"
PPM = "/tmp/aos-gui-px.ppm"


def cmd(c):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2)
    s.connect(MON)
    s.sendall(c.encode() + b"\n")
    try:
        data = s.recv(65536)
    except Exception:
        data = b""
    s.close()
    return data.decode()


def read_pixel(x, y):
    cmd("screendump " + PPM)
    time.sleep(0.15)
    with open(PPM, "rb") as f:
        f.readline()                       # P6
        w = int(f.readline().split()[0])   # width
        f.readline()                       # maxval
        f.read((y * w + x) * 3)
        return tuple(f.read(3))


def main():
    a = sys.argv[1:]
    if a[0] == "click":
        x, y = int(a[1]), int(a[2])
        cmd("mouse_move %d %d" % (x, y))
        cmd("mouse_button 1")
        time.sleep(0.4)
        cmd("mouse_button 0")
    elif a[0] == "snap":
        cmd("screendump " + a[1])
    elif a[0] == "pixel":
        r, g, b = read_pixel(int(a[1]), int(a[2]))
        print("%d %d %d" % (r, g, b))
    elif a[0] == "check":
        x, y = int(a[1]), int(a[2])
        want = tuple(int(v) for v in a[3:6])
        got = read_pixel(x, y)
        if got != want:
            print("FAIL pixel(%d,%d)=%s want %s" % (x, y, got, want))
            sys.exit(1)
        print("OK pixel(%d,%d)=%s" % (x, y, got))
    else:
        sys.exit("usage: guitester click|snap|pixel|check ...")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Build and boot AOS headless**

```bash
make
qemu-system-i386 -display none \
  -monitor unix:/tmp/aos-gui.sock,server,nowait \
  -serial file:/tmp/aos-gui.log -cdrom aos.iso &
sleep 6
```

- [ ] **Step 3: Verify the harness reads real pixels**

Run: `python3 scripts/guitester.py check 30 30 26 32 48`
Expected: `OK pixel(30,30)=(26,32,48)` — the desktop color, i.e. WM booted and no window covers (20,20) yet (autostart still spawns term+clock at this point, so if the check fails because a title bar is there, that is acceptable for Task 1 — re-run at (512,300), which is desktop even with the old autostart).

If `-display none` produces a black/blank screendump, rerun the same QEMU command with `-display gtk` in a session that has an X display, or add `-vnc 127.0.0.1:0` and capture via the VNC framebuffer; the pixel coordinates are unchanged.

- [ ] **Step 4: Kill QEMU**

```bash
kill %1 2>/dev/null
```

- [ ] **Step 5: Commit**

```bash
git add scripts/guitester.py
git commit -m "test: add QEMU monitor guitester harness for WM pixel checks"
```

---

### Task 2: Remove program autostart

**Files:**
- Modify: `programs/wm.c:268-270` (delete the two `spawn(...)` lines in `main()`)

**Interfaces:**
- Consumes: Task 1 harness.
- Produces: boot that leaves an empty desktop (no term/clock windows).

- [ ] **Step 1: Delete the autostart block**

In `programs/wm.c`, remove the comment and both calls:

```c
    // Launch the desktop applications (they create their windows via MSG_CREATE).
    spawn("bin/term", "", getpid());
    spawn("bin/clock", "", getpid());
```

- [ ] **Step 2: Build, boot, verify no windows**

```bash
make
qemu-system-i386 -display none \
  -monitor unix:/tmp/aos-gui.sock,server,nowait \
  -serial file:/tmp/aos-gui.log -cdrom aos.iso &
sleep 6
python3 scripts/guitester.py check 30 30 26 32 48   # term slot (20,20) empty
python3 scripts/guitester.py check 50 55 26 32 48   # clock slot (44,48) empty
```

Expected: both `OK`, desktop color at both window slots.
Kill QEMU: `kill %1 2>/dev/null`.

- [ ] **Step 3: Commit**

```bash
git add programs/wm.c
git commit -m "feat: remove auto-launch of term and clock from WM startup"
```

---

### Task 3: Dock rendering (icons + background)

**Files:**
- Modify: `programs/wm.c` (add constants, icon arrays, `draw_icon`, `draw_dock`; call `draw_dock` at the end of `composite_rect`; clear cursor if it overlaps the dock)

**Interfaces:**
- Consumes: Task 2 (empty desktop).
- Produces: `draw_dock(void)`, `dock_nitems(void)`, `dock_x0(void)`, `dock_y0(void)`, `dock_width(void)` — used by Task 4 hit-testing. Icons `icon_term[32][33]`, `icon_clock[32][33]`, `icon_unknown[32][33]` and color constants `COL_DOCK_BG`, `COL_DOCK_BORDER`, `COL_ICON_FG`, `COL_DOCK_ACTIVE`.

- [ ] **Step 1: Add dock constants and icon pixel art**

At the top of `programs/wm.c`, after the existing color `#define`s, add:

```c
// ---- dock ----------------------------------------------------------------

#define DOCK_H       52
#define DOCK_MARGIN  8
#define DOCK_PAD_X   12
#define DOCK_PAD_Y   10
#define DOCK_ICON    32
#define DOCK_STRIDE  40

#define COL_DOCK_BG      0x20283A
#define COL_DOCK_BORDER  0x2E4E7B
#define COL_ICON_FG      0xE8EEF8
#define COL_DOCK_ACTIVE  0x4A7AB5

// 32x32 1bpp icons, 'X' = foreground pixel, anything else = transparent.
static const char icon_term[32][33] = {
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X........XX.....................X",
    "X.........XX....................X",
    "X..........XX...................X",
    "X..........XX...................X",
    "X.........XX....................X",
    "X........XX.....................X",
    "X..............................X",
    "X..............................X",
    "X..........XXXXXXXXXXX.........X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
};

static const char icon_clock[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X.XXX........XXXXXXXXX.XXX..X..",
    "..X.XXX........XXXXXXXXX.XXX..X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X..........................X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X...........XXX.............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
};

static const char icon_unknown[32][33] = {
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..........XXXXXXX.............X",
    "X.........XXXXXXXX.............X",
    "X.........XXXXXXXX.............X",
    "X........XX....XXX.............X",
    "X........XX....XXX.............X",
    "X........XX....XXX.............X",
    "X.............XXX..............X",
    "X.............XX...............X",
    "X............XX................X",
    "X............XX................X",
    "X...............................X",
    "X...............................X",
    "X............XX................X",
    "X............XX................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "X...............................X",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
};
```

- [ ] **Step 2: Add dock geometry + drawing functions**

Insert these functions after `composite()` (before the cursor section):

```c
// ---- dock ----------------------------------------------------------------

static int dock_nitems(void) { return 2; }

static int dock_x0(void) {
    return (int)fb_w / 2 - (16 + dock_nitems() * DOCK_STRIDE) / 2;
}

static int dock_y0(void) { return (int)fb_h - DOCK_H - DOCK_MARGIN; }

static int dock_width(void) { return 16 + dock_nitems() * DOCK_STRIDE; }

static void draw_icon(int x, int y, const char icon[DOCK_ICON][DOCK_ICON + 1],
                      unsigned int fg) {
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (int r = 0; r < DOCK_ICON; r++)
        for (int c = 0; c < DOCK_ICON; c++)
            if (icon[r][c] == 'X')
                fb[(unsigned)(y + r) * pitch + (unsigned)(x + c)] = fg;
}

static void draw_dock(void) {
    int dx0 = dock_x0(), dy0 = dock_y0();
    int dw = dock_width();
    fb_fill(dx0, dy0, dw, DOCK_H, COL_DOCK_BG);
    fb_fill(dx0, dy0, dw, 1, COL_DOCK_BORDER);
    fb_fill(dx0, dy0 + DOCK_H - 1, dw, 1, COL_DOCK_BORDER);
    fb_fill(dx0, dy0, 1, DOCK_H, COL_DOCK_BORDER);
    fb_fill(dx0 + dw - 1, dy0, 1, DOCK_H, COL_DOCK_BORDER);
    // fake rounded corners
    fb_fill(dx0, dy0, 1, 1, COL_DESKTOP);
    fb_fill(dx0 + dw - 1, dy0, 1, 1, COL_DESKTOP);
    fb_fill(dx0, dy0 + DOCK_H - 1, 1, 1, COL_DESKTOP);
    fb_fill(dx0 + dw - 1, dy0 + DOCK_H - 1, 1, 1, COL_DESKTOP);
    int iy = dy0 + DOCK_PAD_Y;
    draw_icon(dx0 + DOCK_PAD_X, iy, icon_term, COL_ICON_FG);
    draw_icon(dx0 + DOCK_PAD_X + DOCK_STRIDE, iy, icon_clock, COL_ICON_FG);
}
```

- [ ] **Step 3: Draw the dock at the end of every composite_rect**

In `composite_rect()`, AFTER the clip reset lines:

```c
    clip_x0 = 0; clip_y0 = 0;
    clip_x1 = (int)fb_w; clip_y1 = (int)fb_h;
```

add (important: after the reset, otherwise `fb_fill` inside `draw_dock` would be clipped to the redraw rect and only repaint part of the dock):

```c
    draw_dock();
    if (cursor_overlaps(dock_x0(), dock_y0(), dock_width(), DOCK_H))
        has_cur = 0;
```

The cursor-overlap guard clears `has_cur` so a dock repaint never leaves a stale cursor over the dock (the main loop repaints it).

`cursor_overlaps` is defined later in the file — it is already forward-declared? No. Add a forward declaration near the other forward declarations (next to `draw_close_btn`):

```c
static int cursor_overlaps(int x, int y, int w, int h);
```

- [ ] **Step 4: Build, boot, verify dock geometry**

```bash
make
qemu-system-i386 -display none \
  -monitor unix:/tmp/aos-gui.sock,server,nowait \
  -serial file:/tmp/aos-gui.log -cdrom aos.iso &
sleep 6
python3 scripts/guitester.py check 30 30 26 32 48    # desktop still empty
python3 scripts/guitester.py check 512 734 32 40 58  # dock bg between icons
python3 scripts/guitester.py check 477 719 232 238 248  # term icon title bar
python3 scripts/guitester.py check 519 720 232 238 248  # clock icon frame
```

Expected: all four `OK`. Kill QEMU: `kill %1 2>/dev/null`.

- [ ] **Step 5: Commit**

```bash
git add programs/wm.c
git commit -m "feat: render centered dock with term and clock launcher icons"
```

---

### Task 4: z-order, app tracking, dock interactions

**Files:**
- Modify: `programs/wm.c` (launcher table, `app` field on `struct win`, `zorder[]`, dock hit-testing wired into the click handler, `MSG_CREATE`/`MSG_EXIT` hooks)

**Interfaces:**
- Consumes: Task 3 (`draw_dock`, geometry helpers).
- Produces: `launchers[2]`, `app_type_of(pid)`, `raise_pid(pid)`, `dock_hit(mx,my)`, `launcher_click(i)`; `struct win` gains an `int app;` field; `zorder[]`/`nz` replace array-order as the z-order source for rendering and hit-testing.

- [ ] **Step 1: Add the launcher table and `app` field**

Add to the globals area (after `wins[]`):

```c
enum { APP_TERM = 0, APP_CLOCK = 1, APP_UNKNOWN = 2 };

static struct launcher {
    const char *path;
    unsigned int pid;
    int running;
} launchers[2] = {
    { "bin/term", 0, 0 },
    { "bin/clock", 0, 0 },
};

static int zorder[MAX_WINDOWS];
static int nz;
```

Add `int app;` to `struct win`:

```c
struct win {
    int used;
    unsigned int pid;
    int winid;
    int slab;
    int x, y;       // screen position (whole window incl. border/title)
    int cw, ch;     // content size
    int app;        // APP_TERM / APP_CLOCK / APP_UNKNOWN
};
```

- [ ] **Step 2: App-type helper**

Add a forward declaration next to the other forward declarations (near `draw_close_btn`):

```c
static int app_type_of(unsigned int pid);
```

Then add the definition after `free_windows`:

```c
static int app_type_of(unsigned int pid) {
    for (int i = 0; i < 2; i++)
        if (launchers[i].pid == pid) return i;
    return APP_UNKNOWN;
}
```

- [ ] **Step 3: z-order plumbing**

In `alloc_window`, set the app type and register in zorder. After `wins[i].used = 1;` add `wins[i].app = app_type_of(pid);`, and after the `*out_slab = wins[i].slab;` line add `zorder[nz++] = i;`.

Replace `free_windows` with a version that removes entries from zorder and clears launcher running state:

```c
static void free_windows(unsigned int pid) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used && wins[i].pid == pid) {
            wins[i].used = 0;
            for (int j = 0; j < nz; j++)
                if (zorder[j] == i) {
                    for (int k = j; k < nz - 1; k++) zorder[k] = zorder[k + 1];
                    nz--;
                    break;
                }
            if (focus_pid == pid) focus_pid = 0;
        }
    for (int i = 0; i < 2; i++)
        if (launchers[i].pid == pid) launchers[i].running = 0;
}

static void raise_pid(unsigned int pid) {
    int at = -1;
    for (int i = 0; i < nz; i++)
        if (wins[zorder[i]].used && wins[zorder[i]].pid == pid) { at = i; break; }
    if (at < 0) return;
    int wi = zorder[at];
    for (int j = at; j < nz - 1; j++) zorder[j] = zorder[j + 1];
    zorder[nz - 1] = wi;
    if (focus_pid != pid) redraw = 1;
    focus_pid = pid;
}
```

- [ ] **Step 4: Render and hit-test by zorder**

In `composite_rect`, replace the window loop header `for (int i = 0; i < MAX_WINDOWS; i++)` with `for (int k = 0; k < nz; k++)` and use `struct win *wn = &wins[zorder[k]];` (keep the existing `if (!wn->used) continue;` and the rest of the loop body unchanged).

Replace `win_index_at` with a zorder top-down scan:

```c
static int win_index_at(int mx, int my) {
    for (int k = nz - 1; k >= 0; k--) {
        struct win *wn = &wins[zorder[k]];
        if (!wn->used) continue;
        if (mx >= wn->x && mx < wn->x + wn->cw + 2 * BORDER &&
            my >= wn->y && my < wn->y + wn->ch + TITLE_H + 2 * BORDER)
            return zorder[k];
    }
    return -1;
}
```

Replace `close_btn_at` with a zorder top-down scan:

```c
static int close_btn_at(int mx, int my) {
    for (int k = nz - 1; k >= 0; k--) {
        struct win *wn = &wins[zorder[k]];
        if (!wn->used) continue;
        int bx = wn->x + BORDER + wn->cw - 18;
        int by = wn->y + BORDER + (TITLE_H - 16) / 2;
        if (mx >= bx && mx < bx + 16 && my >= by && my < by + 16)
            return zorder[k];
    }
    return -1;
}
```

- [ ] **Step 5: Dock hit-testing**

Add after `draw_dock`:

```c
static int win_at_dock_index(int di) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (wins[i].used) {
            if (n == di) return i;
            n++;
        }
    return -1;
}

static void launcher_click(int i) {
    if (launchers[i].running) {
        raise_pid(launchers[i].pid);
        return;
    }
    int pid = spawn(launchers[i].path, "", getpid());
    if (pid > 0) launchers[i].pid = (unsigned int)pid;
}

static int dock_hit(int mx, int my) {
    int dx0 = dock_x0(), dy0 = dock_y0();
    if (my < dy0 || my >= dy0 + DOCK_H) return 0;
    int rel = mx - dx0;
    if (rel < 0 || rel >= dock_width()) return 0;
    int i = (rel - DOCK_PAD_X) / DOCK_STRIDE;
    if (i < 0) return 1;
    if (i < 2) launcher_click(i);
    else if (i < 2 + nz) {
        int wi = win_at_dock_index(i - 2);
        if (wi >= 0) raise_pid(wins[wi].pid);
    }
    return 1;
}
```

- [ ] **Step 6: Extend `dock_nitems` and `draw_dock` for running windows**

Update `dock_nitems` to count live windows:

```c
static int dock_nitems(void) { return 2 + nz; }
```

Append the running-window loop to `draw_dock`, after the clock launcher icon:

```c
    int di = 2;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!wins[i].used) continue;
        const char (*ic)[DOCK_ICON + 1];
        if (wins[i].app == APP_TERM) ic = icon_term;
        else if (wins[i].app == APP_CLOCK) ic = icon_clock;
        else ic = icon_unknown;
        int ix = dx0 + DOCK_PAD_X + di * DOCK_STRIDE;
        draw_icon(ix, iy, ic, COL_ICON_FG);
        unsigned int *fb = (unsigned int *)fb_addr;
        unsigned int pitch = fb_pitch >> 2;
        int dotx = ix + DOCK_ICON / 2 - 2;
        int doty = iy + DOCK_ICON + 4;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                fb[(unsigned)(doty + r) * pitch + (unsigned)(dotx + c)] = COL_DOCK_ACTIVE;
        di++;
    }
```

- [ ] **Step 7: Wire dock_hit into the click handler and hook MSG_CREATE/MSG_EXIT**

In `main()`'s press handler, wrap the existing window logic:

```c
        if (moved && (mb & 1) && !(last_mb & 1)) {
            if (!dock_hit(mx, my)) {
                int cb = close_btn_at(mx, my);
                ... existing body unchanged ...
            }
        }
```

In the `MSG_CREATE` case, mark the launcher as running (before the `alloc_window` call):

```c
            case MSG_CREATE: {
                for (int i = 0; i < 2; i++)
                    if (launchers[i].pid && launchers[i].pid == m.c)
                        launchers[i].running = 1;
                int wid, slab;
                ...
            }
```

The `MSG_EXIT` case stays `free_windows(m.a); redraw = 1;` — `free_windows` already resets launcher state now.

In `main()`'s redraw block, repaint the cursor whenever a redraw erased it (handles dock repaints during `MSG_UPDATE`/drag partial redraws):

```c
        int need_cursor = moved;
        if (!has_cur) need_cursor = 1;
        if (redraw) {
            composite();
            has_cur = 0;
            redraw = 0;
            need_cursor = 1;
        }
        if (need_cursor) {
            update_cursor(mx, my);
        }
```

- [ ] **Step 8: Build, boot, verify the full interaction flow**

```bash
make
qemu-system-i386 -display none \
  -monitor unix:/tmp/aos-gui.sock,server,nowait \
  -serial file:/tmp/aos-gui.log -cdrom aos.iso &
sleep 6
```

Assertions (each line must print `OK`):

```bash
# 1. Empty desktop + dock, n=2
python3 scripts/guitester.py check 30 30 26 32 48
python3 scripts/guitester.py check 512 734 32 40 58
# 2. Click term launcher (n=2 geometry) -> term window opens, focused
python3 scripts/guitester.py click 492 734
sleep 1
python3 scripts/guitester.py check 30 30 74 122 181   # term title, focused
python3 scripts/guitester.py check 30 60 16 16 16     # term content bg
# 3. Running-window icon + indicator dot appear (n=3 geometry)
python3 scripts/guitester.py check 551 755 74 122 181 # indicator dot
# 4. Click clock launcher (n=3 geometry) -> clock opens, focused
python3 scripts/guitester.py click 512 734
sleep 1
python3 scripts/guitester.py check 50 55 74 122 181   # clock title, focused
python3 scripts/guitester.py check 30 30 46 78 123    # term title now unfocused
# 5. Click term launcher again (n=4 geometry) -> raises term, no duplicate
python3 scripts/guitester.py click 452 734
sleep 1
python3 scripts/guitester.py check 30 30 74 122 181   # term raised + focused
# 6. Close the (now top) term window via its X button
python3 scripts/guitester.py click 650 28
sleep 1
python3 scripts/guitester.py check 30 30 26 32 48     # slot (20,20) free again
# 7. Term launcher cleared -> clicking it opens a fresh term (n=3 geometry)
python3 scripts/guitester.py click 472 734
sleep 1
python3 scripts/guitester.py check 30 30 74 122 181
```

If a click lands on the wrong item because a window shifted the dock, recompute the geometry from the Global Constraints table (n=2/n=3/n=4 click coordinates) rather than weakening the assertion.

Kill QEMU: `kill %1 2>/dev/null`.

- [ ] **Step 9: Commit**

```bash
git add programs/wm.c
git commit -m "feat: dock launcher icons launch apps and raise/focus running windows"
```
