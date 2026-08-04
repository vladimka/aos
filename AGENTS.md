## Terminal (`kernel/terminal.c`)

- **Line editing**: cursor moves left/right via arrow keys, backspace deletes whole UTF-8 character (scans for start byte), delete key (E0 53) removes character at cursor
- **Home/End**: E0 47 / E0 4F jumps cursor to start/end of line
- **Command history** (`hist_*`): circular buffer of 16 entries, Up/Down arrows browse older/newer; Enter pushes current line as new entry
- **Tab completion**: searches built-in `format` + every directory in PATH for prefix match; single match auto-completes inline, multiple matches prints list and re-prompts
- **Keyboard layouts**: US QWERTY and Russian ЙЦУКЕН; Left Ctrl + Left Shift held simultaneously toggles `ru_layout` flag; scancode mapped to Unicode codepoint (U+0400–U+04FF for Cyrillic, shared ASCII punctuation)
- **UTF-8 output**: `insert_codepoint()` encodes codepoint as 1–3 byte UTF-8 sequence into `line_buf`; `line_redraw_from()` re-renders from a given byte offset
- **Caps Lock** (`scancode 0x3A`): toggles `caps_lock` flag, affects only US layout letters
- **Serial console input**: `timer_handler` (`kernel/kernel.c`) drains the COM1 FIFO (`serial_available`/`serial_read`) into `terminal_serial_byte()`. While a user program is running, serial bytes are pushed to the key queue (`key_queue_push`) instead of the line buffer
- **Key queue**: circular `key_queue[64]` for user programs. When `user_program_active()`, printable keys (and serial bytes) push to the queue; `terminal_read_key()` pops (returns -1 when empty). `SYS_READ_KEY` blocks in the kernel via `sti; hlt; cli` loop

# AOS — minimal x86 kernel

## Build & run

```
make           # aos.iso (GRUB2 rescue ISO)
make run       # qemu-system-i386 -cdrom aos.iso
make clean     # full clean, removes kernel/progs.c too
```

`make` is idempotent. NOTE: Makefile has `.SECONDARY: $(KERNEL_OBJS) $(PROG_OBJS) $(PROG_ELFS)` — without it GNU make auto-deletes the program `.o` files after a clean build (they're built by implicit `programs/%.o` pattern rules and the `.d` files don't exist yet on the first pass), making every subsequent `make` rebuild everything.

No dedicated test, lint, or typecheck commands.

## Architecture

- **Multiboot2** boot protocol (magic `0xE85250D6`), GRUB2 entry: `multiboot2 /boot/aos.elf`
- Kernel loaded at **1 MB** (`linker.ld`), linked with `-nostdlib -m32 -m elf_i386`
- All kernel C code compiled `-ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mno-sse -mno-mmx -mno-80387`
- No libc; own `string.c` (`strcmp`, `strncpy`, `strlen`)
- Include paths: `-Ikernel -Idrivers -Iarch/i386 -Iboot`
- **64 KB ramdisk at `0x200000`** — flat SFS (Simple File System), `SFS_MAX_FILES=64`
- **18 syscalls via `int 0x80`** (DPL 3 gate, `idt_install_irq_flags(0x80, isr128, 0xEE)`), R/O user-level interface in `programs/libaos.c`
- `printf()` in `kernel/printf.c` writes to **both** VGA and COM1 (used for all kernel banners)
- **Privilege separation**: paging enabled (`kernel/paging.c`), user programs run in **ring 3** via TSS (`arch/i386/gdt.c`, `kernel/user.c`/`user_tramp.S`), kernel stays in ring 0

### Memory model (after `paging_init`)

- Identity map 0..256 MB via 64 page tables; PDEs + PTEs for the user area carry the user bit
- **User area** `0x01000000..0x01C00000` (PD 4..6): program text/rodata/data load at `0x01000000`, bump-allocator heap at `0x01100000`, user stack top at `0x01804000`
- TSS (`SEL_TSS` = 0x28) `esp0` = top of `sys_stack[8192]`; CPU switches to it on any ring 3 → ring 0 transition
- `launch_user_asm` (`kernel/user_tramp.S`) pushes SS/ESP/EFLAGS/CS/EIP (EFLAGS=0x202) and `iret`s to ring 3; `user_exit_asm` restores the saved kernel `esp` and `ret`s back into the shell as if the launch never happened
- Framebuffer physical range (often above 256 MB) is mapped via `extra_pt[8][]` using `vga_get_fb_info()`

