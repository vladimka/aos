## Terminal (`kernel/terminal.c`)

- **Line editing**: cursor moves left/right via arrow keys, backspace deletes whole UTF-8 character (scans for start byte), delete key (E0 53) removes character at cursor
- **Home/End**: E0 47 / E0 4F jumps cursor to start/end of line
- **Command history** (`hist_*`): circular buffer of 16 entries, Up/Down arrows browse older/newer; Enter pushes current line as new entry
- **Tab completion**: searches built-in `format` + every directory in PATH for prefix match; single match auto-completes inline, multiple matches prints list and re-prompts
- **Keyboard layouts**: US QWERTY and Russian ЙЦУКЕН; Left Ctrl + Left Shift held simultaneously toggles `ru_layout` flag; scancode mapped to Unicode codepoint (U+0400–U+04FF for Cyrillic, shared ASCII punctuation)
- **UTF-8 output**: `insert_codepoint()` encodes codepoint as 1–3 byte UTF-8 sequence into `line_buf`; `line_redraw_from()` re-renders from a given byte offset
- **Caps Lock** (`scancode 0x3A`): toggles `caps_lock` flag, affects only US layout letters

# AOS — minimal x86 kernel

## Build & run

```
make           # aos.iso (GRUB2 rescue ISO)
make run       # qemu-system-i386 -cdrom aos.iso
make clean     # full clean, removes kernel/progs.c too
```

No dedicated test, lint, or typecheck commands.

## Architecture

- **Multiboot2** boot protocol (magic `0xE85250D6`), GRUB2 entry: `multiboot2 /boot/aos.elf`
- Kernel loaded at **1 MB** (`linker.ld`), linked with `-nostdlib -m32 -m elf_i386`
- All kernel C code compiled `-ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mno-sse -mno-mmx -mno-80387`
- No libc; own `string.c` (`strcmp`, `strncpy`, `strlen`)
- Include paths: `-Ikernel -Idrivers -Iarch/i386 -Iboot`
- **64 KB ramdisk at `0x200000`** — flat SFS (Simple File System), `SFS_MAX_FILES=64`
- **16 syscalls via `int 0x80`**, R/O user-level interface in `programs/libaos.c`

### Argument passing to `kernel_main`

In `boot/boot.S` the stack must have `%ebx` (info ptr) pushed first, `%eax` (magic) second — **wrong order causes null framebuffer info**.

### Framebuffer

- GRUB2 boot requests 1024×768×32 linear framebuffer (`tag type 5`)
- `drivers/vga.c` parses both **MB2 tags** (type 8, `magic == 0x36D76289`) and **MB1 info** (`flags[12]`)
- Falls back to VGA text mode (`0xB8000`) when framebuffer unavailable
- Font: CyrSlav-VGA16 8×16 bitmap (`drivers/fb_font.h`, built into kernel) with Cyrillic + Latin glyphs — replaces Lat15-VGA16 (Terminus)
- Unicode glyph lookup via `fb_unicode_map[]` (529 codepoint→glyph entries, binary search in `fb_cp_to_glyph()`)
- UTF-8 decoder in `vga.c` — state machine buffers up to 3-byte sequences, decodes codepoint via lookup table, renders bitmap
- Cursor blink: timer fires `vga_cursor_toggle()` every 9 ticks (~150ms @ 18Hz); framebuffer draws 2-pixel underline, text mode uses VGA hardware cursor registers
- Globals `__saved_mb_info` / `__saved_magic` set before `vga_init()` (`kernel/kernel.c`)

### Programs

All commands (`help`, `uptime`, `clear`, `echo`, `tick`, `info`, `reboot`, `panic`, `ls`, `cat`, `rm`, `shutdown`, `format`, `test`) are **standalone ELF32 programs** under `programs/`:

- Compiled for load address `0x01000000` (`programs/programs.ld`)
- Statically linked (`-static -nostdlib -n`), **no INTERP segment**
- Linked against `programs/libaos.o` (syscall wrappers)
- Embedded into kernel at build time by `scripts/gen_progs.py` → `kernel/progs.c`
- Stored in ramdisk as `bin/<name>`; loaded by `elf_load()` which parses PT_LOAD segments

### Program search order (shell, `kernel/commands.c`)

Programs are located via a **PATH** variable (`char command_path[PATH_MAX]`, default `"bin"`, colon-separated dirs). For each `cmd`:

1. For each PATH directory: try `<dir>/<cmd>` on ramdisk
2. Fallback: try `<cmd>` directly (arbitrary path)

`format` is a **kernel built-in** (formats ramdisk, re-loads embedded programs). Built-in `setpath [dirs]` shows/sets PATH; `PATH_MAX=128`.

### Mouse & scrollback

- **PS/2 mouse** on IRQ12 (`drivers/mouse.c`): initialised with IntelliMouse (wheel) protocol via sample-rate sequence 200/100/80
- PS/2 controller byte status: mouse data has `status & 0x20` set; `kernel/kernel.c` `keyboard_handler()` reads **all** pending PS/2 bytes and routes mouse bytes to `mouse_process_byte()` (otherwise IRQ12 eats ACK bytes and IntelliMouse detection fails, `has_wheel=0`). `mouse_init()` runs under `cli()` with input-buffer drain before `sti()`.
- 4-byte wheel packets: byte 3 = signed wheel delta per IntelliMouse protocol. **PS/2 wheel byte: +1 = wheel down, -1 (0xFF) = wheel up** (QEMU `ps2.c`: `WHEEL_UP → mouse_dz--`, `WHEEL_DOWN → mouse_dz++`). Wheel **up** → `vga_scroll(+3)` (older content), wheel **down** → `vga_scroll(-3)` (live view). Note: QEMU monitor `mouse_move dz` **inverts** sign — `mouse_move 0 0 1` = wheel up (byte 0xFF), `mouse_move 0 0 -1` = wheel down (byte 0x01)
- **Scrollback buffer** in `vga.c`: circular `scrollback_lines[SCROLLBACK_LINES][VGA_MAX_COLS]` (`SCROLLBACK_LINES=512`) storing every line that scrolls off screen
- **`vga_scroll(delta)`**: adjusts `scroll_offset`; when `scroll_offset == 0` displays live view, when `>0` renders scrollback via `render_scrollback()`/`render_cp_row()`; `vga_reset_scroll()` returns to live
- **Escaping scrollback**: any non-mouse input resets scroll offset to live view (`vga_reset_scroll()` in `terminal_set_prompt()`)

### Tab completion cycling (`kernel/terminal.c`)

- **First Tab**: searches `format` + every PATH dir (`bin/*` by default) for prefix match; single match → auto-complete, common prefix → auto-complete prefix, no common prefix → list matches
- **Second Tab** (same partial word): replaces current word with next match in cycle; repeats on each press
- **Any other key**: resets cycle state

### Boot-time init order (`kernel/kernel.c`)

```
serial_init → vga_init → gdt_init → idt_init → interrupts_init
→ irq_install_handler(0, timer) → irq_install_handler(1, keyboard)
→ fs_init → load_embedded_programs → terminal_init → hlt loop
```

## Key constraints

- `boot/boot.S` fields after checksum differ from raw offset — GRUB2 struct places video fields at +32 (after 5 address-field slots)
- `-serial stdio` conflicts with monitor; use `-serial file:serial.log` for debug
- QEMU 10.2.1 cannot boot via `-kernel`; must use `-cdrom aos.iso`
- ISO creation requires `grub-mkrescue` (GRUB 2.14)
- `gen_progs.py` must run after program ELFs are built; `kernel/progs.c` is generated (never edit by hand), always regenerated on `make`
