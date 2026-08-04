# Desktop File Icons (.ico), Context Menu and Notepad — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Turn the desktop into a light file surface: files from the SFS ramdisk are shown as icons on the desktop, `.ico` files are decoded and rendered as their actual image, a right-click context menu creates new text files and folders, and a new `notepad` program opens/edits/saves text files.

**Architecture:** All user-visible work is done by `programs/wm.c` (desktop icons, context menu, name-input dialog, icon open dispatch) and a new `programs/notepad.c` (GUI text editor). A shared pure-C ICO decoder is added as `programs/ico.c`/`programs/ico.h` and linked only into the WM. One small kernel change (Ctrl+letter → control code in `terminal_scan_event`) gives the notepad a distinguishable Ctrl+S save key. A demo `.ico` is embedded into the ramdisk at build time via `gen_progs.py` so the feature is visible and pixel-testable on first boot.

**Tech Stack:** C11 (`-std=c11 -m32 -ffreestanding -nostdlib`), ring-3 programs linked against `programs/libaos.o`. No libc. No test framework — verification is QEMU pixel checks via `scripts/guitester.py` (extended with a right-click command).

## Global Constraints

- Screen is 1024×768, 32bpp, from GRUB2. Colors: desktop `0x1A2030`, dock bg `0x20283A`, title focused `0x4A7AB5`, icon fg `0xE8EEF8`.
- SFS ramdisk: 160 KB at `0x200000`, 64 entries, flat (no real directories). A **folder** is represented as an empty file whose name ends with `/` (this keeps kernel SFS untouched).
- `make` must stay warning-free (`-Wall -Wextra`).
- Desktop icon grid: `GRID_X0 16`, `GRID_Y0 24`, icon 32×32, cell stride 52, row height 56. Files are listed with `fs_list_get`; entries under `bin/` are hidden (they live in the dock).
- `.ico` decode target size: 32×32; decoder returns a full 32×32 `0xAARRGGBB` buffer (transparent outside the fitted image).
- GUI key codes: UP `0x0101`, DOWN `0x0102`, LEFT `0x0103`, RIGHT `0x0104`, HOME `0x0105`, END `0x0106`, DEL `0x0107`, Esc `27`, Enter `\r`, Backspace `\b`.
- QEMU monitor socket `/tmp/aos-gui.sock`; serial log `/tmp/aos-gui.log`.
- Every task ends with a build, a QEMU run, pixel assertions via `guitester.py`, and a commit (in this offline environment builds/tests must be run by the human).

---

### Task 1: ICO decoder (`programs/ico.h`, `programs/ico.c`)

**Files:**
- Create: `programs/ico.h`, `programs/ico.c`

**Interface:**
```c
int ico_decode(const unsigned char *data, unsigned int size,
               unsigned int max_w, unsigned int max_h,
               unsigned int *out_w, unsigned int *out_h,
               unsigned int *out_px);
```
Returns 0 on success, `-1` if not a valid ICO, `-2` if the chosen image uses an unsupported encoding (e.g. embedded PNG). On success `*out_w = max_w`, `*out_h = max_h` and `out_px` holds `max_w * max_h` `0xAARRGGBB` pixels, the image nearest-neighbour scaled to fit and centered, transparent margins.

- [x] **Step 1: Implement the decoder**

  - Header: reserved==0, type==1, count>=1; bounds-check every read against `size`.
  - Pick the directory entry whose `width*height` is closest to `max_w*max_h` (0 → 256).
  - Reject PNG-embedded images (signature `0x89 'P' 'N' 'G'`).
  - Parse `BITMAPINFOHEADER` (40 bytes): width, height/2, bpp. Palette for bpp ≤ 8 (`biClrUsed` else `1<<bpp`, 4 bytes/entry `BGRA`).
  - XOR rows are bottom-up, stride `((w*bpp + 31)/32)*4`; AND mask after XOR, stride `((w+31)/32)*4`, 1-bit per pixel, `1` = transparent.
  - Support bpp 32/24/8/4/1. For 32bpp also honor the alpha byte (`< 0x80` → transparent).
  - Scale per output pixel by sampling source `sx = ox*w/out_w`, `sy = oy*h/out_h` directly from the source rows (no big temp buffer needed).

- [x] **Step 2: Commit** `feat: add pure-C .ico image decoder for user programs`

---

### Task 2: Desktop file icons in the WM (`programs/wm.c`)

**Files:**
- Modify: `programs/wm.c`

- [x] **Step 1: Icon art + file table**

  Add 32×32 1bpp art: `icon_folder`, `icon_file`, `icon_image` (falls back for undecodable `.ico`); reuse `icon_unknown`.
  Add `struct dent { char name[28]; int kind; } files[64]`, `nfiles`, `files_dirty`.
  Kinds: `K_FOLDER` (name ends `/`), `K_TEXT` (`*.txt`), `K_ICO` (`*.ico`), `K_OTHER`. Entries under `bin/` are skipped.

  `refresh_files()` re-lists via `fs_list_get` only when `files_dirty`.