### Syscall hardening (`kernel/syscall.c`)

- All pointers validated: `in_user()` checks `p` is within `0x01000000..0x01804000`, `copy_user_str()` copies a bounded NUL-terminated string to `user_str[1024]`
- Invalid pointers return `-5` instead of crashing the kernel (verified by `test`)
- `SYS_READ_KEY` blocks with `sti; hlt; cli` (the `int 0x80` gate clears IF, so IF must be re-enabled or the keyboard IRQ can never wake it)

### ELF loader (`kernel/elf.c`)

- Segments load to `0x01000000..0x01100000` (`PROG_LOAD_MIN/MAX`); header, entry point, and every PT_LOAD range (vaddr + memsz) are bounds-checked against this window
- Uses `fs_read_at()` — no longer limited to a 4096-byte file read
- `programs/%.o` programs expose `void main(void)`; `programs/libaos.o` provides the `_start()` trampoline that calls `main()`, then `SYS_EXIT`

### Argument passing to `kernel_main`

In `boot/boot.S` the stack must have `%ebx` (info ptr) pushed first, `%eax` (magic) second — **wrong order causes null framebuffer info**.

### Framebuffer

- GRUB2 boot requests 1024×768×32 linear framebuffer (`tag type 5`)
- `drivers/vga.c` parses both **MB2 tags** (type 8, `magic == 0x36D76289`) and **MB1 info** (`flags[12]`)
- Falls back to VGA text mode (`0xB8000`) when framebuffer unavailable
- Font: CyrSlav-VGA16 8×16 bitmap (`drivers/fb_font.h`, built into kernel) with Cyrillic + Latin glyphs — replaces Lat15-VGA16 (Terminus)
- Unicode glyph lookup via `fb_unicode_map[]` (529 codepoint→glyph entries, binary search in `fb_cp_to_glyph()`)
- UTF-8 decoder in `vga.c` — state machine buffers up to 3-byte sequences, decodes codepoint via lookup table, renders bitmap (4-byte codepoints > 0xFFFF render as `?`)
- Cursor blink: timer fires `vga_cursor_toggle()` every 500 ticks (~500ms @ 1000Hz); framebuffer draws 2-pixel underline, text mode uses VGA hardware cursor registers
- Globals `__saved_mb_info` / `__saved_magic` set before `vga_init()` (`kernel/kernel.c`)
- `vga_init()` clamps `max_x/max_y` to `VGA_MAX_COLS-1`/`VGA_MAX_ROWS-1` (screen_mirror is sized `VGA_MAX_COLS×VGA_MAX_ROWS`)
- `vga_clear()` resets scrollback/cursor/UTF-8 state and fills both framebuffer and mirror with the background color

### Programs

All commands (`help`, `uptime`, `clear`, `echo`, `tick`, `info`, `reboot`, `panic`, `ls`, `cat`, `rm`, `shutdown`, `format`, `test`) are **standalone ELF32 programs** under `programs/`:

- Compiled for load address `0x01000000` (`programs/programs.ld`)
- Statically linked (`-static -nostdlib -n`), **no INTERP segment**
- Linked against `programs/libaos.o` (syscall wrappers + `_start` trampoline)
- Embedded into kernel at build time by `scripts/gen_progs.py` → `kernel/progs.c`
- Stored in ramdisk as `bin/<name>`; loaded by `elf_load()` which parses PT_LOAD segments
- Run in **ring 3**; `programs/test.c` exercises `malloc`/`free`, blocking `read_key()`, and syscall pointer validation

### Program search order (shell, `kernel/commands.c`)

Programs are located via a **PATH** variable (`char command_path[PATH_MAX]`, default `"bin"`, colon-separated dirs). For each `cmd`:

1. For each PATH directory: try `<dir>/<cmd>` on ramdisk
2. Fallback: try `<cmd>` directly (arbitrary path)

`format` is a **kernel built-in** (formats ramdisk, re-loads embedded programs). Built-in `setpath [dirs]` shows/sets PATH; `PATH_MAX=128`.

### Mouse & scrollback

