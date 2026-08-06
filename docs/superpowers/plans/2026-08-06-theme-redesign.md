# Config-Driven Modern Dark Theme — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the flat black-bordered, 1-bit-icon GUI with a modern config-driven dark theme — rounded (stair-stepped) window/dock/menu/dialog corners, two-color icons, an accent focus highlight, and a unified palette that every GUI app loads from `sys/config.cfg`.

**Architecture:** A new shared userland loader `programs/musl/theme.c` (+ `theme.h`) reads `sys/config.cfg` once and exposes `theme_color(key, fallback)`; `kernel/config.c` writes the ten `theme_*` defaults into the generated config file; `programs/musl/wm.c` is reworked (chrome, dock, menu, dialog, icons) and `term.c`/`clock.c`/`notepad.c` consume the palette. Because the defaults equal today's anchor colors, the existing pixel tests stay green; an extended `configtest.py` Boot B seeds a disk config with non-default colors to prove end-to-end theme loading.

**Tech Stack:** Static musl i386 user programs (`tools/musl-i686/bin/i686-linux-musl-gcc -static -no-pie -Os -Wall -Wextra -Iprograms`), kernel C11 (`-ffreestanding -nostdlib`). No libc in the kernel. Verification is QEMU pixel assertions via `scripts/guitester.py` + the headless regression scripts in `make test` (established repo convention — no unit-test framework).

**Spec:** `docs/superpowers/specs/2026-08-06-theme-redesign-design.md` (committed as `1deb17a`).

## Global Constraints

- The ten theme keys and their defaults (verbatim from the spec; a missing key falls back to the default):

  | key | default | purpose |
  |---|---|---|
  | `theme_title` | `0x263C5E` | title bar, unfocused |
  | `theme_title_focus` | `0x4E86C7` | title bar, focused |
  | `theme_border` | `0x12161F` | window frame, unfocused |
  | `theme_border_focus` | `0x6B9BD2` | window frame, focused |
  | `theme_dock_bg` | `0x232C40` | dock background |
  | `theme_accent` | `0x5B93D8` | indicators, icon accents, menu hover |
  | `theme_menu_bg` | `0x20283A` | context menu / dialog background |
  | `theme_menu_fg` | `0xFFFFFF` | menu/dialog text |
  | `theme_text_fg` | `0xD8D8D8` | app text |
  | `theme_text_bg` | `0x101010` | app background |

- `timezone`, `wallpaper_top`, `wallpaper_bot` keep their current semantics and defaults (`0x1A2030` / `0x0E1620`); the WM's `load_wallpaper_config()` is **replaced** by `theme_load()` so wallpaper + theme come from one parser. The kernel keeps its own ring-0 `config.c` parser (it runs before the WM exists).
- Corner radii: windows 4 (top corners only), dock 6 (top corners only), menu 3 (all corners), dialog 3 (all corners). Stair-stepped, no anti-aliasing; corner pixels are left untouched so the background shows through.
- Hit-testing (`win_index_at`, drag, `dock_hit`, `close_btn_at`, `icon_at`) stays bounding-box based — rounded corners are cosmetic only.
- Dialog input field stays `0x101010` (a `notepadtest` probe reads the menu bg at a specific pixel, and the input box is a separate visual element).
- The `.ico` decoder (`programs/musl/ico.c`/`ico.h`) is unchanged; desktop `.ico` files still render their decoded pixels.
- Test anchors preserved by defaults (do not change): desktop top pixel `0x1A2030`, `notepadtest` menu bg `0x20283A` at (520,402) and dialog bg at (400,285), content bg `0x101010` at (300,100), bright-text band (`theme_text_fg` on `theme_text_bg`) in `manytest`/`ipctest`/`configtest`.
- `scripts/*` change only where asserted colors actually change — that means only `configtest.py` (which gains Boot-B theme assertions); `manytest.py`, `ipctest.py`, `notepadtest.py` are run unmodified and must stay green.
- `make` must stay warning-free (`-Wall -Wextra`).
- Every task ends with a build, a QEMU run with pixel assertions via the regression scripts, and a commit (in this offline environment builds/tests are run by the human).

---

### Task 1: Shared theme loader + kernel defaults + WM wallpaper via theme

The loader and its plumbing land here, and the WM switches its wallpaper parsing onto it, so Task 1 is observably testable end-to-end (Boot B seeds a non-default `wallpaper_top` and the desktop pixel changes).

**Files:**
- Create: `programs/musl/theme.h`, `programs/musl/theme.c`
- Modify: `Makefile:57-63` (pattern + wm special rules)
- Modify: `kernel/config.c:69-73` (default file text)
- Modify: `programs/musl/wm.c:321-390` (delete `strequal`, `parse_hex_cfg`, `load_wallpaper_config`), `programs/musl/wm.c:1016-1018` (call `theme_load`), `programs/musl/wm.c:1-8` (add include)
- Modify: `scripts/configtest.py:323-344` (Boot B seed + wallpaper pixel assert)

**Interfaces:**
- Produces (used by Tasks 2–5): `void theme_load(void);` and `unsigned int theme_color(const char *key, unsigned int fallback);` — declared in `programs/musl/theme.h`. `theme_load()` reads `sys/config.cfg` once (first call wins, later calls no-op); unknown/duplicate keys are ignored with first occurrence winning; missing keys keep the compile-time default; invalid hex values leave the default.
- Consumes: the config file format written by `kernel/config.c` (lines `key=0xRRGGBB`, `#` comments, blank lines ignored).

