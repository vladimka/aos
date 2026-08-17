## Workflow convention

- **Язык общения и документации — русский.** Ответы пользователю, а также все создаваемые документы и спеки (включая `docs/superpowers/specs/*` и `docs/superpowers/plans/*`) пишутся **только на русском**. Код, сообщения коммитов и комментарии в коде — на английском (как в остальном репозитории). Технические идентификаторы (имена файлов, функций, syscall'ов) остаются без перевода. Правило действует всегда, в том числе при работе по скиллам (brainstorming, writing-plans, executing-plans и т.д.): скиллы задают процесс, но язык вывода определяет это правило.

- **UI/QEMU test suites may be run by the assistant.** `make test`, the individual `scripts/*.py` GUI tests, and `qemu-system-i386` launches are allowed. Remember: QEMU GUI tests are timing-sensitive under TCG and their results are evaluated by the human, so keep runs in `test`/`test-fast`-style harnesses unless debugging.

- **Debug launch**: `make debug` (or `scripts/qemu-debug.sh`) boots `aos.iso` headless with VNC (:5907) + QMP + serial Unix sockets. Connect with the qemu-vnc MCP tools via `vm_connect(vnc_host=127.0.0.1, vnc_port=5907, qmp_socket=/tmp/aos-debug.qmp, serial_socket=/tmp/aos-debug.serial)`. Both `make run` and `scripts/qemu-debug.sh` boot with `-vga none -device virtio-vga,disable-modern=on`, so the WM renders through the virtio-gpu double-buffered flip (`virtio-gpu: framebuffer flip enabled` in the serial log). GUI test harnesses (`notepadtest.py`, `configtest.py`) pass the same `GPU_ARGS`; drop them to exercise the std-VGA fallback (`virtio-gpu: not present, using VGA`, WM draws to VRAM with the software cursor).

## Terminal (`kernel/terminal.c`)

- **Line editing**: cursor moves left/right via arrow keys, backspace deletes whole UTF-8 character (scans for start byte), delete key (E0 53) removes character at cursor
- **Home/End**: E0 47 / E0 4F jumps cursor to start/end of line
- **Command history** (`hist_*`): circular buffer of 16 entries, Up/Down arrows browse older/newer; Enter pushes current line as new entry
- **Tab completion**: searches built-in `format` + every directory in PATH for prefix match; single match auto-completes inline, multiple matches prints list and re-prompts
- **Keyboard layouts**: US QWERTY and Russian ЙЦУКЕН; Left Ctrl + Left Shift held simultaneously toggles `ru_layout` flag; scancode mapped to Unicode codepoint (U+0400–U+04FF for Cyrillic, shared ASCII punctuation)
- **UTF-8 output**: `insert_codepoint()` encodes codepoint as 1–3 byte UTF-8 sequence into `line_buf`; `line_redraw_from()` re-renders from a given byte offset
- **Caps Lock** (`scancode 0x3A`): toggles `caps_lock` flag, affects only US layout letters
- **Serial console input**: `timer_handler` (`kernel/kernel.c`) drains the COM1 FIFO (`serial_available`/`serial_read`) into `terminal_serial_byte()`. The serial console **always feeds the shell line editor** (system console) — it is NOT rerouted to the GUI even after the WM registers as event consumer; GUI apps receive keyboard input from the PS/2 keyboard via `keyboard_handler` → `route_gui_key`. While a user program is running, serial bytes are pushed to the key queue (`key_queue_push`) instead of the line buffer
- **Key queue**: circular `key_queue[64]` for user programs. When `user_program_active()`, printable keys (and serial bytes) push to the queue; `terminal_read_key()` pops (returns -1 when empty). `SYS_READ_KEY` is **non-blocking**; `read_key()` blocks in userland by spinning + `yield()`

# AOS — minimal x86 kernel

## Build & run

```
make           # aos.iso (GRUB2 rescue ISO)
make run       # qemu-system-i386 -m 256 -cdrom aos.iso
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
- **2 MB ramdisk at `0x400000`** (`kernel/block.h` `RAMDISK_BASE`, above kernel BSS, below the staging buffer) — flat SFS (Simple File System), `SFS_MAX_FILES=64`
- **33 syscalls via `int 0x80`** (DPL 3 gate, `idt_install_irq_flags(0x80, isr128, 0xEE)`), R/O user-level interface in `programs/aosabi.h`
- `printf()` in `kernel/printf.c` writes to **both** VGA and COM1 (used for all kernel banners)
- **Privilege separation**: paging enabled (`kernel/paging.c`), user programs run in **ring 3** via TSS (`arch/i386/gdt.c`, `kernel/user.c`/`user_tramp.S`), kernel stays in ring 0

### Memory model (after `paging_init`)

- Identity map 0..256 MB via 64 page tables; PDEs + PTEs for the user area carry the user bit
- **Physical memory**: `kernel/pmm.c` — binary buddy page allocator over the multiboot
  memory map (reserved: low MB, kernel `[_start,_end)`, ramdisk, task-0 user area,
  slab window, framebuffer). `page_alloc`/`page_free` are IRQ-safe; per-frame
  metadata in `pmm_frames[]` (`order`/`flags`/`slab_class`, see `kernel/pmm.h`)
- **Kernel heap**: `kernel/kmm.c` — slab-lite `kmalloc`/`kfree` (size classes
  16..4096 backed by buddy pages; larger blocks as contiguous buddy blocks).
  Class/order recovered from `pmm_frames[]` on free
- **Tasks**: up to `MAX_TASKS=24`. ABI-dependent address space: **AOS** tasks
  pre-map the full 12 MB user area (`task_spawn` allocates kstack, PD, 3 PTs,
  3072 user frames, mailbox, args); **Linux** tasks skip the AOS user area
  entirely (`pts[]` stays NULL) and only get the 32 MB Linux window PTs
  (`lpts[32]`), so a spawned Linux task costs ~1 MB — `many` stress no longer
  drains the buddy's low pool below `0x08000000`. Everything is freed at exit;
  the dying task's kstack is freed lazily via a zombie list because
  `switch_and_restore` restores `%esp` only after `task_switch_kernel`
  returns (running on the dead stack)
- **User area** `0x01000000..0x01C00000` (PD 4..6): program text/rodata/data load at `0x01000000`, bump-allocator heap at `0x01100000`, user stack top at `0x01804000`
- TSS (`SEL_TSS` = 0x28) `esp0` = top of `sys_stack[8192]`; CPU switches to it on any ring 3 → ring 0 transition
- `launch_user_asm` (`kernel/user_tramp.S`) pushes SS/ESP/EFLAGS/CS/EIP (EFLAGS=0x202) and `iret`s to ring 3; `user_exit_asm` restores the saved kernel `esp` and `ret`s back into the shell as if the launch never happened
- Framebuffer physical range (often above 256 MB) is mapped via `extra_pt[8][]` using `vga_get_fb_info()`

### Syscall hardening (`kernel/syscall.c`)

- All pointers validated: `in_user()` checks `p` is within `0x01000000..0x01804000`, `copy_user_str()` copies a bounded NUL-terminated string to `user_str[1024]`
- Invalid pointers return `-5` instead of crashing the kernel (verified by `test`)
- `SYS_READ_KEY` is **non-blocking** (returns `-1` when the queue is empty); `read_key()` blocks in userland by spinning + `yield()`
- **Process syscalls** (`SYS_SLEEP` 30, `SYS_WAITPID` 31, `SYS_GET_CHILDREN` 32): `SYS_SLEEP`/`SYS_WAITPID` block **in the syscall handler** via `sti; hlt; cli` (`task_sleep`/`task_waitpid`, `kernel/task.c`) — the scheduler skips `TASK_SLEEPING`/`TASK_WAITING` tasks and promotes them when `tick >= wake_tick` or the waited child dies. `SYS_EXIT` (16) reads the exit code from `%ebx`; the exiting task stays as `TASK_ZOMBIE` holding `exit_code` until `waitpid` reaps it (its address space/mbox are freed as before, only the slot scalars are kept). `TASK_ZOMBIE` slots are reclaimed by `task_spawn` when no free slot exists (never one a live `TASK_WAITING` task waits on)

### ELF loader (`kernel/elf.c`)

- Segments load to `0x01000000..0x01100000` (`PROG_LOAD_MIN/MAX`); header, entry point, and every PT_LOAD range (vaddr + memsz) are bounds-checked against this window
- Uses `fs_read_at()` — no longer limited to a 4096-byte file read
- Programs expose `void main(void)`; musl's own crt provides `_start()` that calls `main()`, then `SYS_EXIT`

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

All commands (`help`, `uptime`, `clear`, `echo`, `tick`, `info`, `reboot`, `panic`, `ls`, `cat`, `rm`, `shutdown`, `format`, `test`, `cp`, `mv`, `mkdir`, `rmdir`, `head`, `wc`) are **standalone ELF32 programs** under `programs/`:

- Compiled for load address `0x01000000` (`programs/programs.ld`)
- Statically linked (`-static -nostdlib -n`), **no INTERP segment**
- Compiled from `programs/musl/*.c` via static musl i386 toolchain; include path `-Iprograms` for `aosabi.h`
- Embedded into kernel at build time by `scripts/gen_progs.py` → `kernel/progs.c`
- Stored in ramdisk as `bin/<name>`; loaded by `elf_load()` which parses PT_LOAD segments
- Run in **ring 3**; `bin/test` (aos_test framework) exercises `malloc/free`, blocking `read_key()`, and syscall pointer validation

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
→ mouse_init → fs_init → config_load → load_embedded_programs
→ load_embedded_data → terminal_init → hlt loop
```

PIT runs at **1000 Hz** (divisor 1193). `timer_handler` increments `tick`, drains COM1 into `terminal_serial_byte()`, toggles the cursor every 500 ticks.

### Config & boot screen

- `kernel_main` prints a 5-line ASCII **AOS** banner (rows of `AAA/OOO/SSS`) to VGA+COM1 right after `vga_init()` — the first thing in the serial log.
- **`sys/config.cfg`** (SFS, `kernel/config.c`): parsed by `config_load()` after `fs_init()`. If absent, it is created with defaults and the serial banner prints `config: created sys/config.cfg`; on subsequent boots `config: loaded sys/config.cfg`. Keys: `timezone=<min>` (applied via `rtc_set_tz`, see RTC), `wallpaper_top=<0xRRGGBB>`/`wallpaper_bot=<0xRRGGBB>`, and the ten `theme_*` keys (see Theme subsection) — the WM and GUI apps re-read the file via the userland `theme_load()`, the kernel's ring-0 parser ignores the theme keys (they are for userland). The timezone banner uses a manual `+`/`-` sign (kernel `printf` has no `%+d` flag).
- **Gradient wallpaper** (`programs/musl/wm.c`): `draw_desktop_gradient()` fills each damage rect with a per-color fixed-point ramp seeded at the rect's absolute `y0` (no per-pixel division); top = `wp_top` (default `0x1A2030`, from `theme_load()`), bottom = `wp_bot` (`0x0E1620`). `refresh_files()` skips `sys/` so the config file never shows on the desktop.

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
- **Dock & z-order**: `wm.c` draws a centered bottom dock bar (`DOCK_H=52`) with launcher icons (`bin/term`, `bin/clock`) plus one icon per running window. Launcher click spawns the app (logs `wm: dock spawn failed` on failure) or `raise_pid()`s it if already running; a running window's dock icon raises it too. A 4x4 indicator dot (`col_accent`) under the icon marks the focused window. Z-order lives in `zorder[]`/`nz`; `raise_pid` forces a redraw on any z-order change, not just a focus change.
- **vgu_irq chains into `virtio_irq_dispatch`**: virtio-gpu shares its INTx line with the other virtio devices (QEMU assigns one PCI INTx line per bus slot-group; 2-device config puts GPU+blk on IRQ 11, 4-device config GPU+net on IRQ 10, blk+rng on IRQ 11). `vgu_init` must NOT hardcode the IRQ: it installs `vgu_irq` on `vgpu.irq` (read from PCI config in `vm_probe`), otherwise a level-triggered GPU line whose ISR is never acked storms the slave PIC forever (cascade IRQ2 climbs to millions, `vgu_send`'s used-ring poll starves even though the device already wrote the response). The shared-line trap that killed disk I/O (configtest Boot B) is still avoided because `vgu_irq` acks its own ISR then calls `virtio_irq_dispatch()` (non-static, `drivers/virtio.c`), which polls every registered device's ISR; `vgu_irq` overwrites the dispatch only on the GPU's own line, which is fine since it chains into it.
- **Test script**: `python3 scripts/guitester.py` — dumps PPM screenshots to `/tmp/aos-G*.ppm`, checks pixel colors. It drives an **already-running** QEMU (start one with `-vga none -device virtio-vga,disable-modern=on -display none -serial file:/tmp/aos-gui.log -monitor unix:/tmp/aos-gui.sock,server,nowait`). `QEMU` monitor socket at `/tmp/aos-gui.sock`. Serial log at `/tmp/aos-gui.log`. The guest cursor re-centers at (511,383) on boot but `/tmp/aos-mouse.state` keeps the last tracked position, so after **each** QEMU restart run `python3 scripts/guitester.py resetmouse 511 383` (or delete the state file) before the first click.
- **Screen layout**: a top panel (`PANEL_H 26`, `col_dock_bg` 0x232C40, accent line at `PANEL_H-1`, lighter strip at `PANEL_H-2`) spans the whole width; the desktop gradient therefore starts at y=26 (top = 0x1A2030 = (26,32,48), bottom 0x0E1620, see config). Term window at ~(20,34) 640×416px; clock at ~(44,62) 260×100px. Term content bg = 0x101010 = (16,16,16). Clock title unfocused = 0x263C5E (`theme_title`), focused = 0x4E86C7 (`theme_title_focus`). New windows spawn at `y = PANEL_H + 8 + i*28` and are drag-clamped to `ny >= PANEL_H`.
- **Top panel** (`draw_panel` in `wm.c`): centered one-line clock — date `DD.MM.YYYY` dimmed (`lighten(col_dock_bg,8)`) + gap 8 + time `HH:MM:SS` white, single 16-px row (total width 152 px, `x0 = (fb_w-152)/2`, y=(26-16)/2), refreshed once per second from the RTC via `aos_get_rtc` (`tk/1000 != p_last_sec` → format + `redraw=1`; `p_last_sec` init 0xFFFFFFFF). Power button 24×24 at `fb_w-8-24`, y=1 (`pwr_btn_at`), hover = `lighten(col_dock_bg,6)` rounded fill; 16×16 two-tone glyph from `icon_power[16][17]` (`'X'` white ring, `'O'` accent stroke). Clicking toggles the **power menu** (`pwr_menu`, `PMENU_W 140`) below the button at `y = PANEL_H+2` with «Выключить» / «Перезагрузить»; clicking an item prints `wm: shutdown`/`wm: reboot` and calls `reboot(RB_POWER_OFF)`/`reboot(RB_AUTOBOOT)` (syscall 88) directly — no task spawn. The desktop context menu was generalized into `struct popmenu {open,x,y,draw_x,draw_y,n,w,items,last_hover}`; `ctx_menu` (`MENU_W 176`) and `pwr_menu` both go through `draw_menu`/`menu_item_at` and the same hover handling.
- **Desktop file icons**: `wm.c` lists the SFS ramdisk and shows every non-`bin/`/`lin/` entry as a 32×32 icon on a grid (`GRID_X0 16`, `GRID_Y0 = PANEL_H+8` = 34, cell stride 52, row height 56; label under the icon). Kinds: `K_FOLDER` (name ends `/`), `K_TEXT` (`*.txt`), `K_ICO` (decoded via `programs/musl/ico.c` — pure-C ICO/BMP decoder, up to 32×32, palette + 32bpp + AND mask; on decode failure falls back to `icon_image` art), `K_OTHER`. Left-click on a text/other file spawns `bin/notepad <name>`; `.ico` and folders are no-ops. `bin/` and `lin/` prefixes are hidden (system payload lives in the dock / shell PATH, not the desktop).
- **Context menu + create dialog**: right-click on desktop (not a window/dock/power button) opens a menu (`MENU_W 176`) with «Новый файл» / «Новая папка» (and closes any open power menu). Left-clicking an item opens a modal name-input dialog (`dlg_open/dlg_mode/dlg_name[40]/dlg_len`); while open the WM consumes `MSG_KEY` itself (printable ASCII, `\b`, Enter=confirm, Esc=cancel) instead of forwarding. Confirm writes an empty SFS file; folders are names ending in `/`. Then `files_dirty=1`.
- **WM refresh/redraw pitfall**: the main loop runs `refresh_files()` (if `files_dirty` or every 128 iterations) **before** the message loop. So a file created by a message (dialog Enter) lists only on the *next* iteration, by which time the same-iteration `redraw=1` has already been consumed → the new icon never renders. Fix: `refresh_files()` sets `redraw=1` when `nfiles` changed (`if (nfiles != old_n) redraw = 1`).
- **Notepad**: `programs/musl/notepad.c` GUI text editor (window 640×416 = 80×26 chars via `MSG_CREATE`), model `lines[200][80]` codepoints, cursor + `scroll`. Ctrl+S saves (kernel emits Ctrl+letter as `cp & 0x1F`, so Ctrl+S = `0x13`), `\r`/`\b`/GUI_KEY_* (UP `0x0101`..END `0x0106`, DEL `0x0107`) edit. Status bar shows `Файл: <name>` and a brief `сохранено`/`ошибка` (~200 ticks) after a save. Default filename `untitled.txt`.
- **Embedded data**: `scripts/gen_ico.py` generates `scripts/demo.ico` (32×32 32bpp green disc, correct AND mask); `gen_progs.py --data demo.ico=scripts/demo.ico` embeds it into the ramdisk as `embedded_data[]`; `load_embedded_data()` writes it to the SFS on first boot so a demo icon shows on the desktop. `kernel/progs.c` is generated, never edit by hand.
- **notepadtest.py**: `python3 scripts/notepadtest.py` is the E2E regression (boots the ISO, asserts demo.ico pixels, right-click → «Новый файл» → types `note.txt`, opens it in notepad, types + Ctrl+S, then `cat note.txt` in a term). Own QEMU socket `/tmp/aos-notepad.sock`; uses `notepadtest`-prefixed PPM names and its own mouse-state file so it does not collide with `guitester.py`.
- **No panic ≠ success**: the WM may continue running after corruption but render black/garbage. Always check pixel values on screen.

### Theme (config-driven)

- **Shared loader**: `programs/musl/theme.c` + `theme.h` (musl build — the WM/apps link it, NOT `libaos`; see the Makefile `build/prog/wm.elf`/`term.elf`/`clock.elf`/`notepad.elf` rules). `theme_load()` reads `sys/config.cfg` once (first call wins, later calls no-op) and fills a static table; `theme_color(key, fallback)` returns the parsed value or `fallback`. Unknown/duplicate keys are ignored (first occurrence wins); invalid hex leaves the default. `wm.c`'s old `load_wallpaper_config()` is replaced by `theme_load()` + `theme_color("wallpaper_top"/"wallpaper_bot", ...)`.
- **Keys + defaults** (written into the generated `sys/config.cfg` by `kernel/config.c`, `theme_load`'s compile-time defaults must match): `theme_title` 0x263C5E, `theme_title_focus` 0x4E86C7, `theme_border` 0x12161F, `theme_border_focus` 0x6B9BD2, `theme_dock_bg` 0x232C40, `theme_accent` 0x5B93D8, `theme_menu_bg` 0x20283A, `theme_menu_fg` 0xFFFFFF, `theme_text_fg` 0xD8D8D8, `theme_text_bg` 0x101010.
- **Corner radii** (stair-stepped, no AA; corner pixels left untouched so the background shows; hit-testing stays bounding-box): windows r=4 top corners only (`fb_round_fill_top`), dock r=6 top, menu/dialog r=3 all corners (`fb_round_fill`). Title bar gets a lighter top/mid strip via `lighten()`, windows a per-focus frame (`theme_border`/`theme_border_focus`) and title (`theme_title`/`theme_title_focus`), dock an accent top line (`theme_accent`) + active dot, menu/dialog an accent frame + accent hover highlight on the item under the cursor.
- **Two-color icons**: `draw_icon2(x, y, art, fg, accent)` — `'X'` pixels draw `col_icon_fg` (white), `'O'` draw `col_accent`, anything else transparent; clip-aware via `fb_put`. Used by dock launchers, the running-window loop, and desktop icons. Desktop `.ico` files still render decoded pixels (`draw_ico_file`, `programs/musl/ico.c`); only the `icon_image` fallback routes through `draw_icon2`.
- **Apps**: `term.c`/`notepad.c` use `theme_text_fg`/`theme_text_bg`; notepad's status bar uses `theme_dock_bg`/`theme_text_fg`; `clock.c` draws the time in `theme_accent` with the date/sub dimmed from it.
- **`configtest.py` Boot B** seeds a disk `sys/config.cfg` with `timezone=+180`, `wallpaper_top=0x102030`, `theme_accent=0xFF00FF` and asserts the panel pixel at (700,0), the gradient below the panel at (700,26), plus the dock accent line at (480,708), proving config edits apply without a rebuild.
- **`powertest.py`**: `python3 scripts/powertest.py` is the top-panel + power-menu regression (GPU path): asserts the panel bg at (700,0), a bright centered clock strip, the power-button glyph at the top-right, then clicks the button to open the power menu, clicks «Выключить» → asserts `wm: shutdown` in the serial log and that the VM actually powers off (monitor socket disappears — the button-release HMP is skipped), then reboots a fresh VM and clicks «Перезагрузить» → asserts `wm: reboot` and a second boot. Registered in the Makefile `TESTS` list.

## Linux ELF execution (step 1)

Step 1 runs **static musl i386** binaries (ET_EXEC, no INTERP) as user programs. Embedded: `lin/hello`, `lin/ls`, `lin/cat`, `lin/test.txt` (built from `tools/linux/*.c` with musl, `--data lin/...` in the Makefile). `bin/linrun` spawns `lin/hello` as a real pid>0 task; the shell also runs them by path (`lin/hello`).

- **ABI probe** (`kernel/elf.c` `elf_probe`): class=ELF32, machine=3 (EM_386), `e_type==ET_EXEC`. ET_DYN or entry below `LINUX_ENTRY_MIN` is rejected as AOS. `ABI_AOS`/`ABI_LINUX` live in the task (`kernel/task.h`), checked by `syscall_handler` (`kernel/syscall.c:134`) which routes to `linux_syscall_handler`.
- **Address space**: `LINUX_BASE 0x08048000`. Task 0 gets an 8 MB window `0x08000000..0x08800000` identity-mapped + user-accessible in the kernel page tables (`kernel/paging.c`, PDE 32–33) and reserved in `pmm.c`. **Spawned** Linux tasks get a private `0x08000000..0x10000000` (32 MB, PDE 32–63) via `lpts[32]` page-table pages installed into the task PD (`kernel/task.c:260`); `task_spawn` probes the ABI first (`elf_probe`) and leaves the AOS user-area PDEs 4–6 unmapped for Linux tasks (no 12 MB pre-zeroed frames). All ELF segments/brk/mmap/stack pointers are bounds-checked against `lc->win_lo..win_hi`.
- **Loader** (`kernel/elf.c` `elf_load_linux`): validates header/entry/each PT_LOAD range against the window, maps pages with `paging_map_user_page`, computes `brk_base` (end of BSS). `stack_build()` lays out the musl startup stack top-down from `stack_top`: argc/argv/envp + auxv (`AT_PHDR/PHENT/PHNUM/PAGESZ/BASE/ENTRY/UID/EUID/GID/EGID/RANDOM/EXECFN`), 16 `AT_RANDOM` bytes, `AT_EXECFN` string; `lc->stack_sp` is the resulting ESP. `user_program_start_linux`/`launch_user_linux` (`kernel/user.c`, `kernel/user_tramp.S`) iret to ring 3.
- **Runtime context** (`kernel/linux_syscall.c`, `struct linux_ctx` per task, kmalloc'd): `brk_base`/`brk_cur`, top-down `mmap_cur` (mmap2), `stack_top`/`stack_sp`, `win_lo`/`win_hi`, and TLS fields. `linux_ctx_init` runs at spawn. **Fd handling is NOT here**: Linux syscalls read/write the task's real fd table (`get_current_task()->fds[]`, console fds 0–2) via `vfs_*_fd`, and paths resolve against the task CWD (`current_task_cwd()`), exactly like the AOS fd syscalls.
- **Syscalls** (`int 0x80` dispatch to `linux_syscall_handler`): exit/exit_group(1/252, propagate `%ebx` code to the zombie via `task_exit_current`), write(4), writev(146, musl's `__stdio_write`), read(3, fd0 → `terminal_read_key` returning -EAGAIN when empty), open(5)/openat(295, incl. `O_DIRECTORY`; dirfd `-100`==AT_FDCWD → task CWD), close(6), unlink(10), lseek(19)/_llseek(140), access(33), chdir(12), getcwd(183), mkdir(39), rmdir(40), time(13), getpid(20), getuid/gid/euid/egid(24/47/49/50), ioctl(54, -ENOTTY), gettimeofday(78), uname(122), brk(45), mmap2(192, top-down anonymous), munmap(91)/mprotect(125) no-op, nanosleep(162→`task_sleep`, 265→no), clock_gettime(265→zeroed), set_thread_area(243)/modify_ldt(123), set_tid_address(258), stat64(195)/fstatat64(300)/fstat64(197) (fill i386 musl `struct stat`, 108 B, S_IFDIR/S_IFREG st_mode), getdents64(220, iterates the fd's directory via `vfs_readdir_fd`, so `lin/ls /proc` lists procfs, not the SFS root), sync(36)/fsync(118, fd-validated via `lin_fd_valid`) → `vfs_sync()` returning the number of flushed dirty blocks). FDs ≥3 are the VFS open-file table entries (`vfs_ofile_ptr`); `lin_fd_valid()` checks `fd∈[3,TASK_MAX_FDS) && t->fds[fd]`. Userspace pointers validated via `in_luser`/`copy_lin_str` (like `in_user` for AOS, but against the Linux window).
- **TLS**: musl's `__set_thread_area` computes the selector as `entry_number*8 + 3` — a **GDT** selector (TI bit never set) — so the descriptor must live in the GDT, not an LDT. `set_thread_area`/`modify_ldt` record the user_desc in `lctx`, install it at GDT slot `TLS_ENTRY=6` (Linux's `GDT_ENTRY_TLS_MIN`) via `ldt_set_tls` (`arch/i386/gdt.c`), and return `entry_number = 6` → musl loads `%gs = 0x33`. `task_switch_kernel` re-installs the descriptor and calls `tls_reload_gs()` (`mov %gs, 0x33`) when the incoming task is `ABI_LINUX` with `tls_seg32` set; `linux_ctx_init` clears it for AOS tasks.
- **brk/stack/mmap pointers all within the window** — do not touch the AOS user area (0x01000000..0x01804000), so Linux and AOS tasks never collide.
- **Test harnesses**: `scripts/linhello.py` (musl hello in the kernel shell), `scripts/lincat.py` (musl cat reads `lin/test.txt`), both boot the ISO and check serial output for panics. Full regression: `manytest.py`, `ipctest.py`, `notepadtest.py` must stay green (the `lin/*` payload is hidden from the desktop icon grid alongside `bin/` in `wm.c refresh_files()`).

## Pipes & shell pipelines

- **`kernel/pipe.c`** — static array `pipes[PIPE_MAX=8]` of `struct aos_pipe` (4096-byte circular buffer, `nreaders`/`nwriters`), registered as VFS via `struct vfs_fs pipefs_fs` (`name="pipefs"`, per-fs `close` hook from `42500f1`). `read` blocks with `sti;hlt;cli` while `count==0 && nwriters>0` and returns 0 (EOF) when writers are gone; `write` blocks while the buffer is full and returns `-32` (EPIPE) when `nreaders==0`. `pipe_close` decrements the matching counter (VFS_O_WRONLY vs read) and frees the slot when both reach 0. Each pipe has a fake inode (`refcount=2`, `valid=0` so `vfs_put` never frees it).
- **`kernel/vfs.c` `vfs_pipe(&rd,&wr)`** — allocates two global `ofiles[]` slots (`VFS_O_RDONLY`/`VFS_O_WRONLY`) backed by one `pipe_alloc()`'d pipe. `vfs_close_fd` now runs `fs->close` to update reader/writer counts.
- **Pipe ends are global fds**: the read end `rd` and write end `wr` are slots in the shared `ofiles[]`, so a pipeline wires children by copying `c->fds[fd] = vfs_ofile_ptr(fd)` into each spawned task's own fd table (the spawned task has not been scheduled yet, so no race). `SYS_PIPE` (Linux syscall 42) fills a user `int fds[2]` and points the current task's `fds[rd]/fds[wr]` at them (musl `pipe()` falls back from pipe2 to pipe 42).
- **Shell `exec_pipe()`** (`kernel/commands.c`): `a | b | c` splits on `|` operators (requires a SPACE before them, shared `find_operator` with redirects, max `PIPE_STAGES_MAX=8`), validates all stages up front (no builtins — prints `pipe: builtin not supported in pipeline`; no redirects; every cmd must resolve via `path_resolve`), creates N-1 pipes, spawns each stage, then wires the child's `stdin_fd`/`stdout_fd` (and `fds[]`) to the pipe ends after each `task_spawn`. `$?` = last stage's exit code via `task_waitpid` on each child in order. Task 0 (shell) is the parent of all stages.
- **IF preservation is critical for pipelines**: `task_sleep`/`task_waitpid` use `irq_save`/`irq_restore` (pushfl/cli, sti-or-cli restore) instead of inlining `sti;hlt;cli`. The old trailing `cli` stuck when the wait ran in kernel context (exec_pipe's `task_waitpid` on the main-loop stack), leaving the shell's main `hlt` with IF=0 forever and freezing the whole system. `d5797d0`+`f5d1c1a` make the restore exact (added the `else cli`).
- **Supported pipelines**: AOS writer → Linux reader (e.g. `ls /bin | lin/cat`), procfs source (`cat /proc/uptime | lin/cat`), blocking stress (`lin/piptest gen 20000 | lin/cat` through a 4096-byte buffer). Not supported: builtins as stages, redirects inside a stage.
- **Test harness**: `scripts/pipetest.py` (boots the ISO headless, serial-only) checks boot OK, `lin/piptest` (in `tools/linux/piptest.c`, syscall-42 smoke) prints `PIPETEST OK`, the two pipelines above, and the stress case where the writer must block mid-way. It is a Linux test (needs `lin/piptest`), so it lives in `LINUX_TESTS` → part of `make test` regression alongside `linhello`/`lincat`/`lindirtest`.
- **AGENTS.md `Test harnesses` note for `lindirtest`**: `scripts/lindirtest.py` (musl `ls` on SFS root + `/proc`) needs the qtest VNC keymap to include `slash` for the `/` argument — if the harness drops it, the listing renders but the assertion on root rows fails. Keep `qtest.py`'s typing mapping in sync when adding keys.

## Block devices & AHCI

- **Block backends** (`kernel/block.c`): selection priority `ahci > ata > vblk > ram`. Each `struct sdev` exposes sector `read`/`write` plus `read_multi`/`write_multi` (≤128 sectors, wrapper loops for ata/vblk). Init order in `kernel/kernel.c`: `ata_init() → ahci_init() → virtio_init()`; `block_init()` picks the highest-priority present backend and mounts SFS2.
- **AHCI driver** (`drivers/ahci.c`): PCI scan for class `0x010601` (QEMU `ich9-ahci` = 8086:2922), ABAR (BAR5 & ~0xFFF) mapped via `paging_identity_map()`, first port with `PxSSTS` DET=present (0x3) and IPM=active (0x1) selected. Single slot-0 command list (`cmd_list[32]`) + 64-dword command table + one PRD; issues IDENTIFY (0xEC) and LBA48 READ/WRITE_DMA_EXT (0x25/0x35). Waits by polling `PxCI` with a 2000-tick timeout; errors from `PxIS.TFES`/`PxTFD.ERR`. Reports IDENTIFY capacity (w60 or w100 for LBA48) and runs single- + multi-sector selftests on the last sectors.
- **QEMU/device layout traps** (verified against QEMU 10.2 `hw/ide/ahci.c`, `hw/ide/core.c`): (1) H2D FIS `fis[1]` bit7 (0x80) = update-command-register — NOT bit4; (2) sector count lives in FIS bytes 12–13 (spec Table 42), NOT 15/16 — with count==0 QEMU auto-sizes to 65536 in `ide_cmd_lba48_transform`; (3) command-list `tbl_addr` is **64-bit** (`cmd_list[2]`=low, `cmd_list[3]`=high) — leaving high garbage made QEMU read the FIS from a bogus address (all-zero FIS dump); (4) PRD byte count is zero-based (`bytes-1`) in DW3; (5) LBA48 = `fis[10]<<40|fis[9]<<32|fis[8]<<24|fis[6]<<16|fis[5]<<8|fis[4]` (the select byte `fis[7]=0x40` is unused). **Integer-division trap**: PRDT fields are at `cmd_table[0x80/4]`, `[0x84/4]`, `[0x88/4]`, `[0x8C/4]` — writing `cmd_table[0x83/4]` computes 131/4=32, the same slot as `[0x80/4]`, which clobbered the PRD buffer address (data "arrived" at `prd=0x1FF`).
- **`paging_identity_map(phys, bytes)`** (`kernel/paging.c`): identity-maps a physical range outside the pre-built 0..256 MB map (framebuffer, AHCI ABAR) via `extra_pt[8][]`; pages already in the map are untouched. New pages don't need a CR3 reload (TLB miss walks the updated PD); caller may reload if it mapped existing entries.
- **QEMU trace debugging**: AHCI events in `/usr/share/qemu/trace-events-all` (~lines 2064–2117), enable with `-d trace:handle_cmd,handle_reg_h2d_fis,ahci_port_write,ahci_write_fis_d2h,ahci_cmd_done,ahci_populate_sglist,ahci_dma_prepare_buf,ide_bus_exec_cmd`; `handle_cmd_fis_dump` prints the exact FIS QEMU sees. PIO data (IDENTIFY) is copied via `dma_buf_read` with **no** trace event; `ahci_dma_rw_buf` is DMA-path only. SeaBIOS writes AHCI ports (PxCLB=0x0efe9000…) and spams 0xa0 (PACKET) commands before the kernel — expected noise; our code starts at the `PxCLB=0x00269c00`-ish write.
- **Test harness**: `scripts/ahcitest.py` boots the ISO with `-device ich9-ahci,id=ahci -drive file=…,if=none,format=raw,id=d0 -device ide-hd,drive=d0,bus=ahci.0` (4 MiB disk) and asserts `ahci: found`, `selftest OK`, `selftest multi OK`, `block: ahci backend, 8192 sectors`. Full ATA/virtio/persist regressions: `scripts/atatest.py`, `scripts/blktest.py`, `scripts/persisttest.py`.
- **Write-back cache & sync**: SFS2 writes go through the block cache (`block_pin`/`block_mark_dirty`, `block_flush` writes all dirty sectors to the device and returns the flushed count). `vfs_sync()` (`kernel/vfs.c`) = `sfs2_flush` (meta + data) → flushed-block count; it is called on reboot/shutdown and by the **Linux** `sync`(36)/`fsync`(118) syscalls (both fd-validated; `fsync` needs a real fd ≥3, `sync` is global). `bin/sync` (`programs/musl/sync.c`) calls syscall 36 and prints `sync: flushed N blocks`. AOS-ABI programs (if any) do NOT use these — `bin/*` and `lin/*` are all `ABI_LINUX` (musl entry ≥ `LINUX_ENTRY_MIN`), so fs operations go through `linux_syscall_handler`.
- **SFS2/VFS limits** (audit result, unchanged): `SFS2_INODES=256` (max files), max file size = 8 direct + 128 indirect blocks = 136 × 512 B ≈ 68 KiB, `SFS2_DIRENTS_PER_BLOCK=16` per dir block, `VFS_OFILES=64` global open-file slots, `TASK_MAX_FDS=64` per-task fd table, block cache `BLOCK_CACHE_SECTORS=128` (64 KiB). Sizes beyond that are out of scope; tests stay under the file-size cap (`atatest` `big.txt` uses indirect blocks).

## virtio-gpu (WM framebuffer)

- **Modern transport mandatory**: QEMU 10.2.1 virtio-vga (`1af4:1050`) is **non-transitional** — it has no legacy I/O BAR, so `drivers/virtio.c` (I/O-port transport) cannot drive it. `drivers/virtio_modern.c/.h` is a capability-based modern virtio-pci transport (`vm_*` API mirroring the legacy `virtio_*` signatures). Device ids: GPU `0x1050`, blk `0x1001`.
- **Driver** (`drivers/virtio_gpu.c`): `vgu_init()` probes `vm_probe(0x1050)` → registers queues (ctrlq qidx 0, cursorq qidx 1) → creates resources rid1/2 (1024×768 B8G8R8X8, pitch 4096) + rid3 cursor (64×64) → selftest flips + cursor show/hide (`vgu: active`, `vgu: flip ok` x2, `vgu: cursor ok`). Commands are a **two-descriptor [cmd|resp] chain** (a writable resp descriptor is mandatory — without it QEMU answers 0x1203 `INVALID_RESOURCE_ID`); without a `RESP_OK_NODATA`-check the used ring stays consistent because each command consumes one head id.
- **Key protocol facts**: `CREATE_2D=0x0101`, `SET_SCANOUT=0x0103`, `FLUSH=0x0104` (payload = rect first), **`TRANSFER_TO_HOST_2D=0x0105`** (payload = `{rect{x,y,w,h}, offset u64, resource_id, pad}`, 32 B packed), `ATTACH_BACKING=0x0106`, `UPDATE_CURSOR=0x0300`/`MOVE_CURSOR=0x0301` (payload `{scanout_id,x,y,pad,resource,hot_x,hot_y,pad2}`), `RESP_OK_NODATA=0x1100`, `ctrl_hdr` 24 B. GPU window `0x04000000..0x04800000` (`GPU_BASE`), buffers stride `GPU_STRIDE 0x300000`.
- **TRANSFER_TO_HOST_2D is mandatory** — QEMU's 2D path renders from a **host-side pixman image**, guest backing is copied into it **only** by TRANSFER_TO_HOST_2D; `set_scanout`+`flush` alone leave the host image all-zero → black screen. Every flip does `vgu_transfer(rid)` before `set_scanout(rid)`; `front` is toggled only after a successful scanout.
- **IRQ**: `vgu_irq` acks the device ISR then calls `virtio_irq_dispatch()` (see WM notes above — the GPU shares its line with vblk/vrng/vnet, number from PCI config). Without the chain, `vgu_irq` would overwrite the shared dispatch and kill disk I/O. Since the fix in `vgu_init`, the handler is installed on `vgpu.irq` (real PCI line), not hardcoded IRQ 11 — with 4 virtio devices the GPU lands on IRQ 10.
- **Syscalls** (`kernel/aos_gui.c`): `AOS_GPU_INFO 521` (eax=n, ebx=arg1..), `AOS_GPU_FLIP 522`, `AOS_CURSOR 523`; `AOS_FB_INFO` in GPU mode returns `vgu_back()/1024/768/4096/32`. User wrappers in `programs/aosabi.h`.
- **WM**: `wm.c` detects the GPU via `gpu_present()` (syscall probe); `gpu_mode` draws into the back buffer and flips, hardware cursor via `AOS_CURSOR`. On VGA fallback (`virtio-gpu: not present, using VGA`) the WM draws to VRAM with the software cursor — the split is transparent to the GUI tests.
- **Tests**: `scripts/vguitest.py` (GPU selftest + desktop gradient pixel (700,400)=(19,26,39)); `notepadtest.py`/`configtest.py` boot with `GPU_ARGS=["-vga","none","-device","virtio-vga,disable-modern=on"]` (drop to exercise VGA fallback). GUI text-band checks poll instead of fixed sleeps — TCG is slower under the GPU scanout path.