- **PS/2 mouse** on IRQ12 (`drivers/mouse.c`): initialised with IntelliMouse (wheel) protocol via sample-rate sequence 200/100/80
- PS/2 controller byte status: mouse data has `status & 0x20` set; `kernel/kernel.c` `keyboard_handler()` reads **all** pending PS/2 bytes and routes mouse bytes to `mouse_process_byte()` (otherwise IRQ12 eats ACK bytes and IntelliMouse detection fails, `has_wheel=0`). `mouse_init()` runs under `cli()` with input-buffer drain before `sti()`.
- 4-byte wheel packets: byte 3 = signed wheel delta per IntelliMouse protocol. **PS/2 wheel byte: +1 = wheel down, -1 (0xFF) = wheel up** (QEMU `ps2.c`: `WHEEL_UP → mouse_dz--`, `WHEEL_DOWN → mouse_dz++`). Wheel **up** → `vga_scroll(+3)` (older content), wheel **down** → `vga_scroll(-3)` (live view). Note: QEMU monitor `mouse_move dz` **inverts** sign — `mouse_move 0 0 1` = wheel up (byte 0xFF), `mouse_move 0 0 -1` = wheel down (byte 0x01)
- **Wheel bursts never lose events**: `mouse_process_byte()` only accumulates deltas into `wheel_acc` (no redraw in IRQ); `keyboard_handler()`/`mouse_handler()` drain **all** pending PS/2 bytes per IRQ. `mouse_flush_wheel()` applies the whole accumulated delta as one `vga_scroll()` from the **main `hlt` loop** (IF=1). Rationale: a scroll redraw takes tens of ms in TCG; running it inside an IRQ (IF=0) overflows QEMU's 16-byte PS/2 queue and desyncs wheel packets (pcount), causing dropped/jumping scrolls. With the main-loop flush, IRQs keep draining during the redraw, so fast bursts are lossless at any speed (verified pixel-exact at 2 ms/event). Latency is ≤1 tick (1000 Hz) when idle.
- **XY axes**: `mouse_x += (signed char)packet[1]`, `mouse_y -= (signed char)packet[2]` — the Y is **negated** (`-=`). QEMU `ps2.c` does `mouse_dy -= move->value` (down-positive input from GTK becomes down-negative in the guest PS/2 stream; verified empirically: `mouse_move 0 50` moved the cursor up before the flip), so the negation makes `mouse_move 0 +dy` move the cursor **down**, matching X ("positive = right/down"). Both the 4-byte (wheel) and 3-byte packet paths apply it. Note: this follows QEMU's convention, not raw PS/2 hardware sign.
- **Scrollback buffer** in `vga.c`: circular `scrollback_lines[SCROLLBACK_LINES][VGA_MAX_COLS]` (`SCROLLBACK_LINES=512`) storing every line that scrolls off screen
- **`vga_scroll(delta)`**: adjusts `scroll_offset`; `scroll_offset == 0` = live view, `>0` = scrollback. Scrollback is **double-buffered**: full redraws render into a CPU staging buffer at `FB_STAGE_ADDR` (0x00C00000, ≤3 MB) and flip to VRAM with one `memcpy_fast`. Navigation **within** scrollback is incremental — `shift_view()` memmoves the staged framebuffer by `delta*16` pixel rows and re-renders only the newly exposed rows (`fb_render_row_fast` draws only set font pixels as u32 stores, background already filled). Full redraw happens on entry (`render_scrollback_full`) and return to live (`render_live_full`); `vga_reset_scroll()` returns to live. Fast glyph renderer requires `fb_bpp == 32` for the u32 path (falls back to byte writes otherwise)
- **TCG performance**: `kernel/string.c` provides `memcpy_fast()` (`rep movsl`/`movsb`) and `memset_fast32()` (`rep stosl`) — QEMU TCG runs string ops as a 32-byte/iter helper, several × faster than scalar 4-byte loops for the 3 MB stage↔VRAM copies and fills in `vga.c` (compiled with `-std=c11`, so use `__asm__ __volatile__`, not `asm`)
- **Escaping scrollback**: any non-mouse input resets scroll offset to live view (`vga_reset_scroll()` in `terminal_set_prompt()`)

### Tab completion cycling (`kernel/terminal.c`)