- [x] **Step 2: Drawing**

  `fb_put()` — clip-aware single-pixel write. `draw_icon_art()` — 1bpp art through `fb_put`. `fb_text()` — renders a UTF-8 string into `scratch` (slab 0) and blits with clipping (generalises `draw_title`).
  `draw_desktop_icons()` — grid; for `.ico` files `fs_read` into an 8 KB buffer, `ico_decode`, blit opaque pixels; else draw the right art; label under the icon. Called in `composite_rect` right after the desktop fill, before windows.

- [x] **Step 3: Hit-testing + open**

  `icon_at(mx,my)` — icon rect hit. In the left-press handler, after window/close tests, if an icon is hit: `K_TEXT`/`K_OTHER` → `spawn("bin/notepad", name, getpid())`; `K_ICO`/`K_FOLDER` → no-op.

- [x] **Step 4: Build + pixel test** boot with the demo `.ico` embedded (Task 6), assert its green icon pixels at grid position of `demo.ico`. Commit `feat: render desktop file icons, decode .ico on the desktop`

---

### Task 3: Context menu + name dialog (`programs/wm.c`)

- [x] **Step 1: Menu state & drawing**

  `menu_open/menu_x/menu_y`, items `"Новый файл"`, `"Новая папка"` (UTF-8 escapes). `draw_menu()` overlay drawn after the dock in `composite_rect`.

- [x] **Step 2: Interaction**

  Right-press on desktop (not dock, not a window) opens the menu. Left-press on an item runs it and closes; left-press elsewhere closes; right-press reopens at cursor.

- [x] **Step 3: Name-input dialog**

  `dlg_open/dlg_mode/dlg_name[40]/dlg_len`. `draw_dialog()` overlay. While open the WM consumes `MSG_KEY` itself (printable ASCII, backspace, Enter=confirm, Esc=cancel) instead of forwarding. On confirm: file → `fs_write(name,"",0)`; folder → `fs_write(name+"/","",0)`. Then `files_dirty=1`, `redraw=1`.

- [x] **Step 4: Build + pixel test** right-click desktop, check menu box, click "Новый файл", type a name, Enter, assert the new icon appears. Commit `feat: desktop context menu creates files and folders`

---

### Task 4: Notepad (`programs/notepad.c`)

- [x] **Step 1: Text model**

  `lines[NMAX][TW]` codepoints (`NMAX=200`, `TW=80`), `llen[]`, `nlines`. Cursor `(crow,ccol)`, `scroll`. Ops: type (UTF-8 codepoint insert), Enter (split), Backspace, Del, Left/Right/Up/Down/Home/End. `utf8_encode`/`utf8_decode`.

- [x] **Step 2: Load/Save + window**

  `get_args()` → filename (default `untitled.txt`); load via `fs_read` + UTF-8 decode. Save builds UTF-8 bytes + `\n` per line, `fs_write`. Status bar row shows `Файл: <name> [Ctrl+S сохранить]` and a brief `сохранено`/`ошибка` after save. Window 80×26 chars (640×416) via `MSG_CREATE`, like `term.c`.

- [x] **Step 3: Keys** — `0x13` (Ctrl+S) saves; `\r`,`\b`, GUI_KEY_* navigate/edit. `MSG_CLOSE` → exit.

- [x] **Step 4: Build + pixel test** open notepad from a desktop icon, type via `sendkey`, Ctrl+S, assert status shows saved and the file exists (via shell `ls`). Commit `feat: add notepad text editor with Ctrl+S save`

---

### Task 5: Kernel Ctrl+letter control codes (`kernel/terminal.c`)

- [x] **Step 1: Mask ctrl+letter**

  In `terminal_scan_event` after `map_scancode`: if `ctrl_pressed` and the mapped codepoint is `a-z`/`A-Z`, return `cp & 0x1F` (so Ctrl+S = `0x13`). The shell already ignores `k < 0x20`.

- [x] **Step 2: Commit** `feat: kernel emits control codes for Ctrl+letter key chords`

---

### Task 6: Build plumbing (`Makefile`, `scripts/gen_progs.py`, `kernel/progload.c`)

- [x] **Step 1: Makefile** add `notepad` to `PROGRAMS`; add `programs/ico.o` to the `wm.elf` link rule; add `scripts/demo.ico` rule via `scripts/gen_ico.py`; pass `--data demo.ico=scripts/demo.ico` to `gen_progs.py`.

- [x] **Step 2: gen_progs.py** accept `--data name=path`, emit an `embedded_data[]` table.

- [x] **Step 3: progload.c** `load_embedded_programs()` also writes embedded data files when absent.

- [x] **Step 4: gen_ico.py** generate a 32×32 32bpp `.ico` (e.g. green circle) with a correct AND mask.

- [x] **Step 5: Commit** `feat: embed demo.ico and notepad into ramdisk`

---

### Task 7: Test harness + docs

- [x] **Step 1: guitester.py** add `rclick <x> <y>` (mouse_button 2).
- [x] **Step 2: AGENTS.md** document desktop icons, context menu, notepad, Ctrl+S, the folder-`/` convention, demo.ico.
- [x] **Step 3: Commit** `test: add right-click support to guitester; docs`