- [ ] **Step 1: Create `programs/musl/theme.h`**

```c
#ifndef THEME_H
#define THEME_H

// Shared config-file theme loader for GUI programs (musl build).
// Reads sys/config.cfg once (the same file kernel/config.c writes/parses in
// ring 0) and fills a static table. Unknown/duplicate keys are ignored
// (first occurrence wins); missing keys fall back to compile-time defaults.

void theme_load(void);

// Returns the parsed value for `key`, or `fallback` when the key is unknown.
unsigned int theme_color(const char *key, unsigned int fallback);

#endif
```

- [ ] **Step 2: Create `programs/musl/theme.c`**

```c
#include <fcntl.h>
#include <unistd.h>
#include "theme.h"

// Defaults must match the generated sys/config.cfg (kernel/config.c).
static unsigned int v_title = 0x263C5E;
static unsigned int v_title_focus = 0x4E86C7;
static unsigned int v_border = 0x12161F;
static unsigned int v_border_focus = 0x6B9BD2;
static unsigned int v_dock_bg = 0x232C40;
static unsigned int v_accent = 0x5B93D8;
static unsigned int v_menu_bg = 0x20283A;
static unsigned int v_menu_fg = 0xFFFFFF;
static unsigned int v_text_fg = 0xD8D8D8;
static unsigned int v_text_bg = 0x101010;
static unsigned int v_top = 0x1A2030;
static unsigned int v_bot = 0x0E1620;

struct th_entry {
    const char *key;
    unsigned int *val;
};

static struct th_entry entries[] = {
    { "theme_title", &v_title },
    { "theme_title_focus", &v_title_focus },
    { "theme_border", &v_border },
    { "theme_border_focus", &v_border_focus },
    { "theme_dock_bg", &v_dock_bg },
    { "theme_accent", &v_accent },
    { "theme_menu_bg", &v_menu_bg },
    { "theme_menu_fg", &v_menu_fg },
    { "theme_text_fg", &v_text_fg },
    { "theme_text_bg", &v_text_bg },
    { "wallpaper_top", &v_top },
    { "wallpaper_bot", &v_bot },
};
#define NENT (int)(sizeof(entries) / sizeof(entries[0]))

static int strequal(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

// Parse a hex color. Returns 0 and stores *out on success; -1 (leaving the
// caller's default intact) when the value has no hex digits or trailing
// garbage. Note: *s is peeked, never consumed past the terminator.
static int parse_hex(const char *s, unsigned int *out) {
    unsigned int v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    int n = 0;
    for (;;) {
        char c = *s;            // peek; never consume the terminator
        unsigned int d;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
        else break;
        s++;
        v = v * 16 + d;
        n++;
    }
    if (n == 0 || *s != 0) return -1;
    *out = v;
    return 0;
}

void theme_load(void) {
    static char seen[NENT];
    char buf[512];
    int fd = open("sys/config.cfg", O_RDONLY);
    if (fd < 0) return;
    int sz = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (sz <= 0) return;
    buf[sz] = 0;
    char *p = buf;
    while (p && *p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        if (eol > p && eol[-1] == '\r') eol[-1] = 0;
        char saved = *eol;
        *eol = 0;
        char *k = p;
        while (*k == ' ' || *k == '\t') k++;
        if (*k != '#' && *k != 0) {
            char *eq = k;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') {
                *eq = 0;
                const char *val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                for (int i = 0; i < NENT; i++)
                    if (!seen[i] && strequal(entries[i].key, k)) {
                        unsigned int hv;
                        if (parse_hex(val, &hv) == 0) {
                            *entries[i].val = hv;
                            seen[i] = 1;
                        }
                        break;
                    }
            }
        }
        *eol = saved;
        if (saved == '\n') p = eol + 1;
        else break;
    }
}

unsigned int theme_color(const char *key, unsigned int fallback) {
    for (int i = 0; i < NENT; i++)
        if (strequal(entries[i].key, key)) return *entries[i].val;
    return fallback;
}
```

- [ ] **Step 3: Add the ten `theme_*` defaults to `kernel/config.c`**

In `kernel/config.c:69-73` replace the `def[]` string:

```c
        static const char def[] =
            "# AOS system config\n"
            "timezone=0\n"
            "wallpaper_top=0x1A2030\n"
            "wallpaper_bot=0x0E1620\n"
            "theme_title=0x263C5E\n"
            "theme_title_focus=0x4E86C7\n"
            "theme_border=0x12161F\n"
            "theme_border_focus=0x6B9BD2\n"
            "theme_dock_bg=0x232C40\n"
            "theme_accent=0x5B93D8\n"
            "theme_menu_bg=0x20283A\n"
            "theme_menu_fg=0xFFFFFF\n"
            "theme_text_fg=0xD8D8D8\n"
            "theme_text_bg=0x101010\n";
```

No `apply_line` change is needed — the kernel's ring-0 parser ignores unknown keys (they are for userland).

- [ ] **Step 4: Link `theme.c` into the four GUI programs (`Makefile`)**

Replace `Makefile:57-63` with:

```make
# Programs are compld static musl ELFs (Task 30). wm additionally links the
# pure-C ICO decoder (programs/musl/ico.c); the GUI apps link the shared theme
# loader (programs/musl/theme.c).
build/prog/%.elf: programs/musl/%.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ $<

build/prog/wm.elf: programs/musl/wm.c programs/musl/ico.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/wm.c programs/musl/ico.c programs/musl/theme.c

build/prog/term.elf: programs/musl/term.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/term.c programs/musl/theme.c

build/prog/clock.elf: programs/musl/clock.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/clock.c programs/musl/theme.c

build/prog/notepad.elf: programs/musl/notepad.c programs/musl/theme.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ programs/musl/notepad.c programs/musl/theme.c
```

