# WM & apps: modern dark theme (config-driven) — design

Date: 2026-08-06

## Problem

- The GUI looks dated: flat black window borders, monochrome 1-bit dock icons,
  hardcoded ad-hoc colors scattered across `wm.c`, `term.c`, `clock.c`,
  `notepad.c`.
- There is no way to restyle the desktop without recompiling the kernel: every
  color is a `#define` or literal in program source.

## Goal

- A modern dark theme with:
  - rounded window/dock/menu corners (stair-stepped, no anti-aliasing);
  - two-color (outline + accent) dock and desktop icons;
  - visible focus highlight (accent border + brighter title on the focused
    window, dimmed on the rest);
  - a unified palette shared by WM, terminal, clock, notepad;
  - **all theme colors loaded from `sys/config.cfg`** (kernel-provided defaults
    on first boot), so the look can change without a rebuild.

## Scope

- `programs/wm.c` (window chrome, dock, icons, menu, dialog, desktop icons).
- `programs/libaos.c` + `programs/libaos.h` (shared theme loader).
- `programs/term.c`, `programs/clock.c`, `programs/notepad.c` (consume theme).
- `kernel/config.c` (default theme keys in the generated `sys/config.cfg`).
- `scripts/*` test updates only where asserted colors actually change.

Out of scope: window shadows, real alpha blending, hover-scale dock animations,
gradient title bars with more than 2–3 flat tones.

## Architecture

### Theme loading (`programs/libaos.c`)

All programs link `libaos.o`, so the theme loader lives there — no kernel
headers needed from ring 3, and every app shares one parser.

- `void theme_load(void)` — reads `sys/config.cfg` once, fills a static table
  of known keys; called at the top of each app's startup (and from the WM,
  replacing its local `load_wallpaper_config()`).
- `unsigned int theme_color(const char *key, unsigned int fallback)` — returns
  the parsed value or the fallback when the key is missing/invalid.

Keys and defaults (defaults equal today's anchor colors, so tests stay green):

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

Existing `timezone`, `wallpaper_top`, `wallpaper_bot` stay as-is; the WM's
`load_wallpaper_config()` is replaced by `theme_load()` so wallpaper and theme
colors come from one parser. (The kernel keeps its own `config.c` parser — it
runs in ring 0 before the WM exists and cannot call libaos.)

`kernel/config.c` default file gains the ten `theme_*` lines. Test anchors
(`0x1A2030` wallpaper top, `0x20283A` menu bg, `0x101010` content bg,
`0xD8D8D8` bright text) are all preserved by these defaults.

### Rounded corners (`programs/wm.c`)

- `static void fb_round_fill(int x, int y, int w, int h, int r, unsigned int rgb)`
  — fills the rect like `fb_fill` but skips a stair-stepped `r`-pixel L-shape in
  each corner. Corner pixels are left untouched, so whatever is behind them
  (desktop gradient or another window) shows through — honest compositing
  without alpha. Rows are drawn with one `u32` loop like `fb_fill`; only the
  first/last `r` rows shorten their span.
- Radii: windows 4 (top corners), dock 6 (top corners), menu 3, dialog 3.

### Window chrome

- Frame: 1 px. Focused window = `theme_border_focus`, unfocused =
  `theme_border`. Top corners rounded (radius 4).
- Title bar: `theme_title_focus` / `theme_title`; white text; a 2–3 tone
  vertical gradient (lighter strip near the top edge) for depth, drawn as a
  couple of short `fb_fill` rows.
- Close button, text offset, clip handling: unchanged.
- Hit-testing (`win_index_at`, drag, `dock_hit`) is bounding-box based and is
  **not** changed — rounded corners are cosmetic only.

### Dock

- Top corners rounded (radius 6), background `theme_dock_bg`, thin accent line
  on top, slightly lighter top edge.
- Active-window indicator dot: 4×4 `theme_accent` (as today).

### Two-color icons

- `static void draw_icon2(int x, int y, const char art[32][33], unsigned int
  fg, unsigned int accent)` — `'X'` draws `fg`, `'O'` draws `accent`, `.` is
  transparent. Used for both dock and desktop icons (replacing the 1-bit
  `draw_icon` / `draw_icon_art`).
- Art re-drawn per icon:
  - terminal: white frame + dark screen + accent `>_` prompt;
  - clock: white frame + accent hands;
  - unknown/file/folder/image: white outline + accent detail.

### Menu / dialog

- Rounded corners (radius 3), `theme_menu_bg` fill, `theme_accent` frame.
- Hover highlight: the menu item under the cursor gets `theme_accent`
  background with `theme_menu_fg` text.
- Dialog input field stays `0x101010` (a `notepadtest` probe reads the menu bg
  at a specific pixel, and the input box is a separate visual element).

### Apps

- `term.c`: `theme_load()`; fg/bg = `theme_text_fg` / `theme_text_bg`
  (defaults `0xD8D8D8` / `0x101010` — same as today).
- `notepad.c`: same text colors; status bar = `theme_dock_bg` bg +
  `theme_text_fg` fg.
- `clock.c`: background `theme_text_bg`; time in `theme_accent`, date in a
  dimmed tone, replacing the current green-on-black.

## Error handling

- Missing `sys/config.cfg`: `theme_load()` no-ops, every `theme_color()` falls
  back to its default → identical to today's hardcoded palette.
- Unknown/duplicate keys: ignored; first occurrence wins.
- Invalid hex value: key falls back to its default.

## Verification

- `make` builds cleanly.
- Boot QEMU; `notepadtest.py`, `manytest.py`, `ipctest.py`, `configtest.py`
  stay green (anchor colors unchanged by defaults).
- Screenshot checks via `scripts/guitester.py`:
  - dock has rounded top corners and two-color icons;
  - focused window has an accent frame, unfocused has a dim frame;
  - menu and dialog have rounded corners and hover highlight;
  - clock shows theme colors, terminal/notepad keep `0x101010`/`0xD8D8D8`.
- Edit `sys/config.cfg` (e.g. `theme_accent=0xFF00FF`), reboot, confirm the
  accent color changed without a kernel rebuild.