- **First Tab**: searches `format` + every PATH dir (`bin/*` by default) for prefix match; single match → auto-complete, common prefix → auto-complete prefix, no common prefix → list matches
- **Second Tab** (same partial word): replaces current word with next match in cycle; repeats on each press
- **Any other key**: resets cycle state

### Boot-time init order (`kernel/kernel.c`)

```
serial_init → vga_init → gdt_init → idt_init → paging_init → user_init
→ interrupts_init → pit_init_1000 → irq_install_handler(0, timer)
→ irq_install_handler(1, keyboard) → irq_install_handler(12, mouse)
→ mouse_init → fs_init → load_embedded_programs → terminal_init → hlt loop
```

PIT runs at **1000 Hz** (divisor 1193). `timer_handler` increments `tick`, drains COM1 into `terminal_serial_byte()`, toggles the cursor every 500 ticks.

### Interrupt acknowledgement (`kernel/interrupts.c`)

`irq_handler()` sends the EOI **before** running the handler. Rationale: the timer handler can switch to ring 3 (serial newline → command → `user_program_start`) and `iret` away before its own EOI, leaving IRQ0 in-service — which would block lower-priority IRQ1 (keyboard) for the whole user program lifetime and break `read_key()`. EOI-first is safe because the interrupt gates clear IF, so no nesting occurs during the handler.

## Key constraints

- `boot/boot.S` fields after checksum differ from raw offset — GRUB2 struct places video fields at +32 (after 5 address-field slots)
- `-serial stdio` conflicts with monitor; use `-serial file:serial.log` for debug
- QEMU 10.2.1 cannot boot via `-kernel`; must use `-cdrom aos.iso`
- ISO creation requires `grub-mkrescue` (GRUB 2.14)
- `gen_progs.py` must run after program ELFs are built; `kernel/progs.c` is generated (never edit by hand), always regenerated on `make`

## Window Manager & IPC debugging notes