- [ ] **Step 5: Switch the WM's wallpaper parsing onto `theme_load` (`programs/musl/wm.c`)**

Add after `#include "ico.h"` (line 8): `#include "theme.h"`.

Delete the functions `strequal` (lines 321-324), `parse_hex_cfg` (lines 340-354), and `load_wallpaper_config` (lines 356-390). They are used only by `load_wallpaper_config` and by nothing else.

In `main()` (line 1018) replace `load_wallpaper_config();` with:

```c
    theme_load();
    wp_top = theme_color("wallpaper_top", 0x1A2030);
    wp_bot = theme_color("wallpaper_bot", 0x0E1620);
```

- [ ] **Step 6: Extend `configtest.py` Boot B to prove userland theme loading**

In `scripts/configtest.py` add constants near the top (after line 38):

```python
ACCENT = (255, 0, 255)              # theme_accent 0xFF00FF
WALL = (16, 32, 48)                 # wallpaper_top 0x102030
BB = "/tmp/aos-config-bootb.ppm"
```

In Boot B (line 329) change the seed to include a non-default wallpaper, and after the timezone assertions (after line 342) take a screenshot and assert the desktop top pixel:

```python
    with open(IMG, "wb") as f:
        f.write(build_sfs([("sys/config.cfg",
                            b"timezone=+180\nwallpaper_top=0x102030\n")]))
```

```python
        if "KERNEL PANIC" in serial_text():
            raise AssertionError("kernel panic during config boot")
        time.sleep(2)
        snap(BB)
        assert_pixel(BB, 700, 0, WALL,
                     "wallpaper_top from disk config via theme_load")
        print("  ok: disk-seeded timezone +180 and wallpaper_top applied")
```

- [ ] **Step 7: Build and test**

Run: `make` then `python3 scripts/configtest.py`
Expected: clean build (no warnings); `configtest` reports `PASS: boot logo, config, gradient wallpaper` — Boot A still shows `0x1A2030` at (700,0) (defaults unchanged) and `cat sys/config.cfg` renders (the band grows more than before because the file is now 14 lines); Boot B shows `(16,32,48)` at (700,0), proving the WM pulled `wallpaper_top` through `theme_load` from the disk config.

- [ ] **Step 8: Commit**

```bash
git add programs/musl/theme.h programs/musl/theme.c Makefile kernel/config.c programs/musl/wm.c scripts/configtest.py
git commit -m "feat: add shared theme loader, config defaults, and WM wallpaper via theme"
```

---

### Task 2: WM window chrome + dock redesign

Rounded window corners, per-focus frame color, title gradient, and the new dock (rounded top corners, accent top line, theme background, accent active-dot).

**Files:**
- Modify: `programs/musl/wm.c:15-24` (color defines → theme globals), `:35-38` (dock defines), `:328-338` (add `fb_hline` near `fb_fill`), `:432-455` (`draw_title`), `:486-524` (`composite_rect` frame), `:552-586` (`draw_dock`)
- Modify: `scripts/configtest.py` (Boot B seed adds `theme_accent`; dock accent-line assert)

**Interfaces:**
- Consumes: `theme_load()` / `theme_color()` from Task 1.
- Produces (used by Tasks 3–4): module-level globals `col_title`, `col_title_focus`, `col_border`, `col_border_focus`, `col_dock_bg`, `col_accent`, `col_menu_bg`, `col_menu_fg`, `col_icon_fg` (all `static unsigned int`, initialized to the Task 1 defaults); helpers `fb_hline(int x,int y,int x1,unsigned int rgb)`, `fb_round_fill_top(int x,int y,int w,int h,int r,unsigned int rgb)`, `fb_round_fill(int x,int y,int w,int h,int r,unsigned int rgb)`, `lighten(unsigned int c,unsigned int amt)`.

- [ ] **Step 1: Color defines → theme globals (`wm.c:15-24`)**

Replace the block at lines 15-24:

```c
#define COL_DESKTOP      0x1A2030
#define COL_TITLE_TEXT   0xFFFFFF
#define COL_CURSOR       0xFFFFFF

// Theme colors; assigned from sys/config.cfg via theme_load() in main.
// The initializers equal the config defaults (see theme.c). Declare each
// global in the task that first uses it (Task 3 adds col_icon_fg, Task 4
// adds col_menu_bg/col_menu_fg) so -Wall -Wextra never sees an unused
// static variable.
static unsigned int col_title = 0x263C5E;
static unsigned int col_title_focus = 0x4E86C7;
static unsigned int col_border = 0x12161F;
static unsigned int col_border_focus = 0x6B9BD2;
static unsigned int col_dock_bg = 0x232C40;
static unsigned int col_accent = 0x5B93D8;

// Desktop gradient colors; overridden from sys/config.cfg if present.
static unsigned int wp_top = COL_DESKTOP;
static unsigned int wp_bot = 0x0E1620;
```

(`COL_BORDER`, `COL_TITLE`, `COL_TITLE_FOCUS`, `COL_DOCK_BG`, `COL_DOCK_BORDER`, `COL_DOCK_ACTIVE` are gone; `COL_DESKTOP`/`COL_TITLE_TEXT`/`COL_CURSOR` remain and are used in this task.)

Delete `COL_DOCK_BG` and `COL_DOCK_BORDER` and `COL_DOCK_ACTIVE` from lines 35-38 (the dock now uses `col_dock_bg`/`col_accent`). Keep `COL_ICON_FG` for now — it is still referenced by the `draw_icon(...)` calls in `draw_dock` until Task 3 replaces them and removes the macro.

- [ ] **Step 2: Add drawing helpers (after `fb_fill`, ~line 338)**

```c
static void fb_hline(int x, int y, int x1, unsigned int rgb) {
    if (y < clip_y0 || y >= clip_y1) return;
    if (x < clip_x0) x = clip_x0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (x >= x1) return;
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    for (; x < x1; x++) fb[(unsigned)y * pitch + (unsigned)x] = rgb;
}

// Return c lightened by amt/16 toward white, per channel.
static unsigned int lighten(unsigned int c, unsigned int amt) {
    unsigned int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r += ((0xFF - r) * amt) >> 4;
    g += ((0xFF - g) * amt) >> 4;
    b += ((0xFF - b) * amt) >> 4;
    return (r << 16) | (g << 8) | b;
}

// Fill rect (x,y,w,h) with rgb, cutting a stair-stepped r-pixel L-shape from
// the two top corners. Corner pixels are left untouched (background shows).
// Radius-r stair: row dy (0..r-1) cuts (r-1-dy) px from each edge.
static void fb_round_fill_top(int x, int y, int w, int h, int r,
                              unsigned int rgb) {
    for (int yy = 0; yy < h; yy++) {
        int cut = (yy < r) ? (r - 1 - yy) : 0;
        fb_hline(x + cut, y + yy, x + w - cut, rgb);
    }
}
```

(`fb_round_fill`, the all-four-corners variant, is added in Task 4 where it is
first used — an unused static function here would trip `-Wunused-function`.)

- [ ] **Step 3: Title bar with focus color + gradient (`draw_title`, lines 432-455)**

Replace the first two lines of `draw_title` (lines 433-434) with:

```c
    unsigned int tcol = (wn->pid == focus_pid) ? col_title_focus : col_title;
    fb_fill(wn->x + BORDER, wn->y + BORDER, wn->cw, TITLE_H, tcol);
    fb_fill(wn->x + BORDER, wn->y + BORDER, wn->cw, 1,
            lighten(tcol, 4));                       // lighter top strip
    fb_fill(wn->x + BORDER, wn->y + BORDER + 1, wn->cw, 1,
            lighten(tcol, 2));                       // mid strip
```

(Everything below — text render, clip, `draw_close_btn` — is unchanged.)

- [ ] **Step 4: Window frame with rounded corners + focus border (`composite_rect`, lines 500-506)**

Replace the four `fb_fill` border calls inside the window loop (lines 500-506) with:

```c
            unsigned int bcol = (wn->pid == focus_pid) ? col_border_focus
                                                       : col_border;
            fb_round_fill_top(wn->x, wn->y, wn->cw + 2 * BORDER,
                              wn->ch + TITLE_H + 2 * BORDER, 4, bcol);
```

(`draw_title` and `blit_content` after it cover the interior; the 1px frame ring and its rounded top corners remain visible.)

- [ ] **Step 5: Dock redesign (`draw_dock`, lines 552-586)**

Replace the body of `draw_dock` (the `fb_fill` border + fake-corner lines 555-564) with:

```c
    fb_round_fill_top(dx0, dy0, dw, DOCK_H, 6, col_dock_bg);
    fb_hline(dx0 + 5, dy0, dx0 + dw - 6, col_accent);            // accent line
    fb_hline(dx0 + 5, dy0 + 1, dx0 + dw - 6, lighten(col_dock_bg, 2));
    fb_hline(dx0 + 5, dy0 + 2, dx0 + dw - 6, lighten(col_dock_bg, 1));
```

Then in the running-window loop (line 583) replace `COL_DOCK_ACTIVE` with `col_accent`. The launcher/running icon calls (`draw_icon(..., COL_ICON_FG)`) stay as-is until Task 3.

- [ ] **Step 6: Assign theme colors in `main()`**

In `main()`, right after the `theme_load()`/`wp_top`/`wp_bot` lines from Task 1, add:

```c
    col_title = theme_color("theme_title", 0x263C5E);
    col_title_focus = theme_color("theme_title_focus", 0x4E86C7);
    col_border = theme_color("theme_border", 0x12161F);
    col_border_focus = theme_color("theme_border_focus", 0x6B9BD2);
    col_dock_bg = theme_color("theme_dock_bg", 0x232C40);
    col_accent = theme_color("theme_accent", 0x5B93D8);
```

- [ ] **Step 7: Extend `configtest.py` Boot B with the accent assertion**

Change the Boot B seed (line 329) to:

```python
        f.write(build_sfs([("sys/config.cfg",
                            b"timezone=+180\nwallpaper_top=0x102030\n"
                            b"theme_accent=0xFF00FF\n")]))
```

And after the wallpaper assert from Task 1, add:

```python
        assert_pixel(BB, 480, 708, ACCENT,
                     "dock top accent line == theme_accent")
```

Rationale: dock `dy0 = 768 - 52 - 8 = 708`, `dx0 = 1024/2 - (16 + 2*40)/2 = 464`, width 96 → the accent line spans x 469..554 at y=708; (480,708) is clear of the cursor ((511,383)) and of desktop icons (the dock is drawn last and covers any icon overlap).