- **`mcpy` semantics**: `n` is **word count** (unsigned int), NOT byte count. All callers must NOT multiply by 4. Fixed: draw_title, blit_content, save_snap, restore_snap were passing `width*4` — off-by-4x overflow that clobbered BSS with the desktop color (0x1A2030).
- **WM globals layout** (from `nm programs/wm.elf`): `snap@0x1001720`, `cur_y@0x1001b20`, `has_cur@0x1001b28`, `focus_pid@0x1001b2c`, `scratch@0x1001b30`, `fb_pitch@0x1001b34`, `fb_h@0x1001b38`, `fb_w@0x1001b3c`, `fb_addr@0x1001b40`, `wins@0x1001b60` (256 bytes, stride 32).
- **wm.c debug**: `composite()` prints `X` per call; window loop starts at `wins[0]` (0x1001b60). The desktop fill uses `fb_addr` global loaded fresh each call. If `fb_addr` is corrupted to 0x1A2030 → EIP 0x1000860, CR2 0x1a2030 (user-mode write to kernel identity page).
- **`in_user_area()`** accepts both user area (0x01000000–0x01804000) and slab range (0x03000000–0x04000000). If SYS_FILL/SYS_TEXT returns -5, check which check fails.
- **Window dragging**: `wm.c` drags a window when the left button is pressed on its **title bar** (`y` within `[y+BORDER, y+BORDER+TITLE_H)`), moving it with the cursor (grab offset `drag_dx/drag_dy`) and clamping to the screen; release ends the drag. Pressing anywhere else on a window only focuses it.
- **Cursor persistence (flicker)**: the WM draws the cursor as a VRAM overlay with a 16×16 snapshot. After **any** redraw (full `composite()` or a targeted `MSG_UPDATE` re-blit), the snapshot is stale — the code clears `has_cur` and re-draws the cursor at the current position. `MSG_UPDATE` re-blits only the affected window (plus higher-index windows, z-order) instead of a full composite; `cursor_overlaps()` re-draws the cursor only if the blit region touched its snapshot area. Without this, the cursor blinked off every clock tick and left ghost crosses.
- **Dock & z-order**: `wm.c` draws a centered bottom dock bar (`DOCK_H=52`) with launcher icons (`bin/term`, `bin/clock`) plus one icon per running window. Launcher click spawns the app (logs `wm: dock spawn failed` on failure) or `raise_pid()`s it if already running; a running window's dock icon raises it too. A 4x4 indicator dot (`COL_DOCK_ACTIVE`) under the icon marks the focused window. Z-order lives in `zorder[]`/`nz`; `raise_pid` forces a redraw on any z-order change, not just a focus change.
- **Test script**: `python3 scripts/guitester.py` — dumps PPM screenshots to `/tmp/aos-G*.ppm`, checks pixel colors. `QEMU` monitor socket at `/tmp/aos-gui.sock`. Serial log at `/tmp/aos-gui.log`. The guest cursor re-centers at (511,383) on boot but `/tmp/aos-mouse.state` keeps the last tracked position, so after **each** QEMU restart run `python3 scripts/guitester.py resetmouse 511 383` (or delete the state file) before the first click.
- **Screen layout**: term window at ~(20,20) 640×416px; clock at ~(44,48) 260×100px; desktop COLOR = 0x1A2030 = (26,32,48). Term content bg = 0x101010 = (16,16,16). Clock title unfocused = 0x2E4E7B, focused = 0x4A7AB5.
- **Desktop file icons**: `wm.c` lists the SFS ramdisk and shows every non-`bin/` entry as a 32×32 icon on a grid (`GRID_X0 16`, `GRID_Y0 24`, cell stride 52, row height 56; label under the icon). Kinds: `K_FOLDER` (name ends `/`), `K_TEXT` (`*.txt`), `K_ICO` (decoded via `programs/ico.c` — pure-C ICO/BMP decoder, up to 32×32, palette + 32bpp + AND mask; on decode failure falls back to `icon_image` art), `K_OTHER`. Left-click on a text/other file spawns `bin/notepad <name>`; `.ico` and folders are no-ops.
- **Context menu + create dialog**: right-click on desktop (not a window/dock) opens a menu (`MENU_W 176`) with «Новый файл» / «Новая папка». Left-clicking an item opens a modal name-input dialog (`dlg_open/dlg_mode/dlg_name[40]/dlg_len`); while open the WM consumes `MSG_KEY` itself (printable ASCII, `\b`, Enter=confirm, Esc=cancel) instead of forwarding. Confirm writes an empty SFS file; folders are names ending in `/`. Then `files_dirty=1`.
- **WM refresh/redraw pitfall**: the main loop runs `refresh_files()` (if `files_dirty` or every 128 iterations) **before** the message loop. So a file created by a message (dialog Enter) lists only on the *next* iteration, by which time the same-iteration `redraw=1` has already been consumed → the new icon never renders. Fix: `refresh_files()` sets `redraw=1` when `nfiles` changed (`if (nfiles != old_n) redraw = 1`).
- **Notepad**: `programs/notepad.c` GUI text editor (window 640×416 = 80×26 chars via `MSG_CREATE`), model `lines[200][80]` codepoints, cursor + `scroll`. Ctrl+S saves (kernel emits Ctrl+letter as `cp & 0x1F`, so Ctrl+S = `0x13`), `\r`/`\b`/GUI_KEY_* (UP `0x0101`..END `0x0106`, DEL `0x0107`) edit. Status bar shows `Файл: <name>` and a brief `сохранено`/`ошибка` (~200 ticks) after a save. Default filename `untitled.txt`.
- **Embedded data**: `scripts/gen_ico.py` generates `scripts/demo.ico` (32×32 32bpp green disc, correct AND mask); `gen_progs.py --data demo.ico=scripts/demo.ico` embeds it into the ramdisk as `embedded_data[]`; `load_embedded_data()` writes it to the SFS on first boot so a demo icon shows on the desktop. `kernel/progs.c` is generated, never edit by hand.
- **notepadtest.py**: `python3 scripts/notepadtest.py` is the E2E regression (boots the ISO, asserts demo.ico pixels, right-click → «Новый файл» → types `note.txt`, opens it in notepad, types + Ctrl+S, then `cat note.txt` in a term). Own QEMU socket `/tmp/aos-notepad.sock`; uses `notepadtest`-prefixed PPM names and its own mouse-state file so it does not collide with `guitester.py`.
- **No panic ≠ success**: the WM may continue running after corruption but render black/garbage. Always check pixel values on screen.