- [ ] **Step 8: Build and test**

Run: `make` then `python3 scripts/configtest.py`
Expected: clean build; `configtest` PASS — Boot B now shows `(255,0,255)` on the dock accent line, proving `theme_accent` flows from disk config → `theme_load` → `draw_dock`.
Then run `python3 scripts/notepadtest.py` — must stay green (menu/dialog are untouched this task).

- [ ] **Step 9: Commit**

```bash
git add programs/musl/wm.c scripts/configtest.py
git commit -m "feat: themed window chrome and dock with rounded corners and focus border"
```

---

### Task 3: Two-color dock + desktop icons

Replace the 1-bit icon art with two-color (white outline + accent detail) art and route every icon through one `draw_icon2` helper.

**Files:**
- Modify: `programs/musl/wm.c:41-249` (all six icon art arrays), `:542-550` (`draw_icon` → `draw_icon2`), `:566-567,576` (dock icon calls), `:651-657` (`draw_icon_art` removed), `:752-788` (`draw_ico_file` fallback + `draw_desktop_icons`)

**Interfaces:**
- Consumes: `col_icon_fg`, `col_accent` globals from Task 2.
- Produces: `static void draw_icon2(int x, int y, const char art[32][33], unsigned int fg, unsigned int accent)` — `'X'` draws `fg`, `'O'` draws `accent`, anything else is transparent; clip-aware (uses `fb_put`). Used by dock and desktop icons. It replaces `draw_icon` (dock) and `draw_icon_art` (desktop).

- [ ] **Step 1: Replace all six icon art arrays (lines 41-249)**

Declare the icon foreground global (used by `draw_icon2` and the dock/desktop
call sites) near the dock defines:

```c
static unsigned int col_icon_fg = 0xFFFFFF;
```

`icon_term` (terminal: white monitor frame, accent `>_` prompt + taskbar):

```c
static const char icon_term[32][33] = {
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X.......O......................X",
    "X........O.....................X",
    "X.........O....................X",
    "X..........O...................X",
    "X..........O...................X",
    "X.........O....................X",
    "X........O.....................X",
    "X.......O......................X",
    "X......OOOOOOO.................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X.........OOOOOOOOOOO..........X",
    "X.........OOOOOOOOOOO..........X",
    "X.........OOOOOOOOOOO..........X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "X..............................X",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
};
```

`icon_clock` (clock: white frame, accent hands + ticks):

```c
static const char icon_clock[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..X...........OOO............X..",
    "..X...........OOO............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X.OO.........OO.........OO.X..",
    "..X.OO.........OO.........OO.X..",
    "..X.OO..OOOOOOOOOOOOOOOO..OO.X..",
    "..X.....OOOOOOOOOOOOOOOO.....X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X............OO............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...........OOO............X..",
    "..X...........OOO............X..",
    "..X..........................X..",
    "..X..........................X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
    "................................",
    "................................",
};
```

`icon_file` (text file: white page, accent text lines):

```c
static const char icon_file[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X...OOOOOOOOOOOOOOOOOOO....X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOOOOOOOOOO..........X..",
    "..X...OOOOOOOOOOOOO..........X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
};
```

`icon_folder` (folder: white outline, accent tab):

```c
static const char icon_folder[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..XOOOOOOOOOOOOOOOOOOOOOOOOOOX..",
    "..XOOOOOOOOOOOOOOOOOOOOOOOOOOX..",
    "..XOOOOOOOOOOOOOOOOOOOOOOOOOOX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
    "................................",
    "................................",
};
```

`icon_unknown` (unknown file: white card, accent `?`):

```c
static const char icon_unknown[32][33] = {
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
    "X..............................X",
    "X..............................X",
    "X..XXXXXXXXXXXXXXXXXXXXXXXXXX..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X.....OOOOOOOO...........X..X",
    "X..X.............O..........X..X",
    "X..X.............O..........X..X",
    "X..X........OOOOO...........X..X",
    "X..X........O...............X..X",
    "X..X........O...............X..X",
    "X..X........O...............X..X",
    "X..X.........OOO............X..X",
    "X..X...........O............X..X",
    "X..X...........O............X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X...........O............X..X",
    "X..X...........O............X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..X........................X..X",
    "X..XXXXXXXXXXXXXXXXXXXXXXXXXX..X",
    "X..............................X",
    "X..............................X",
    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
};
```

`icon_image` (image fallback: white frame, accent sun + mountains):

```c
static const char icon_image[32][33] = {
    "................................",
    "................................",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X...OOOOO..................X..",
    "..X...OOOOO..................X..",
    "..X...OOOOO..................X..",
    "..X...OOOOO..................X..",
    "..X...OOOOO..O...............X..",
    "..X.........O.O..............X..",
    "..X........O...O.............X..",
    "..X.......O.....OOOO.........X..",
    "..X......O......OO..O........X..",
    "..X.....O......O..O..O.......X..",
    "..X....O......O....O..O......X..",
    "..X...O......O......O..O.....X..",
    "..X..O......O........O..O....X..",
    "..X.O......O..........O..OOOOX..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..X..........................X..",
    "..XXXXXXXXXXXXXXXXXXXXXXXXXXXX..",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
};
```

- [ ] **Step 2: Replace `draw_icon` with `draw_icon2` (lines 542-550)**

```c
static void draw_icon2(int x, int y, const char art[32][33],
                       unsigned int fg, unsigned int accent) {
    for (int r = 0; r < 32; r++)
        for (int c = 0; c < 32; c++) {
            char ch = art[r][c];
            if (ch == 'X') fb_put(x + c, y + r, fg);
            else if (ch == 'O') fb_put(x + c, y + r, accent);
        }
}
```

(It uses `fb_put` so it is clip-aware for desktop icons; the dock calls it after the clip reset, so it is unclipped there too. `draw_icon` and `draw_icon_art` are deleted.)

- [ ] **Step 3: Update dock icon calls**

In `draw_dock`, replace `draw_icon(dx0 + DOCK_PAD_X, iy, icon_term, COL_ICON_FG);` → `draw_icon2(dx0 + DOCK_PAD_X, iy, icon_term, col_icon_fg, col_accent);`, the clock launcher line likewise, and the running-window loop line `draw_icon(ix, iy, ic, COL_ICON_FG);` → `draw_icon2(ix, iy, ic, col_icon_fg, col_accent);`.

- [ ] **Step 4: Update desktop icons + `.ico` fallback**

In `draw_desktop_icons` (lines 778-786) replace the `switch` body:

```c
        switch (files[i].kind) {
        case K_FOLDER: draw_icon2(x, y, icon_folder, col_icon_fg, col_accent); break;
        case K_ICO:    draw_ico_file(i, x, y); break;
        case K_TEXT:   draw_icon2(x, y, icon_file, col_icon_fg, col_accent); break;
        default:       draw_icon2(x, y, icon_unknown, col_icon_fg, col_accent); break;
        }
        if (y + ICON_H + 2 < clip_y1)
            fb_text(x, y + ICON_H + 2, files[i].name, col_icon_fg,
                    wp_top);
```

In `draw_ico_file` (line 768) replace the fallback with `draw_icon2(x, y, icon_image, col_icon_fg, col_accent);`.

- [ ] **Step 5: Remove the `COL_ICON_FG` define and the old `draw_icon`/`draw_icon_art`**

Delete `#define COL_ICON_FG 0xE8EEF8` (line 37). Delete `draw_icon_art` (lines 651-657). Ensure no other reference to `COL_ICON_FG` or `draw_icon`/`draw_icon_art` remains (`grep -n COL_ICON_FG programs/musl/wm.c` must return nothing).

- [ ] **Step 6: Build and test**

Run: `make` then `python3 scripts/configtest.py` and `python3 scripts/notepadtest.py`
Expected: clean build (no warnings, no unused-symbol errors); both scripts PASS.
Visual check via `python3 scripts/guitester.py`: dock shows white-outline icons with accent detail (e.g. term prompt `>_` in accent), desktop `demo.ico` still renders its decoded green disc (`.ico` path untouched).

- [ ] **Step 7: Commit**

```bash
git add programs/musl/wm.c
git commit -m "feat: two-color dock and desktop icons"
```

---

### Task 4: Menu + dialog redesign (rounded, accent frame, hover)

Context menu and create dialog get rounded corners, an accent frame, and a hover highlight on the menu item under the cursor.

**Files:**
- Modify: `programs/musl/wm.c:285-300` (menu defines + state, add `g_mx`/`g_my`/`last_hover`), `:420-430` (forward decl `draw_menu(int,int)`), `:486-524` (`composite_rect` calls `draw_menu(g_mx,g_my)`), `:812-876` (`draw_menu` + `draw_dialog`)

**Interfaces:**
- Consumes: `col_accent`, `col_menu_bg`, `col_menu_fg` globals; `fb_round_fill` from Task 2; `menu_item_at()` unchanged.
- Produces: `static void draw_menu(int mx, int my)` (signature change from `draw_menu(void)`); module globals `static int g_mx, g_my;` and `static int last_hover = -1;`.

- [ ] **Step 1: Remove the menu color macros**

Delete `#define COL_MENU_BG 0x20283A`, `#define COL_MENU_BORDER 0x4A7AB5`, `#define COL_MENU_FG 0xFFFFFF` (lines 291-293). Replace them with the two globals (declared here; `col_accent` already exists from Task 2):

```c
static unsigned int col_menu_bg = 0x20283A;
static unsigned int col_menu_fg = 0xFFFFFF;
```

Add the `fb_round_fill` helper (all-four-corners rounding) next to
`fb_round_fill_top`:

```c
// Same as fb_round_fill_top, rounding all four corners.
static void fb_round_fill(int x, int y, int w, int h, int r,
                          unsigned int rgb) {
    for (int yy = 0; yy < h; yy++) {
        int cut;
        if (yy < r) cut = r - 1 - yy;
        else if (yy >= h - r) cut = r - 1 - (h - 1 - yy);
        else cut = 0;
        fb_hline(x + cut, y + yy, x + w - cut, rgb);
    }
}
```

Then replace their uses with `col_menu_bg`/`col_accent`/`col_menu_fg`.

- [ ] **Step 2: Add mouse-tracking globals**

Near the menu state (line 295), add:

```c
static int g_mx, g_my;              // last mouse position (for hover)
static int last_hover = -1;         // last hovered menu item (-1 = none)
```

- [ ] **Step 3: `draw_menu` with rounded corners + hover (lines 812-828)**

```c
static void draw_menu(int mx, int my) {
    if (!menu_open) return;
    int x = menu_x, y = menu_y;
    int mw = MENU_W;
    int mh = MENU_N * MENU_ITEM_H + 2 * MENU_BORDER;
    if (x + mw > (int)fb_w) x = (int)fb_w - mw;
    if (y + mh > (int)fb_h) y = (int)fb_h - mh;
    menu_draw_x = x;
    menu_draw_y = y;
    fb_round_fill(x, y, mw, mh, 3, col_accent);
    fb_round_fill(x + MENU_BORDER, y + MENU_BORDER, mw - 2 * MENU_BORDER,
                  mh - 2 * MENU_BORDER, 2, col_menu_bg);
    int hi = menu_item_at(mx, my);
    for (int i = 0; i < MENU_N; i++) {
        int iy = y + MENU_BORDER + i * MENU_ITEM_H;
        unsigned int bg = col_menu_bg;
        if (i == hi) {
            fb_fill(x + MENU_BORDER + 2, iy, mw - 2 * MENU_BORDER - 4,
                    MENU_ITEM_H, col_accent);
            bg = col_accent;
        }
        fb_text(x + 10, iy + 3, menu_items[i], col_menu_fg, bg);
    }
}
```

- [ ] **Step 4: `draw_dialog` with rounded corners + accent frame (lines 855-876)**

Replace the body:

```c
static void draw_dialog(void) {
    if (!dlg_open) return;
    int x = (int)fb_w / 2 - 180;
    int y = (int)fb_h / 3;
    dlg_draw_x = x;
    dlg_draw_y = y;
    fb_round_fill(x, y, 360, 88, 3, col_accent);
    fb_round_fill(x + 1, y + 1, 358, 86, 2, col_menu_bg);
    const char *title = dlg_mode
        ? "\xd0\x98\xd0\xbc\xd1\x8f \xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb9 \xd0\xbf\xd0\xb0\xd0\xbf\xd0\xba\xd0\xb8:"
        : "\xd0\x98\xd0\xbc\xd1\x8f \xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xb0:";
    fb_text(x + 10, y + 8, title, col_menu_fg, col_menu_bg);
    int bx = x + 10, by = y + 36, bw = 340, bh = 20;
    fb_fill(bx, by, bw, bh, 0x101010);
    if (dlg_len)
        fb_text(bx + 4, by + 2, dlg_name, col_menu_fg, 0x101010);
    int cx = bx + 4 + dlg_len * 8;      // dialog names are ASCII only
    fb_fill(cx, by + 3, 2, 14, col_menu_fg);
}
```

- [ ] **Step 5: Wire hover + mouse coords into the main loop**

Update the forward declaration (line 429) to `static void draw_menu(int mx, int my);` and the call in `composite_rect` (line 514) to `draw_menu(g_mx, g_my);`.

At the top of the `for (;;)` loop (right after `aos_mouse(&mx, &my, &mb, &wheel);`, line 1025), add:

```c
        g_mx = mx;
        g_my = my;
        if (menu_open && (mx != last_mx || my != last_my)) {
            int hi = menu_item_at(mx, my);
            if (hi != last_hover) { last_hover = hi; redraw = 1; }
        }
```

Reset `last_hover = -1;` in the two press handlers whenever `menu_open` is set to 0 or 1: after `menu_open = 0;` on an item click (line 1100) and after `menu_open = 1;` when opening (line 1143).

- [ ] **Step 6: Build and test**

Run: `make` then `python3 scripts/notepadtest.py`
Expected: clean build; `notepadtest` PASS — the menu bg probe at (520,402) is interior menu bg (unchanged), the dialog probe at (400,285) is interior menu bg.
Visual via `guitester.py`: right-click desktop → menu has rounded top corners (corner pixel shows desktop gradient, not menu color); moving the cursor over «Новый файл» paints it with the accent background and white text.

- [ ] **Step 7: Commit**

```bash
git add programs/musl/wm.c
git commit -m "feat: rounded context menu and dialog with accent frame and hover"
```

---

### Task 5: term / clock / notepad consume the theme

**Files:**
- Modify: `programs/musl/term.c:10-11,72,82,84,185-203` (colors + `theme_load`)
- Modify: `programs/musl/clock.c:34,47,61,74,8-22` (colors + `theme_load`)
- Modify: `programs/musl/notepad.c:16-19,234,246,250,252-257,321-343` (colors + `theme_load`)

**Interfaces:**
- Consumes: `theme_load()` / `theme_color()` from Task 1 (already linked into these ELFs by Task 1's Makefile rules).

- [ ] **Step 1: `term.c`**

Add `#include "theme.h"` after `#include "aosabi.h"` (line 3). Replace lines 10-11:

```c
static unsigned int col_fg = 0xD8D8D8;
static unsigned int col_bg = 0x101010;
```

Replace the three uses of `COL_FG`/`COL_BG` (`render()`, lines 72, 82, 84) with `col_bg`/`col_fg` accordingly. In `main()` after the `w`/`h` assignment (line 199-200) and before `prompt()`:

```c
    theme_load();
    col_fg = theme_color("theme_text_fg", 0xD8D8D8);
    col_bg = theme_color("theme_text_bg", 0x101010);
```

- [ ] **Step 2: `clock.c`**

Add `#include "theme.h"` after `#include "aosabi.h"` (line 3). In `main()` before the `MSG_CREATE` send (line 10):

```c
    unsigned int col_bg = 0x101010;
    unsigned int col_time = 0x5B93D8;
    unsigned int col_date = 0x2D496C;
    unsigned int col_sub = 0x162436;
    theme_load();
    col_bg = theme_color("theme_text_bg", 0x101010);
    col_time = theme_color("theme_accent", 0x5B93D8);
    col_date = (col_time >> 1) & 0x7F7F7F;
    col_sub = (col_time >> 2) & 0x3F3F3F;
```

Replace the literals in `aos_fill`/`aos_render_text` calls:
- line 34: `aos_fill(win, CW * 4, 0, 0, CW, CH, 0x000000);` → `... col_bg);`
- line 47: `aos_render_text(win, CW * 4, 16, 16, buf, 0x00FF80, 0x000000);` → `... col_time, col_bg);`
- line 61: `... d, 0x9090D0, 0x000000);` → `... col_date, col_bg);`
- line 74: `... sub, 0x4050A0, 0x000000);` → `... col_sub, col_bg);`

- [ ] **Step 3: `notepad.c`**

Add `#include "theme.h"` after `#include "aosabi.h"` (line 5). Replace lines 16-19:

```c
static unsigned int col_fg = 0xD8D8D8;
static unsigned int col_bg = 0x101010;
static unsigned int col_status_bg = 0x232C40;
static unsigned int col_status_fg = 0xD8D8D8;
```

Replace `COL_FG`/`COL_BG`/`COL_STATUS_BG`/`COL_STATUS_FG` uses in `render()` (lines 234, 246, 250, 252-257) with `col_bg`/`col_fg`/`col_status_bg`/`col_status_fg`. In `main()` before `render()` (line 343):

```c
    theme_load();
    col_fg = theme_color("theme_text_fg", 0xD8D8D8);
    col_bg = theme_color("theme_text_bg", 0x101010);
    col_status_bg = theme_color("theme_dock_bg", 0x232C40);
    col_status_fg = theme_color("theme_text_fg", 0xD8D8D8);
```

- [ ] **Step 4: Build and test**

Run: `make` then `python3 scripts/configtest.py`, `python3 scripts/notepadtest.py`, `python3 scripts/manytest.py`, `python3 scripts/ipctest.py`
Expected: clean build; all four PASS. Defaults keep every anchor: term/notepad text `0xD8D8D8` on `0x101010`, notepad content bg `0x101010`, status bar `0x232C40`.
Visual via `guitester.py`: click the dock clock launcher → clock shows the time in accent blue `0x5B93D8` on `0x101010`, date dimmed.

- [ ] **Step 5: Commit**

```bash
git add programs/musl/term.c programs/musl/clock.c programs/musl/notepad.c
git commit -m "feat: term, clock, and notepad read text/accent colors from the theme"
```

---

### Task 6: Docs + full regression

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: Document the theme in `AGENTS.md`**

Under the WM notes, add a `### Theme (config-driven)` subsection: the ten `theme_*` keys with defaults, the shared loader `programs/musl/theme.c` + `theme.h` (musl build, not `libaos`), that `wm.c`'s `load_wallpaper_config()` is replaced by `theme_load()`, the corner radii (windows 4 top, dock 6 top, menu/dialog 3), two-color icons (`draw_icon2`: `'X'`=white, `'O'`=`theme_accent`), the focus frame (`theme_border_focus`/`theme_title_focus`), the menu hover highlight, and that `sys/config.cfg` now carries the theme keys. Update the "Screen layout" color notes: title focus `0x4E86C7`, frame focus `0x6B9BD2`, dock bg `0x232C40`, menu frame accent. Note `configtest.py` Boot B seeds `theme_accent`/`wallpaper_top` and asserts the dock accent line + desktop pixel.

- [ ] **Step 2: Full regression**

Run: `make` then `make test`
Expected: `ALL <N> TESTS PASSED` (configtest, manytest, notepadtest, ipctest, plus the other boot-time tests). Re-run `python3 scripts/guitester.py` once for a manual screenshot sanity pass.

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md
git commit -m "docs: document the config-driven dark theme"
```

---

## Self-Review

**Spec coverage**
- Theme loader (`theme_load`/`theme_color`) → Task 1.
- Ten keys + defaults in `kernel/config.c` default file → Task 1 (Step 3).
- WM `load_wallpaper_config()` replaced by `theme_load()` → Task 1 (Step 5).
- Rounded window corners (r=4 top) + focus border + title gradient → Task 2 (Steps 3-4).
- Dock (r=6 top, `theme_dock_bg`, accent top line, lighter edge, accent dot) → Task 2 (Step 5).
- Two-color icons (white + accent) for dock + desktop; `.ico` decode unchanged → Task 3.
- Menu/dialog rounded (r=3), accent frame, hover highlight, input field stays `0x101010` → Task 4.
- Apps: term/clock/notepad consume theme (clock bg `theme_text_bg`, time `theme_accent`, dimmed date) → Task 5.
- Error handling: missing config → defaults (loader no-ops, `theme_color` falls back); unknown/duplicate keys ignored (first wins); invalid hex → default → built into Task 1's `theme.c`.
- Verification: `make` clean; `notepadtest`/`manytest`/`ipctest`/`configtest` green; screenshot checks; config-edit-visible-without-rebuild proven by `configtest.py` Boot B → Tasks 2/5/6.

**Placeholder scan:** No TBD/TODO; every step has concrete code or an exact command with expected output.

**Type consistency:** `theme_load()`/`theme_color()` used consistently everywhere; `draw_menu(int,int)` signature matches across forward declaration, `composite_rect`, and definition; `draw_icon2` signature matches all dock/desktop/fallback call sites; color globals (`col_title`, `col_title_focus`, `col_border`, `col_border_focus`, `col_dock_bg`, `col_accent`, `col_menu_bg`, `col_menu_fg`, `col_icon_fg`) are declared once (Task 2) and referenced identically in Tasks 3-4.
