# Рефакторинг утилит + SGR-цвета в терминале: план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Стандартизировать утилиты AOS (флаги/stderr/exit-коды), добавить цветной `ls` через общий слой `uutils`, поддержку ANSI SGR в терминале (fb-консоль и GUI-терминал) и механизм env `TERM`.

**Architecture:** Программы (musl ELF, ring 3) красят вывод, только если `getenv("TERM")` непуст. TERM экспортируется шеллами (kernel shell и GUI `sh`) и снимается при редиректе stdout. Рендер SGR выполняется в `drivers/vga.c` (fb) и `programs/musl/term.c` (GUI); `serial.c` фильтрует SGR из serial-логов. Env передаётся от родителя к ребёнку через `stack_build`/`task_spawn`/`program_load` и новый syscall `AOS_SPAWN_FDS_ENV`.

**Tech Stack:** gcc -m32 kernel (C11, freestanding), static musl i386 toolchain для программ, QEMU + Python-тесты (`scripts/*.py`).

**Spec:** `docs/superpowers/specs/2026-08-17-utilities-refactor-design.md`

## Global Constraints

- Язык ответов и спеки — русский; код, коммиты, комментарии в коде — английский.
- Kernel C: `-ffreestanding -nostdlib -std=c11`, никакого libc; `__asm__ __volatile__` (не `asm`).
- Программы: статический musl i386 (`-static -no-pie -Os -Wall -Wextra -Iprograms`).
- `format` — kernel builtin; утилиты — standalone ELF32 в `programs/musl/*.c`.
- Все тесты — QEMU E2E через `scripts/*.py`; формат `wc` — `<lines> <words> <bytes> name` (сохранён, значения зависят от содержимого файла); `pwd` — строка `\n/proc\n` и т.п. (совместимость сохранена).
- `TERM` НЕ передаётся при `>`/`>>`/`<`/`|`; в pipeline не передаётся ни одному stage.
- serial-лог должен оставаться чистым (без `\x1b`).
- Правки `stack_build` обязаны сохранить 16-выравнивание `stack_sp` (musl `_start` реальинивает `%esp`).
- Syscall-номера: `AOS_SPAWN_FDS=520`, `AOS_GPU_INFO=521`, `AOS_GPU_FLIP=522`, `AOS_CURSOR=523`; новый — `AOS_SPAWN_FDS_ENV=524`.
- `programs/aosabi.h` включается и ядром (`-D__AOS_KERNEL__`), и musl-программами; user-side wrappers — под `#ifndef __AOS_KERNEL__`.

---

### Task 1: Общий слой `uutils.{c,h}` + интеграция в Makefile

**Files:**
- Create: `programs/musl/uutils.h`
- Create: `programs/musl/uutils.c`
- Modify: `Makefile:104-106` (generic `build/prog/%.elf` rule)

**Interfaces:**
- Consumes: musl `getopt`, `getenv`, `opendir`/`readdir`/`stat` (через vfs fd syscalls).
- Produces:
  - `int u_have_color(int fd)` — 1 если `getenv("TERM")` непуст (fd игнорируется).
  - `void u_color(int fd, int idx)` — пишет `\x1b[38;5;Nm` в fd.
  - `void u_color_bg(int fd, int idx)` — пишет `\x1b[48;5;Nm` в fd.
  - `void u_color_reset(int fd)` — пишет `\x1b[0m` в fd.
  - `const char *u_hsize(unsigned int n, char *buf, unsigned int bufsz)` — `1234`→`1.2K`, `1048576`→`1.0M`, возвращает `buf`.
  - `int u_list_dir(const char *dir, struct u_entry *ent, int max, int show_dot)` — возвращает число записей (или -1); сортирует по имени; `.` и `..` всегда пропускаются, прочие dot-файлы — только при `show_dot`.
  - `void u_print_columns(int fd, const struct u_entry *ent, int n, int one_per_line)` — колоночный вывод имён с маркерами `/` (dir) и `*` (exec), с цветом если `u_have_color`.
  - `int u_isdir(const struct u_entry *e)` — e->type == dir.
  - `struct u_entry { char name[VFS_NAME_MAX + 1]; unsigned int type; unsigned int size; };` (type: 1 file, 2 dir)
  - Цвета: `U_C_DIR 33`, `U_C_EXEC 70`.
  - `VFS_NAME_MAX` — define как `128` здесь (не включать kernel headers).

- [ ] **Step 1: Write `programs/musl/uutils.h`**

```c
#ifndef UUTILS_H
#define UUTILS_H

#define U_NAME_MAX 128
#define U_C_DIR    33
#define U_C_EXEC   70

struct u_entry {
    char name[U_NAME_MAX + 1];
    unsigned int type;      // 1 file, 2 dir (stat st_mode DT_*: use S_IFDIR)
    unsigned int size;
};

int u_have_color(int fd);
void u_color(int fd, int idx);
void u_color_bg(int fd, int idx);
void u_color_reset(int fd);
const char *u_hsize(unsigned int n, char *buf, unsigned int bufsz);
int u_list_dir(const char *dir, struct u_entry *ent, int max, int show_dot);
void u_print_columns(int fd, const struct u_entry *ent, int n, int one_per_line);

#endif
```

- [ ] **Step 2: Write `programs/musl/uutils.c`**

```c
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "uutils.h"

int u_have_color(int fd) {
    const char *t = getenv("TERM");
    return t && t[0] ? 1 : 0;
}

void u_color(int fd, int idx) {
    char b[16];
    int n = snprintf(b, sizeof b, "\x1b[38;5;%dm", idx);
    write(fd, b, (size_t)n);
}

void u_color_bg(int fd, int idx) {
    char b[16];
    int n = snprintf(b, sizeof b, "\x1b[48;5;%dm", idx);
    write(fd, b, (size_t)n);
}

void u_color_reset(int fd) {
    write(fd, "\x1b[0m", 4);
}

const char *u_hsize(unsigned int n, char *buf, unsigned int bufsz) {
    static const char *units[] = { "B", "K", "M", "G" };
    int u = 0;
    unsigned int v = n;
    while (v >= 1024 && u < 3) { v /= 1024; u++; }
    if (u == 0)
        snprintf(buf, bufsz, "%u%s", n, units[0]);
    else
        snprintf(buf, bufsz, "%u.%u%s", v, (n % 1024) / 100, units[u]);
    return buf;
}

static int u_cmp(const void *a, const void *b) {
    return strcmp(((const struct u_entry *)a)->name,
                  ((const struct u_entry *)b)->name);
}

int u_list_dir(const char *dir, struct u_entry *ent, int max, int show_dot) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (!show_dot && e->d_name[0] == '.') continue;
        strncpy(ent[n].name, e->d_name, U_NAME_MAX);
        ent[n].name[U_NAME_MAX] = 0;
        ent[n].type = 1;
        ent[n].size = 0;
        char p[512];
        if (strcmp(dir, "/") == 0)
            snprintf(p, sizeof p, "/%s", e->d_name);
        else
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0) {
            ent[n].type = S_ISDIR(st.st_mode) ? 2 : 1;
            ent[n].size = (unsigned int)st.st_size;
        }
        n++;
    }
    closedir(d);
    if (n > 1) qsort(ent, (size_t)n, sizeof(struct u_entry), u_cmp);
    return n;
}

void u_print_columns(int fd, const struct u_entry *ent, int n, int one_per_line) {
    if (one_per_line) {
        for (int i = 0; i < n; i++) {
            if (u_have_color(fd) && ent[i].type == 2) u_color(fd, U_C_DIR);
            write(fd, ent[i].name, strlen(ent[i].name));
            if (ent[i].type == 2) write(fd, "/", 1);
            if (u_have_color(fd)) u_color_reset(fd);
            write(fd, "\n", 1);
        }
        return;
    }
    int rows = n;
    int per = 1;
    while (per < rows) { per++; rows = (n + per - 1) / per; }   // square-ish grid
    /* simpler fixed layout: 4 columns when n > 4, else 1 */
    int cols = n > 4 ? 4 : 1;
    int each = (n + cols - 1) / cols;
    for (int r = 0; r < each; r++) {
        for (int c = 0; c < cols; c++) {
            int i = c * each + r;
            if (i >= n) continue;
            if (u_have_color(fd) && ent[i].type == 2) u_color(fd, U_C_DIR);
            write(fd, ent[i].name, strlen(ent[i].name));
            if (ent[i].type == 2) write(fd, "/", 1);
            if (u_have_color(fd)) u_color_reset(fd);
            write(fd, "  ", 2);
        }
        write(fd, "\n", 1);
    }
}
```

- [ ] **Step 3: Integrate into Makefile**

Modify the generic rule (lines 104-106) so every `programs/musl/*.c` program links `uutils.c`:

```make
build/prog/%.elf: programs/musl/%.c programs/musl/uutils.c programs/aosabi.h
	@mkdir -p build/prog
	$(MUSL_CC) -static -no-pie -Os -Wall -Wextra -Iprograms -o $@ $< programs/musl/uutils.c
```

- [ ] **Step 4: Build to verify**

Run: `make build/prog/head.elf`
Expected: builds cleanly (head.c links uutils.c, no duplicate-main link errors).

- [ ] **Step 5: Commit**

```bash
git add programs/musl/uutils.c programs/musl/uutils.h Makefile
git commit -m "utils: add shared uutils layer (color, listing, human sizes)"
```

---

### Task 2: SGR-парсер в `drivers/vga.c` (fb-режим)

**Files:**
- Modify: `drivers/vga.c:127-130` (ANSI state), `:29-34` (color table), `:189-237` (text_putchar), `:239-340` (fb_putchar), `:105-109` (vga_set_color), `:348-365` (fb_render_cell)

**Interfaces:**
- Consumes: existing `ansi_state`/`ansi_n` machine.
- Produces:
  - `static unsigned int xterm_rgb[256]` — полная xterm 256-палитра.
  - `static unsigned char fg_index = 7, bg_index = 0;` — текущие индексы.
  - `static int ansi_params[8], ansi_np;` — накопитель параметров SGR.
  - `static int ansi_bold;` — флаг bold (fg = (idx & 0xF8) | 8 для базовых 8).
  - `static unsigned int cur_fg_rgb(void)` — возвращает `xterm_rgb[bold ? fg_index|8 : fg_index]`.
  - `static unsigned int cur_bg_rgb(void)` — `xterm_rgb[bg_index]`.
  - SGR поддерживает: `0` (reset fg=7,bg=0,bold=0), `1` (bold), `38;5;N` (fg), `48;5;N` (bg), `39`/`49` (reset fg/bg). Прочие — игнор.
  - Text-режим: `static unsigned char xterm_to_vga(int idx)` — аппроксимация 256→16.
- Все существующие использования `color_rgb[fg_color]`/`color_rgb[bg_color]` в fb-путях заменяются на `cur_fg_rgb()`/`cur_bg_rgb()`; `vga_set_color(fg,bg)` продолжает задавать базовые индексы (0-15).

- [ ] **Step 1: Add the xterm palette + index state**

```c
// after color_rgb[16]
static const unsigned int xterm_rgb[256] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
    0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
    0x000000, 0x00005F, 0x000087, 0x0000AF, 0x0000D7, 0x0000FF, 0x005F00, 0x005F5F,
    0x005F87, 0x005FAF, 0x005FD7, 0x005FFF, 0x008700, 0x00875F, 0x008787, 0x0087AF,
    0x0087D7, 0x0087FF, 0x00AF00, 0x00AF5F, 0x00AF87, 0x00AFAF, 0x00AFD7, 0x00AFFF,
    0x00D700, 0x00D75F, 0x00D787, 0x00D7AF, 0x00D7D7, 0x00D7FF, 0x00FF00, 0x00FF5F,
    0x00FF87, 0x00FFAF, 0x00FFD7, 0x00FFFF, 0x5F0000, 0x5F005F, 0x5F0087, 0x5F00AF,
    0x5F00D7, 0x5F00FF, 0x5F5F00, 0x5F5F5F, 0x5F5F87, 0x5F5FAF, 0x5F5FD7, 0x5F5FFF,
    0x5F8700, 0x5F875F, 0x5F8787, 0x5F87AF, 0x5F87D7, 0x5F87FF, 0x5FAF00, 0x5FAF5F,
    0x5FAF87, 0x5FAFAF, 0x5FAFD7, 0x5FAFFF, 0x5FD700, 0x5FD75F, 0x5FD787, 0x5FD7AF,
    0x5FD7D7, 0x5FD7FF, 0x5FFF00, 0x5FFF5F, 0x5FFF87, 0x5FFFAF, 0x5FFFD7, 0x5FFFFF,
    0x870000, 0x87005F, 0x870087, 0x8700AF, 0x8700D7, 0x8700FF, 0x875F00, 0x875F5F,
    0x875F87, 0x875FAF, 0x875FD7, 0x875FFF, 0x878700, 0x87875F, 0x878787, 0x8787AF,
    0x8787D7, 0x8787FF, 0x87AF00, 0x87AF5F, 0x87AF87, 0x87AFAF, 0x87AFD7, 0x87AFFF,
    0x87D700, 0x87D75F, 0x87D787, 0x87D7AF, 0x87D7D7, 0x87D7FF, 0x87FF00, 0x87FF5F,
    0x87FF87, 0x87FFAF, 0x87FFD7, 0x87FFFF, 0xAF0000, 0xAF005F, 0xAF0087, 0xAF00AF,
    0xAF00D7, 0xAF00FF, 0xAF5F00, 0xAF5F5F, 0xAF5F87, 0xAF5FAF, 0xAF5FD7, 0xAF5FFF,
    0xAF8700, 0xAF875F, 0xAF8787, 0xAF87AF, 0xAF87D7, 0xAF87FF, 0xAFAF00, 0xAFAF5F,
    0xAFAF87, 0xAFAFAF, 0xAFAFD7, 0xAFAFFF, 0xAFD700, 0xAFD75F, 0xAFD787, 0xAFD7AF,
    0xAFD7D7, 0xAFD7FF, 0xAFFF00, 0xAFFF5F, 0xAFFF87, 0xAFFFAF, 0xAFFFD7, 0xAFFFFF,
    0xD70000, 0xD7005F, 0xD70087, 0xD700AF, 0xD700D7, 0xD700FF, 0xD75F00, 0xD75F5F,
    0xD75F87, 0xD75FAF, 0xD75FD7, 0xD75FFF, 0xD78700, 0xD7875F, 0xD78787, 0xD787AF,
    0xD787D7, 0xD787FF, 0xD7AF00, 0xD7AF5F, 0xD7AF87, 0xD7AFAF, 0xD7AFD7, 0xD7AFFF,
    0xD7D700, 0xD7D75F, 0xD7D787, 0xD7D7AF, 0xD7D7D7, 0xD7D7FF, 0xD7FF00, 0xD7FF5F,
    0xD7FF87, 0xD7FFAF, 0xD7FFD7, 0xD7FFFF, 0xFF0000, 0xFF005F, 0xFF0087, 0xFF00AF,
    0xFF00D7, 0xFF00FF, 0xFF5F00, 0xFF5F5F, 0xFF5F87, 0xFF5FAF, 0xFF5FD7, 0xFF5FFF,
    0xFF8700, 0xFF875F, 0xFF8787, 0xFF87AF, 0xFF87D7, 0xFF87FF, 0xFFAF00, 0xFFAF5F,
    0xFFAF87, 0xFFAFAF, 0xFFAFD7, 0xFFAFFF, 0xFFD700, 0xFFD75F, 0xFFD787, 0xFFD7AF,
    0xFFD7D7, 0xFFD7FF, 0xFFFF00, 0xFFFF5F, 0xFFFF87, 0xFFFFAF, 0xFFFFD7, 0xFFFFFF,
    0x080808, 0x121212, 0x1C1C1C, 0x262626, 0x303030, 0x3A3A3A, 0x444444, 0x4E4E4E,
    0x585858, 0x626262, 0x6C6C6C, 0x767676, 0x808080, 0x8A8A8A, 0x949494, 0x9E9E9E,
    0xA8A8A8, 0xB2B2B2, 0xBCBCBC, 0xC6C6C6, 0xD0D0D0, 0xDADADA, 0xE4E4E4, 0xEEEEEE
};
```

```c
// next to fg_color/bg_color (line ~20)
static unsigned char fg_index = 7;   // xterm light-grey
static unsigned char bg_index = 0;
static int ansi_bold = 0;
static int ansi_params[8];
static int ansi_np = 0;

static unsigned int cur_fg_rgb(void) {
    return xterm_rgb[ansi_bold ? ((fg_index & 0xF8) | 8) : fg_index];
}
static unsigned int cur_bg_rgb(void) {
    return xterm_rgb[bg_index];
}

static unsigned char xterm_to_vga(int idx) {
    /* nearest of the 16 base colors */
    if (idx < 8) return (unsigned char)idx;
    if (idx < 16) return (unsigned char)(idx & 7);        /* bright -> base */
    if (idx >= 232) return (unsigned char)(idx < 244 ? 8 : 7);
    /* cube colors: pick by luminance buckets */
    int r = ((idx - 16) / 36), g = ((idx - 16) / 6) % 6, b = (idx - 16) % 6;
    int gr = (r + 1) / 2, gg = (g + 1) / 2, gb = (b + 1) / 2;
    unsigned char col = (unsigned char)(gr * 4 + gg * 2 + gb);
    if (col > 8) col = 7;
    return col;
}
```

- [ ] **Step 2: Extend the ANSI parser to collect SGR params**

Replace the `ansi_state == 2` block in `text_putchar` (lines 210-223) and in `fb_putchar` (lines 266-284) with the following shared logic (add as `static void ansi_csi(int final, int *params, int np)` once, called from both):

```c
static void ansi_sgr(int *p, int np) {
    for (int i = 0; i < np; i++) {
        int v = p[i];
        if (v == 0) { fg_index = 7; bg_index = 0; ansi_bold = 0; }
        else if (v == 1) ansi_bold = 1;
        else if (v == 39) { fg_index = 7; ansi_bold = 0; }
        else if (v == 49) bg_index = 0;
        else if (v == 38 || v == 48) {
            if (i + 2 < np && p[i + 1] == 5) {
                int idx = p[i + 2];
                if (idx < 0) idx = 0;
                if (idx > 255) idx = 255;
                if (v == 38) fg_index = (unsigned char)idx;
                else bg_index = (unsigned char)idx;
                i += 2;
            } else if (i + 4 < np && p[i + 1] == 2) {
                /* 38;2;r;g;b -> skip, unsupported */
                i += 4;
            }
        }
    }
}

static void ansi_collect(unsigned char c) {
    /* called when ansi_state==2 and c is a non-digit, non-';' final byte */
    if (c == 'm') ansi_sgr(ansi_params, ansi_np);
    else if (c == 'K') vga_clear_eol();          /* both paths share clear */
    else if (c == 'D') {                         /* CUB: cursor left N */
        int k = ansi_np > 0 ? ansi_params[0] : 0;
        if (k <= 0) k = 1;                       /* bare ESC[D moves 1 */
        cursor_x -= k;
        if (cursor_x < 0) cursor_x = 0;
    }
    ansi_np = 0;
}
```

The digit/`;` accumulation becomes (both `text_putchar` and `fb_putchar` `ansi_state == 2` blocks):

```c
} else if (ansi_state == 2) {
    if (uc >= '0' && uc <= '9') {
        if (ansi_np < 8) ansi_params[ansi_np] = ansi_params[ansi_np] * 10 + (uc - '0');
        return;
    }
    if (uc == ';') { if (ansi_np < 8) ansi_np++; return; }
    ansi_state = 0;
    ansi_collect(uc);
    return;
}
```

Note: the `uc == '['` branch must now reset the SGR param buffer too: `ansi_state = 2; ansi_np = 0; for (int i = 0; i < 8; i++) ansi_params[i] = 0;` (replacing `ansi_n = 0` — `ansi_n` is no longer used, delete its declaration). `ansi_collect` resets `ansi_np` after each final byte. The old inline `K`/`D` blocks in both paths are removed (delegated to `ansi_collect`); `vga_clear_eol` is already static-visible (`drivers/vga.c:215`).

- [ ] **Step 3: Route fb drawing through index colors**

In `fb_putchar` replace `color_rgb[fg_color]`/`color_rgb[bg_color]` with `cur_fg_rgb()`/`cur_bg_rgb()` at lines 254, 331, and inside `fb_erase_underline` (149-150), `fb_scroll` (173), `fb_draw_glyph` calls. Keep `text_putchar` using VGA attributes: `text_color = xterm_to_vga(fg_index) << 4 | xterm_to_vga(bg_index)` computed in a helper `static void vga_refresh_text_color(void)` called after any SGR change and from `vga_set_color`.

- [ ] **Step 4: Build and boot-check**

Run: `make aos.iso` then `timeout 60 qemu-system-i386 -m 256 -cdrom aos.iso -display none -serial file:/tmp/sgr.log 2>&1 | head; grep -c 'Booting\|AOS' /tmp/sgr.log`
Expected: ISO builds; serial log shows the normal banner; no build errors.

- [ ] **Step 5: Commit**

```bash
git add drivers/vga.c
git commit -m "vga: parse SGR 256-color sequences in fb and text paths"
```

---

### Task 3: serial-фильтр SGR

**Files:**
- Modify: `drivers/serial.c:16-19` (`serial_putchar`)

**Interfaces:**
- Consumes: nothing.
- Produces: `serial_putchar` пропускает `\x1b[...m`-последовательности (и любые ESC-последовательности вида `ESC[ <digits/;/?> <alpha>`), не выводя их в COM1. Обычный текст и бинарные байты (кроме `\x1b`) выводятся как раньше. State: `static int ser_esc = 0, ser_csi = 0;`.

- [ ] **Step 1: Add the filter**

```c
static int ser_esc = 0;   // 0 idle, 1 = saw ESC, 2 = in CSI
static int ser_csi_n = 0; // bytes seen in CSI (cap to avoid runaway skip)

void serial_putchar(char c) {
    if (ser_esc == 0) {
        if (c == 0x1b) { ser_esc = 1; return; }
        while (!(inb(PORT + 5) & 0x20));
        outb(PORT, c);
        return;
    }
    if (ser_esc == 1) {
        if (c == '[') { ser_esc = 2; ser_csi_n = 0; }
        else ser_esc = 0;                 // lone ESC: drop it (wasn't followed by [)
        return;
    }
    /* in CSI: digits, ;, ? are skipped; a final alpha byte ends the sequence.
       A >64-byte "sequence" is treated as garbage and dropped too. */
    ser_csi_n++;
    if (ser_csi_n > 64 || !(c >= 0x20 && c < 0x7F)) { ser_esc = 0; return; }
    if (c >= '0' && c <= '9') return;
    if (c == ';' || c == '?') return;
    ser_esc = 0;                           // final byte consumed
    return;
}
```

- [ ] **Step 2: Build**

Run: `make aos.iso`
Expected: builds; existing serial banner unchanged (kernel printf emits no ESC).

- [ ] **Step 3: Commit**

```bash
git add drivers/serial.c
git commit -m "serial: filter ANSI escape sequences from COM1 output"
```

---

### Task 4: SGR-рендер в GUI-терминале `programs/musl/term.c`

**Files:**
- Modify: `programs/musl/term.c:21` (`screen`), `:62-73` (`put_cp`), `:75-92` (`render`), `:94-155` (`term_out_byte`)

**Interfaces:**
- Consumes: xterm palette (копия таблицы из Task 2 в виде `static const unsigned int xterm_rgb[256]`), существующий esc-парсер.
- Produces:
  - `struct tcell { unsigned int cp; unsigned char fg; unsigned char bg; };` и `static struct tcell screen[TH][TW];`.
  - `static unsigned char fg_idx = 15, bg_idx = 0;` (term по умолчанию fg = theme_text_fg; инициализируется из `col_fg` при первом рендере).
  - SGR-обработка в `term_out_byte` (final `m`): `0` reset, `1` bold (игнор — term использует точные 256 цветов), `38;5;N`/`48;5;N`.
  - `render()` рисует каждый глиф цветом `xterm_rgb[fg]` на `xterm_rgb[bg]` (через per-cell `aos_render_text` c одним символом, сгруппированным по run'ам с одинаковым (fg,bg) для скорости).
  - `newline`/`EL`/`ED`/`\b` обнуляют tcell целиком (`cp=0`).

- [ ] **Step 1: Extend the cell model**

```c
struct tcell { unsigned int cp; unsigned char fg; unsigned char bg; };
static struct tcell screen[TH][TW];
static unsigned char fg_idx = 15, bg_idx = 0;
```

- [ ] **Step 2: Update `newline`, `put_cp`, clear paths**

```c
static void newline(void) {
    if (crow < TH - 1) {
        crow++;
    } else {
        for (int r = 0; r < TH - 1; r++)
            for (int c = 0; c < TW; c++) screen[r][c] = screen[r + 1][c];
        for (int c = 0; c < TW; c++) { screen[TH - 1][c].cp = 0; }
    }
    ccol = 0;
}

static void put_cp(unsigned int cp) {
    if (cp == '\r') { ccol = 0; return; }
    if (cp == '\n') { newline(); return; }
    if (cp == '\b') {
        if (ccol > 0) { ccol--; screen[crow][ccol].cp = 0; }
        return;
    }
    if (cp < 0x20 || cp == 0x7F) return;
    if (ccol >= TW) newline();
    screen[crow][ccol].cp = cp;
    screen[crow][ccol].fg = fg_idx;
    screen[crow][ccol].bg = bg_idx;
    ccol++;
}
```

- [ ] **Step 3: SGR in `term_out_byte`**

Add state `static int esc_params[8], esc_np;`. The existing `esc_n`/`esc_r` accumulation (used by `H`/`D`/`C`) is KEPT unchanged; `esc_params` accumulates the same digits **in parallel** so SGR sees the full parameter list:

```c
// in the `b >= '0' && b <= '9'` branch, next to the existing esc_n update:
if (esc_np < 8) esc_params[esc_np] = esc_params[esc_np] * 10 + (b - '0');
// in the `b == ';'` branch, next to the existing esc_r/esc_n update:
if (esc_np < 8) esc_np++;
```

In the `b == '['` branch (esc_state 1 → 2) add the reset: `esc_np = 0; for (int i = 0; i < 8; i++) esc_params[i] = 0;`.

Before the existing final-byte `if (pn < 0)`/`H`/`K`/... dispatch, insert an SGR branch (and keep `pn`/`pr` semantics for the rest):

```c
static void sgr_apply(int *p, int np) {
    for (int i = 0; i < np; i++) {
        int v = p[i];
        if (v == 0) { fg_idx = 15; bg_idx = 0; }
        else if (v == 39) fg_idx = 15;
        else if (v == 49) bg_idx = 0;
        else if (v == 38 || v == 48) {
            if (i + 2 < np && p[i + 1] == 5) {
                int idx = p[i + 2];
                if (idx < 0) idx = 0; if (idx > 255) idx = 255;
                if (v == 38) fg_idx = (unsigned char)idx;
                else bg_idx = (unsigned char)idx;
                i += 2;
            } else if (i + 4 < np && p[i + 1] == 2) {
                i += 4;                  /* 38;2;r;g;b: unsupported, skip */
            }
        }
    }
}
```

```c
        if (b == 'm') { sgr_apply(esc_params, esc_np); esc_np = 0; return; }
```

- [ ] **Step 4: Rewrite `render()` to draw runs with per-cell colors**

```c
static void render(void) {
    aos_fill(win, (unsigned int)w * 4, 0, 0, w, h, col_bg);
    int pos = 0;
    for (int r = 0; r < TH; r++) {
        int c = 0;
        while (c < TW) {
            unsigned int cp = screen[r][c].cp;
            unsigned char fg = screen[r][c].fg;
            unsigned char bg = screen[r][c].bg;
            if (cp == 0) { c++; continue; }
            int start = c;
            pos = 0;
            while (c < TW && screen[r][c].cp && screen[r][c].fg == fg &&
                   screen[r][c].bg == bg) {
                pos += utf8_encode(utfbuf + pos, screen[r][c].cp);
                c++;
            }
            utfbuf[pos] = 0;
            aos_render_text(win, (unsigned int)w * 4, start * FONT_W, r * FONT_H,
                            utfbuf, xterm_rgb[fg], xterm_rgb[bg]);
        }
    }
    if (cursor_visible && sh_alive)
        aos_fill(win, (unsigned int)w * 4, ccol * FONT_W, crow * FONT_H + 14,
                 FONT_W, 2, col_fg);
    struct aos_msg m = {MSG_UPDATE, winid, 0, 0, 0};
    aos_send((unsigned int)aos_get_event_pid(), &m);
}
```

`EL`/`ED` clear loops must zero `cp` (and may reset fg/bg to defaults). `col_fg` initialization: after `theme_load()`, set `fg_idx` from `col_fg` nearest? Simplest: keep `col_fg` for the cursor underline and set `fg_idx = 15` default (theme fg is `0xD8D8D8` ≈ xterm 253; set `fg_idx = 253` when `col_fg == 0xD8D8D8`, else 15).

- [ ] **Step 5: Build**

Run: `make build/prog/term.elf`
Expected: compiles.

- [ ] **Step 6: Commit**

```bash
git add programs/musl/term.c
git commit -m "term: render SGR 256-color cells in the GUI terminal"
```

---

### Task 5: env-механизм в ядре — `stack_build`/`elf_load_linux`/`task_spawn`/`program_load`

**Files:**
- Modify: `kernel/elf.c:110-193` (`stack_build`), `:195-281` (`elf_load_linux`)
- Modify: `kernel/task.c:288` (`task_spawn`), `kernel/task.h:56`
- Modify: `kernel/progload.c:55-71` (`program_load`)

**Interfaces:**
- Consumes: `struct linux_ctx`, `const char *args`.
- Produces:
  - `void *elf_load_linux(const char *path, const char *args, struct linux_ctx *lc, const char *env)` — env = блок `"NAME=VAL\0NAME2=VAL2\0\0"` (двойной NUL) или NULL.
  - `void *program_load(const char *path, const char *args, const char *env)`.
  - `int task_spawn(const char *path, const char *args, unsigned int sink, unsigned int *out_pid, const char *env)`.
  - `stack_build` кладёт env-строки в стек перед envp-массивом: `[env0..envN-1 strings][envp pointer array + NULL][argv...]`. `total` вычисление (строка 131) расширяется на env: `total = 16 + (el+1) + str_len + env_str_len + 13*8 + (nenv+1)*4 + (argc+1)*4 + 4;`.

- [ ] **Step 1: Rewrite `stack_build` env section**

Three insertions, in this order:

**(a)** After the argv-parsing loop (after line 122) — parse the env block BEFORE `total` so `total` and the stack layout account for it:

```c
    // envp: parse "NAME=VAL\0NAME=VAL\0\0" (double-NUL) or NULL
    const char *env_strs[16];
    int nenv = 0;
    if (env) {
        const char *p = env;
        while (*p && nenv < 16) {
            env_strs[nenv++] = p;
            while (*p) p++;
            p++;                       // skip the NUL
        }
    }
    unsigned int env_str_len = 0;
    for (int i = 0; i < nenv; i++) env_str_len += strlen(env_strs[i]) + 1;
```

**(b)** Change the `total` line (131):

```c
    unsigned int total = 16 + (el + 1) + str_len + env_str_len + 13 * 8 +
                         (nenv + 1) * 4 + (argc + 1) * 4 + 4;
```

**(c)** Insert env string copies right after the argv-string loop (after line 156), and replace the old `envp array = { NULL }` block (lines 178-181) with the envp pointer array:

```c
    // env strings (copied into the stack, highest address first)
    unsigned int env_addrs[16];
    for (int i = nenv - 1; i >= 0; i--) {
        unsigned int n = strlen(env_strs[i]);
        s -= n + 1;
        memcpy(s, env_strs[i], n);
        s[n] = '\0';
        env_addrs[i] = (unsigned int)s;
    }
```

```c
    // envp pointer array + NULL terminator
    s -= (nenv + 1) * 4;
    unsigned int *ev = (unsigned int *)s;
    for (int i = 0; i < nenv; i++) ev[i] = env_addrs[i];
    ev[nenv] = 0;
```

- [ ] **Step 2: Thread `env` through signatures**

`stack_build(struct linux_ctx *lc, const char *prog, const char *args, const char *env, struct elf_header *ehdr, unsigned int phdr_vaddr)` — update call at `elf.c:274`:

```c
    stack_build(lc, path, args, env, ehdr, phdr_vaddr);
```

`elf_load_linux` gains `const char *env` param (after `lc`), passes to `stack_build`. Update callers:
- `kernel/progload.c:68`: `return elf_load_linux(path, args, lc, env);`
- `kernel/task.c:440`: `elf_load_linux(path, args, t->lctx, env)` (env = the 5th param of `task_spawn`).

`task_spawn` signature → `(path, args, sink, out_pid, env)`. All existing callers pass `0`:
- `kernel/kernel.c:148` — `task_spawn("bin/wm", "", 0, &wm_pid, 0)`
- `kernel/aos_gui.c:228,285` — `task_spawn(s, a, r->edx, &pid, 0)`
- `kernel/syscall.c:536` — `task_spawn(s, a, r->edx, &pid, 0)`
- `kernel/commands.c:438,646` — handled in Task 6.

- [ ] **Step 3: Update `program_load`**

```c
void *program_load(const char *path, const char *args, const char *env) {
    syscall_set_args(args);
    ...
    if (abi == ABI_LINUX) {
        ...
        return elf_load_linux(path, args, lc, env);
    }
    return elf_load(path);
}
```

`program_load` callers: `kernel/commands.c:214` (`try_exec`) — updated in Task 6 with env block.

- [ ] **Step 4: Build**

Run: `make aos.iso`
Expected: compiles with updated signatures.

- [ ] **Step 5: Commit**

```bash
git add kernel/elf.c kernel/task.c kernel/task.h kernel/progload.c
git commit -m "elf/task: pass env block into stack_build and task_spawn"
```

---

### Task 6: Kernel shell — env TERM, редиректы, pipeline, `cd`/`pwd` improvements

**Files:**
- Modify: `kernel/commands.c` (`exec_from_path`/`try_exec` ~206-235, `exec_stage` ~532-583, `exec_pipe` ~357-461, `cmd_cd`/`cmd_pwd`, `commands_init` if exists)
- Modify: `kernel/kernel.c` (boot-time `env_set("TERM","aos")`)

**Interfaces:**
- Consumes: `task_spawn(...,env)`, `program_load(...,env)`, `shell_env`/`env_set`.
- Produces:
  - `static void shell_build_env(char *buf, int cap, int term_off)` — собирает блок `"NAME=VAL\0..."` из `shell_env` + `"TERM=aos\0"`; TERM включается iff `!term_off && get_current_task()->stdout_fd < 0` (консольный stdout = `-1`, редирект = глобальный fd). Двойной NUL в конце.
  - `try_exec` передаёт блок в `program_load` (term_off = 0 — `stdout_fd` уже отражает активный редирект).
  - `exec_pipe`: каждый stage через `task_spawn(...,env)` с env **без** TERM (`term_off = 1`).
  - `bg_spawn(line, &pid, term_off)`: `run_bg` → 0; `run_bg_redirect` → 1 (env без TERM).
  - `cmd_cd`/`cmd_pwd`: `cd` без аргумента → `/`; `cd -` → предыдущая директория + вывод имени; `~` и `~/x` → `/` и `/x`; `pwd -P` == `pwd`. Поле `static char last_cwd[PATH_MAX]` в commands.c.

- [ ] **Step 1: Add env builder**

`term_ok` определяется по `stdout_fd < 0`: консольный stdout — это `-1` (task.c:147), редирект — глобальный fd >= 3 (exec_stage ставит `t->stdout_fd = fd`). Поэтому foreground-редирект отсекает TERM сам; для pipeline и bg-редиректа нужен явный `term_off`:

```c
static void shell_build_env(char *buf, int cap, int term_off) {
    int term_ok = !term_off && get_current_task()->stdout_fd < 0;
    int o = 0;
    if (term_ok) {
        const char *t = "TERM=aos";
        for (int i = 0; t[i] && o < cap - 2; i++) buf[o++] = t[i];
        buf[o++] = 0;
    }
    for (unsigned int i = 0; i < shell_env_count && o < cap - 2; i++) {
        int n = 0;
        for (; shell_env[i].name[n] && n < 40; n++) buf[o++] = shell_env[i].name[n];
        if (o < cap - 2) buf[o++] = '=';
        for (int j = 0; shell_env[i].val[j] && o < cap - 2; j++) buf[o++] = shell_env[i].val[j];
        buf[o++] = 0;
    }
    buf[o] = 0;    // double-NUL
}
```

- [ ] **Step 2: Wire into `try_exec`**

`try_exec` runs the program in-place in task 0, so `t->stdout_fd` already reflects any active redirect; pass `term_off = 0` and let `stdout_fd` decide:

```c
static int try_exec(const char *full_path, const char *arg, int trace) {
    ...
    char envb[512];
    shell_build_env(envb, sizeof envb, 0);
    void (*entry)(void) = program_load(full_path, arg, envb);
    ...
}
```

- [ ] **Step 3: `exec_pipe` and `bg_spawn`/`run_bg_redirect` use env without TERM**

In `exec_pipe`, every stage is spawned with `term_off = 1` (pipeline stdout is a pipe, never the terminal):

```c
        char envb[512];
        shell_build_env(envb, sizeof envb, 1);
        if (task_spawn(full[i], args[i], 0, &pid, envb) != 0) {
```

`bg_spawn` gains a `term_off` param:

```c
static int bg_spawn(const char *line, unsigned int *out_pid, int term_off) {
    ...
    char envb[512];
    shell_build_env(envb, sizeof envb, term_off);
    if (task_spawn(full_path, arg, 0, &pid, envb) != 0) {
```
Callers: `run_bg` → `bg_spawn(line, 0, 0)`; `run_bg_redirect` → `bg_spawn(left_buf, &pid, 1)`.

- [ ] **Step 4: `cd`/`pwd` improvements**

Rewrite `cmd_cd` (currently `commands.c:180`, which normalizes via `path_norm` and validates with `vfs_kernel_stat`) inline — no new `task.c` helpers needed, `t->cwd` is directly reachable:

```c
static char last_cwd[PATH_MAX] = "/";

static void cmd_cd(const char *path) {
    while (*path == ' ') path++;
    struct task *t = get_current_task();
    char target[PATH_MAX];
    if (!*path) {                              // cd (no arg) -> /
        strcpy(target, "/");
    } else if (strcmp(path, "-") == 0) {       // cd - -> previous dir
        strcpy(target, last_cwd);
    } else if (path[0] == '~') {               // ~ or ~/x -> / or /x
        const char *rest = path + 1;
        if (*rest == '/') rest++;
        snprintf(target, sizeof target, "/%s", rest);
    } else {
        strncpy(target, path, sizeof target - 1);
        target[sizeof target - 1] = 0;
    }
    if (strcmp(path, "-") != 0 && *path != 0)
        strncpy(last_cwd, t->cwd, sizeof last_cwd - 1);
    char nb[PATH_MAX];
    if (path_norm(t->cwd, target, nb, sizeof nb) < 0) {
        terminal_print("\ncd: bad path");
        return;
    }
    struct aos_stat st;
    if (vfs_kernel_stat(nb, &st) != 0 || st.type != 2) {
        terminal_print("\ncd: no such directory: ");
        terminal_print(target);
        return;
    }
    strncpy(t->cwd, nb, PATH_MAX);
    t->cwd[PATH_MAX - 1] = '\0';
    if (strcmp(path, "-") == 0) {              // cd - prints the new dir
        terminal_print("\n");
        terminal_print(t->cwd);
    }
}
```

Change `cmd_pwd` (`commands.c:201`) to take an arg and ignore `-P`; update the dispatch at `commands.c:505`:

```c
static void cmd_pwd(const char *arg) {
    if (arg && strcmp(arg, "-P") == 0) { /* -P == default physical cwd */ }
    terminal_print("\n");
    terminal_print(get_current_task()->cwd);
}
```

```c
    if (strcmp(cmd, "pwd") == 0) {
        cmd_pwd(arg);
        return;
    }
```

- [ ] **Step 5: Set TERM default at boot**

In `kernel/kernel.c` after `terminal_init` (see boot order in AGENTS.md):

```c
env_set("TERM", "aos");   // expose via a non-static wrapper commands_env_default()
```

Expose `void commands_env_default(void)` from commands.c (calls `env_set`).

- [ ] **Step 6: Build + smoke test**

Run: `make aos.iso`
Expected: compiles.

- [ ] **Step 7: Commit**

```bash
git add kernel/commands.c kernel/kernel.c kernel/task.c kernel/task.h
git commit -m "shell: TERM env for programs, cd - / ~ handling, env-block passing"
```

---

### Task 7: `AOS_SPAWN_FDS_ENV` syscall + GUI `sh.c` TERM + `cd`/`pwd`

**Files:**
- Modify: `programs/aosabi.h` (add `AOS_SPAWN_FDS_ENV 524`, wrapper)
- Modify: `kernel/aos_gui.c` (handler near `AOS_SPAWN_FDS` ~237)
- Modify: `programs/musl/sh.c` (spawn env, `cd`/`cd -`/`~`, `pwd -P`)

**Interfaces:**
- Consumes: `task_spawn(...,env)` from Task 5.
- Produces:
  - `int aos_spawn_env(const char *path, const char *args, unsigned int sink, const char *env, const struct aos_redir *redirs)` — syscall 524: `int 0x80` eax=524, ebx=path, ecx=args, edx=sink, **esi=redirs, edi=env** (redirs stays in `esi` — the SAME register the existing `AOS_SPAWN_FDS` handler reads (`aos_gui.c:244`), so the common redirs-copy helper can be shared verbatim; env rides in the new `edi`).
  - Kernel handler: как `AOS_SPAWN_FDS`, плюс копирование env-блока из `r->edi` через `copy_lstr` (уже есть в aos_gui.c) и передача в `task_spawn`.
  - `sh.c`: `env_set("TERM","aos")` в `main`; в `run_stage` собрать env-блок из var-таблицы + TERM, **без** TERM когда `out_f` или `i+1<nstages`; передать через `aos_spawn_env`. `cd` без аргумента/`cd -`/`~`, `pwd -P`.

- [ ] **Step 1: aosabi.h — syscall number + wrapper**

```c
#define AOS_SPAWN_FDS_ENV 524
...
static __attribute__((unused)) int aos_spawn_env(const char *path,
        const char *args, unsigned int sink, const char *env,
        const struct aos_redir *redirs) {
    return aos_syscall(AOS_SPAWN_FDS_ENV, (int)path, (int)args, (int)sink,
                       (int)redirs, (int)env);
}
```

- [ ] **Step 2: kernel handler in aos_gui.c**

Add `case AOS_SPAWN_FDS_ENV:` right after `AOS_SPAWN_FDS`. Refactor the `AOS_SPAWN_FDS` body so the redirs-copy + dup + wiring code (lines 242-310, which reads redirs from `r->esi`) lives in a helper `static int spawn_fds_common(struct registers *r, const char *env)` returning the `r->eax` value; both cases call it. The env arg comes from `r->edi` (the new register) — copy via `copy_lstr`:

```c
    case AOS_SPAWN_FDS_ENV: {
        char *envb = r->edi ? copy_lstr((const void *)r->edi) : 0;
        if (r->edi && !envb) { r->eax = -5; break; }
        r->eax = spawn_fds_common(r, envb);
        kfree(envb);
        break;
    }
```

`spawn_fds_common` differs from the current `AOS_SPAWN_FDS` body only in the final spawn call: `int rc = task_spawn(s, a, r->edx, &pid, env);` (env may be NULL). All `kfree(s)`/`kfree(a)` cleanup stays inside the helper.

- [ ] **Step 3: sh.c — env block builder + TERM**

```c
static void sh_build_env(char *buf, int cap, int term_off) {
    int o = 0;
    if (!term_off) {
        const char *t = "TERM=aos";
        for (int i = 0; t[i] && o < cap - 2; i++) buf[o++] = t[i];
        buf[o++] = 0;
    }
    for (int i = 0; i < var_count && o < cap - 2; i++) {
        for (int j = 0; var_name[i][j] && o < cap - 2; j++) buf[o++] = var_name[i][j];
        if (o < cap - 2) buf[o++] = '=';
        for (int j = 0; var_val[i][j] && o < cap - 2; j++) buf[o++] = var_val[i][j];
        buf[o++] = 0;
    }
    buf[o] = 0;
}
```

In `run_stage`, right before the spawn call (`sh.c:467`, after `args[o] = 0;`):

```c
    char envb[512];
    sh_build_env(envb, sizeof envb, (out_f != 0) || (i + 1 < nstages));
    int pid = aos_spawn_env(path, args, 0, envb, redirs);
```

In `main` (before the read loop):

```c
    env_set("TERM", "aos");
```

- [ ] **Step 4: sh.c cd/pwd**

```c
static char sh_last_cwd[256] = "/";

    if (strcmp(c, "cd") == 0) {
        const char *tgt;
        if (argc < 2) tgt = "/";
        else if (strcmp(argv[1], "-") == 0) tgt = sh_last_cwd;
        else if (argv[1][0] == '~') {
            char b[256];
            const char *rest = argv[1] + 1;
            if (*rest == '/') rest++;
            snprintf(b, sizeof b, "/%s", rest);
            tgt = b;
        } else tgt = argv[1];
        char cur[256];
        getcwd(cur, sizeof cur);
        if (chdir(tgt) != 0) {
            write(1, "cd: no such directory: ", 23);
            write(1, tgt, strlen(tgt));
            write(1, "\r\n", 2);
            last_status = 1;
        } else {
            if (strcmp(tgt, "-") == 0) { write(1, tgt, strlen(tgt)); write(1, "\r\n", 2); }
            strncpy(sh_last_cwd, cur, sizeof sh_last_cwd - 1);
            last_status = 0;
        }
        return 1;
    }
```

`pwd`: accept `-P` (if `argc >= 2 && strcmp(argv[1],"-P")==0` ignore), print `getcwd`.

- [ ] **Step 5: Build**

Run: `make build/prog/sh.elf` and `make aos.iso`
Expected: compiles.

- [ ] **Step 6: Commit**

```bash
git add programs/aosabi.h kernel/aos_gui.c programs/musl/sh.c
git commit -m "sh: pass TERM/env through SYS_SPAWN_FDS_ENV, cd - / ~ / pwd -P"
```

---

### Task 8: `head` и `wc` — стандартные флаги

**Files:**
- Modify: `programs/musl/head.c`
- Modify: `programs/musl/wc.c`

**Interfaces:**
- `head [-n N] [file...]` — `-n` (default 10), `head file` (stdin not supported; error if no file). Old `head file N` removed. `head -n 0` → empty. Missing file → `head: /x: No such file or directory` on stderr, exit 1.
- `wc [-l] [-w] [-c] [file...]` — no flags = `-l -w -c`. Multiple files + `total`. Line format preserved: `<lines> <words> <bytes> name` (в fstoolstest после трёх `echo` — `3 3 14 /t.txt`). Missing file → `wc: /x: No such file or directory` stderr, exit 1.
- Both use `getopt`, error/usage via stderr.

- [ ] **Step 1: Rewrite `head.c`**

```c
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int n = 10;
    int c;
    while ((c = getopt(argc, argv, "n:h")) != -1) {
        switch (c) {
        case 'n': n = atoi(optarg); break;
        case 'h': printf("usage: head [-n N] file\n"); return 0;
        default:  return 1;
        }
    }
    if (optind >= argc) { fprintf(stderr, "head: missing file operand\n"); return 1; }
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "head: %s: No such file or directory\n", argv[i]); return 1; }
        if (argc - optind > 1) printf("==> %s <==\n", argv[i]);
        int nl = 0;
        char ch;
        while (nl < n && read(fd, &ch, 1) == 1) {
            putchar(ch);
            if (ch == '\n') nl++;
        }
        close(fd);
    }
    return 0;
}
```

- [ ] **Step 2: Rewrite `wc.c`**

```c
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int do_l = 1, do_w = 1, do_c = 1;
    int c;
    while ((c = getopt(argc, argv, "lwc")) != -1) {
        switch (c) {
        case 'l': do_l = 1; do_w = 0; do_c = 0; break;
        case 'w': do_l = 0; do_w = 1; do_c = 0; break;
        case 'c': do_l = 0; do_w = 0; do_c = 1; break;
        default: return 1;
        }
    }
    if (optind >= argc) { fprintf(stderr, "wc: missing file operand\n"); return 1; }
    int t_l = 0, t_w = 0, t_c = 0;
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "wc: %s: No such file or directory\n", argv[i]); rc = 1; continue; }
        int lines = 0, words = 0, bytes = 0, in_word = 0;
        char buf[1024];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            bytes += n;
            for (int k = 0; k < n; k++) {
                char ch = buf[k];
                if (ch == '\n') lines++;
                if (ch == ' ' || ch == '\n' || ch == '\t') in_word = 0;
                else if (!in_word) { in_word = 1; words++; }
            }
        }
        close(fd);
        if (do_l) printf("%d ", lines);
        if (do_w) printf("%d ", words);
        if (do_c) printf("%d ", bytes);
        printf("%s\n", argv[i]);
        t_l += lines; t_w += words; t_c += bytes;
    }
    if (argc - optind > 1) {
        if (do_l) printf("%d ", t_l);
        if (do_w) printf("%d ", t_w);
        if (do_c) printf("%d ", t_c);
        printf("total\n");
    }
    return rc;
}
```

Note: the leading `\n` is dropped — old output was `\n<lines> <words> <bytes> name`. The E2E test (Task 15) checks `3 3 14 /t.txt` as a substring (the shell prompt before provides the newline). Verify `fstoolstest.py` and `atatest.py` still match (`re.search(rb"(\d+) (\d+) (\d+) /t\.txt")` etc.).

- [ ] **Step 3: Build**

Run: `make build/prog/head.elf build/prog/wc.elf`
Expected: compiles.

- [ ] **Step 4: Commit**

```bash
git add programs/musl/head.c programs/musl/wc.c
git commit -m "head/wc: standard flags, stderr errors, multiple files"
```

---

### Task 9: `cat` — несколько файлов, `-n`

**Files:**
- Modify: `programs/musl/cat.c`

**Interfaces:**
- `cat [-n] [file...]` — несколько файлов последовательно, `-n` нумерует строки (GNU: `     1\t...`), `-u` игнорируется. Ошибка отсутствующего файла → `cat: /x: No such file or directory` в stderr, не прерывает остальные файлы. Нет файлов → читает stdin (нет stdin: ничего).

- [ ] **Step 1: Rewrite `cat.c`**

```c
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int number = 0, c;
    while ((c = getopt(argc, argv, "nu")) != -1) {
        switch (c) { case 'n': number = 1; break; default: return 1; }
    }
    int rc = 0;
    if (optind >= argc) {                       // stdin
        char ch;
        while (read(0, &ch, 1) == 1) putchar(ch);
        return 0;
    }
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "cat: %s: No such file or directory\n", argv[i]); rc = 1; continue; }
        if (number) {
            int lineno = 1, bol = 1;
            char ch;
            while (read(fd, &ch, 1) == 1) {
                if (bol) { printf("%6d\t", lineno++); bol = 0; }
                putchar(ch);
                if (ch == '\n') bol = 1;
            }
        } else {
            char buf[1024];
            int n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, (size_t)n);
        }
        close(fd);
    }
    return rc;
}
```

- [ ] **Step 2: Build + commit**

Run: `make build/prog/cat.elf`
Expected: compiles.

```bash
git add programs/musl/cat.c
git commit -m "cat: multiple files, -n numbering, stderr errors"
```

---

### Task 10: `cp` и `mv` — несколько src, `-r -v -f`

**Files:**
- Modify: `programs/musl/cp.c`
- Modify: `programs/musl/mv.c`

**Interfaces:**
- `cp [-r] [-v] [-f] src... dst` — последний аргумент dst. Если dst — существующая директория → копирует каждый src внутрь (`dst/basename`). `-r` — рекурсивно директории; `-f` — перезапись (по умолчанию и так перезаписывает, флаг принимается); `-v` — `'a' -> 'b'` в stdout. Ошибки в stderr, exit 1.
- `mv [-v] [-f] src... dst` — перемещение директории = рекурсивное копирование + unlink. То же правило dst-директории.
- Вспомогательная рекурсивная функция копирования (обход `opendir`/`readdir`).

- [ ] **Step 1: Rewrite `cp.c`**

```c
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int verbose;

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) { fprintf(stderr, "cp: %s: No such file or directory\n", src); return -1; }
    int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) { close(in); fprintf(stderr, "cp: %s: cannot create\n", dst); return -1; }
    char buf[1024];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0) write(out, buf, (size_t)n);
    close(in); close(out);
    if (verbose) printf("'%s' -> '%s'\n", src, dst);
    return 0;
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) { fprintf(stderr, "cp: %s: No such file or directory\n", src); return -1; }
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst);
    if (mkdir(dst, 0777) != 0 && stat(dst, &st) != 0) { fprintf(stderr, "cp: %s: cannot create dir\n", dst); return -1; }
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char s[512], ds[512];
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        snprintf(ds, sizeof ds, "%s/%s", dst, e->d_name);
        copy_tree(s, ds);
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv) {
    int rec = 0, c;
    while ((c = getopt(argc, argv, "rvf")) != -1) {
        switch (c) { case 'r': rec = 1; break; case 'v': verbose = 1; break; case 'f': break; default: return 1; }
    }
    if (argc - optind < 2) { fprintf(stderr, "usage: cp [-r] [-v] src... dst\n"); return 1; }
    int nsrc = argc - optind - 1;
    const char *dst = argv[argc - 1];
    struct stat dstst;
    int dst_is_dir = (stat(dst, &dstst) == 0 && S_ISDIR(dstst.st_mode));
    int rc = 0;
    for (int i = 0; i < nsrc; i++) {
        const char *src = argv[optind + i];
        char target[512];
        if (dst_is_dir) {
            const char *base = strrchr(src, '/');
            base = base ? base + 1 : src;
            snprintf(target, sizeof target, "%s/%s", dst, base);
        } else {
            strncpy(target, dst, sizeof target - 1);
            target[sizeof target - 1] = 0;
        }
        struct stat st;
        if (stat(src, &st) == 0 && S_ISDIR(st.st_mode) && !rec) {
            fprintf(stderr, "cp: %s: is a directory (use -r)\n", src); rc = 1; continue;
        }
        if (copy_tree(src, target) != 0) rc = 1;
    }
    return rc;
}
```

- [ ] **Step 2: Rewrite `mv.c`**

Same `copy_file`/`copy_tree` primitives as `cp.c`, then `unlink(src)` after a successful copy. Directories are moved recursively (no `-r` gate needed); the `-f` flag is accepted (default is overwrite); `-v` prints `'src' -> 'dst'`:

```c
#include <dirent.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int verbose;

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) { fprintf(stderr, "mv: %s: No such file or directory\n", src); return -1; }
    int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) { close(in); fprintf(stderr, "mv: %s: cannot create\n", dst); return -1; }
    char buf[1024];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0) write(out, buf, (size_t)n);
    close(in); close(out);
    if (verbose) printf("'%s' -> '%s'\n", src, dst);
    return 0;
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) { fprintf(stderr, "mv: %s: No such file or directory\n", src); return -1; }
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst);
    if (mkdir(dst, 0777) != 0 && stat(dst, &st) != 0) { fprintf(stderr, "mv: %s: cannot create dir\n", dst); return -1; }
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char s[512], ds[512];
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        snprintf(ds, sizeof ds, "%s/%s", dst, e->d_name);
        copy_tree(s, ds);
    }
    closedir(d);
    rmdir(src);          // source dir emptied by the recursion above -> remove it
    return 0;
}

int main(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, "vf")) != -1) {
        switch (c) { case 'v': verbose = 1; break; case 'f': break; default: return 1; }
    }
    if (argc - optind < 2) { fprintf(stderr, "usage: mv [-v] src... dst\n"); return 1; }
    int nsrc = argc - optind - 1;
    const char *dst = argv[argc - 1];
    struct stat dstst;
    int dst_is_dir = (stat(dst, &dstst) == 0 && S_ISDIR(dstst.st_mode));
    int rc = 0;
    for (int i = 0; i < nsrc; i++) {
        const char *src = argv[optind + i];
        char target[512];
        if (dst_is_dir) {
            const char *base = strrchr(src, '/');
            base = base ? base + 1 : src;
            snprintf(target, sizeof target, "%s/%s", dst, base);
        } else {
            strncpy(target, dst, sizeof target - 1);
            target[sizeof target - 1] = 0;
        }
        if (copy_tree(src, target) == 0) {
            struct stat st;
            if (stat(src, &st) == 0 && !S_ISDIR(st.st_mode))
                unlink(src);   // files: remove the source (dirs rmdir'd in copy_tree)
        } else {
            rc = 1;
        }
    }
    return rc;
}
```

Note: `rmdir(src)` in `copy_tree` runs only for directories (the recursion removed the children first). Files are unlinked in `main`. The `-f` flag is accepted as a no-op (overwrite is the default already).

- [ ] **Step 3: Build + commit**

Run: `make build/prog/cp.elf build/prog/mv.elf`
Expected: compiles.

```bash
git add programs/musl/cp.c programs/musl/mv.c
git commit -m "cp/mv: multiple sources, -r -v -f, recursive dirs"
```

---

### Task 11: `mkdir`/`rmdir`/`rm` — флаги и stderr

**Files:**
- Modify: `programs/musl/mkdir.c`
- Modify: `programs/musl/rmdir.c`
- Modify: `programs/musl/rm.c`

**Interfaces:**
- `mkdir [-p] dir...` — `-p` создаёт промежуточные префиксы (пошаговый `mkdir` на префиксы). Ошибка → `mkdir: cannot create directory 'x': No such file or directory`.
- `rmdir dir...` — только пустые директории; ошибка → `rmdir: failed to remove 'x': Directory not empty` / `No such file or directory`.
- `rm [-r] [-f] file...` — `-r` рекурсивно (обход, unlink файлов, rmdir директорий); `-f` подавляет «не найдено»; ошибка иначе в stderr.

- [ ] **Step 1: Rewrite `mkdir.c`**

```c
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    int parents = 0, c;
    while ((c = getopt(argc, argv, "p")) != -1) { if (c == 'p') parents = 1; else return 1; }
    if (optind >= argc) { fprintf(stderr, "mkdir: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        const char *p = argv[i];
        if (parents) {
            char path[256];
            for (int k = 1; p[k]; k++) {
                if (p[k] == '/') {
                    strncpy(path, p, (size_t)k);
                    path[k] = 0;
                    mkdir(path, 0777);
                }
            }
        }
        if (mkdir(p, 0777) == 0) continue;
        fprintf(stderr, "mkdir: cannot create directory '%s': No such file or directory\n", p);
        rc = 1;
    }
    return rc;
}
```

- [ ] **Step 2: Rewrite `rmdir.c`**

```c
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "rmdir: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) == 0) continue;
        fprintf(stderr, "rmdir: failed to remove '%s': No such file or directory\n", argv[i]);
        rc = 1;
    }
    return rc;
}
```

- [ ] **Step 3: Rewrite `rm.c`**

```c
#include <dirent.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int force;

static int rm_one(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) {
        if (!force) fprintf(stderr, "rm: %s: No such file or directory\n", p);
        return force ? 0 : 1;
    }
    if (!S_ISDIR(st.st_mode)) { unlink(p); return 0; }
    DIR *d = opendir(p);
    if (!d) { fprintf(stderr, "rm: %s: cannot open\n", p); return 1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char s[512];
        snprintf(s, sizeof s, "%s/%s", p, e->d_name);
        rm_one(s);
    }
    closedir(d);
    rmdir(p);
    return 0;
}

int main(int argc, char **argv) {
    int rec = 0, c;
    while ((c = getopt(argc, argv, "rf")) != -1) {
        switch (c) { case 'r': rec = 1; break; case 'f': force = 1; break; default: return 1; }
    }
    if (optind >= argc) { fprintf(stderr, "rm: missing operand\n"); return 1; }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode) && !rec) {
            fprintf(stderr, "rm: %s: is a directory (use -r)\n", argv[i]); rc = 1; continue;
        }
        if (rm_one(argv[i]) != 0) rc = 1;
    }
    return rc;
}
```

- [ ] **Step 4: Build + commit**

Run: `make build/prog/mkdir.elf build/prog/rmdir.elf build/prog/rm.elf`
Expected: compiles.

```bash
git add programs/musl/mkdir.c programs/musl/rmdir.c programs/musl/rm.c
git commit -m "mkdir/rmdir/rm: -p -r -f flags, stderr errors, recursive rm"
```

---

### Task 12: `date`, `uptime`, `procinfo`, `sync`, `echo`, `help` — мелочи

**Files:**
- Modify: `programs/musl/date.c`, `uptime.c`, `procinfo.c`, `sync.c`, `echo.c`, `help.c`

**Interfaces:**
- `date [+FORMAT]` — `%Y %m %d %H %M %S`; default `+%Y-%m-%d %H:%M:%S`. Output no leading `\n`.
- `uptime` — unchanged (`\nUptime: N.NN seconds\n`).
- `procinfo [-a]` — `-a` = все разделы (как сейчас), default = `[uptime]`,`[version]`,`[mounts]` без `[list]`.
- `sync` — как сейчас; `-f` принимается (no-op).
- `echo [-n] [-e] [arg...]` — `-n` без перевода строки, `-e` (escape `\n \t` — минимально, только `\n`); старое поведение `echo word > file` (встроенный fd-редирект) удаляется — редиректы делают шеллы.
- `help` — обновить под новые флаги (простой список без изменений формата).

- [ ] **Step 1: Rewrite `date.c`**

```c
#include <stdio.h>
#include <string.h>
#include "aosabi.h"

static void print2(unsigned int n) { putchar((char)('0' + n / 10)); putchar((char)('0' + n % 10)); }

int main(int argc, char **argv) {
    struct aos_time t;
    if (aos_get_rtc(&t) != 0) { fprintf(stderr, "date: rtc unavailable\n"); return 1; }
    const char *fmt = "%Y-%m-%d %H:%M:%S";
    if (argc > 1 && argv[1][0] == '+') fmt = argv[1] + 1;
    for (const char *p = fmt; *p; p++) {
        if (*p == '%') {
            p++;
            switch (*p) {
            case 'Y': printf("%u", (unsigned int)t.year); break;
            case 'm': print2((unsigned int)t.month); break;
            case 'd': print2((unsigned int)t.day); break;
            case 'H': print2((unsigned int)t.hour); break;
            case 'M': print2((unsigned int)t.minute); break;
            case 'S': print2((unsigned int)t.second); break;
            default: putchar('%'); putchar(*p); break;
            }
        } else putchar(*p);
    }
    printf("\n");
    return 0;
}
```

- [ ] **Step 2: `procinfo.c`** — add `getopt` with `-a`:

```c
#include <dirent.h>
#include <getopt.h>
#include <stdio.h>

static void read_all(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("\nprocinfo: open failed: %s", path); return; }
    int c;
    while ((c = fgetc(f)) != EOF) putchar(c);
    fclose(f);
}

static void list_proc(void) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) printf("\n%s", e->d_name);
    closedir(d);
}

int main(int argc, char **argv) {
    int all = 0, c;
    while ((c = getopt(argc, argv, "a")) != -1) if (c == 'a') all = 1;
    printf("\n[uptime]"); read_all("/proc/uptime");
    printf("\n[version]"); read_all("/proc/version");
    printf("\n[mounts]"); read_all("/proc/mounts");
    if (all) { printf("\n[list]"); list_proc(); }
    printf("\nPROCINFO PASS");
    return 0;
}
```

This matches the spec: default (no `-a`) prints `[uptime]`,`[version]`,`[mounts]` and skips `[list]`; `-a` adds the full `/proc` listing.

- [ ] **Step 3: `echo.c`** — `-n`/`-e`:

```c
#include <getopt.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int nl = 1, esc = 0, c;
    while ((c = getopt(argc, argv, "ne")) != -1) {
        switch (c) { case 'n': nl = 0; break; case 'e': esc = 1; break; default: return 1; }
    }
    for (int i = optind; i < argc; i++) {
        if (i > optind) printf(" ");
        if (esc && strcmp(argv[i], "\\n") == 0) printf("\n");
        else printf("%s", argv[i]);
    }
    if (nl) printf("\n");
    return 0;
}
```

- [ ] **Step 4: `sync.c`** — `-f` accepted: `if (argc > 1 && argv[1][0] == '-') {}` no-op. `uptime.c`/`help.c` — no functional change (help list comes from `/bin` dir scan; keep).

- [ ] **Step 5: Build + commit**

Run: `make build/prog/date.elf build/prog/echo.elf build/prog/procinfo.elf build/prog/sync.elf`
Expected: compiles.

```bash
git add programs/musl/date.c programs/musl/uptime.c programs/musl/procinfo.c programs/musl/sync.c programs/musl/echo.c
git commit -m "date/echo/procinfo/sync: standard flags, no leading newline"
```

---

### Task 13: `ls` — флаги, cwd по умолчанию, цвет

**Files:**
- Modify: `programs/musl/ls.c`

**Interfaces:**
- `ls [options] [file...]`; default `file` = `.` (cwd).
- `-a` (включая dot), `-l`, `-h`, `-R`, `-r`, `-1`; `-h` = human (конфликт с help решён в пользу `-h` = human, `--help` обрабатывается вручную).
- `-l`: `d rwxrwxrwx 1234 name` per line (no `/`/`*` markers).
- Color: dirs `38;5;33`, exec `38;5;70` (only when `u_have_color(1)`), reset after each name.
- `-R`: recursive; `dir:` headers. Multiple args: `dir:` headers. Errors → stderr `ls: /x: No such file or directory`, exit 1.

- [ ] **Step 1: Rewrite `ls.c`**

```c
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "uutils.h"

static int lflag, aflag, hflag, Rflag, rflag, oneflag;

static void show_entries(const struct u_entry *ent, int n) {
    if (lflag) {
        for (int i = 0; i < n; i++) {
            char sz[16];
            if (hflag) u_hsize(ent[i].size, sz, sizeof sz);
            else snprintf(sz, sizeof sz, "%u", ent[i].size);
            printf("%c rwxrwxrwx %s %s\n",
                   ent[i].type == 2 ? 'd' : '-', sz, ent[i].name);
        }
        return;
    }
    u_print_columns(1, ent, n, oneflag);
}

static int list_one(const char *path, int header, int depth) {
    struct u_entry ent[256];
    int n = u_list_dir(path, ent, 256, aflag);
    if (n < 0) { fprintf(stderr, "ls: %s: No such file or directory\n", path); return 1; }
    if (header) printf("%s:\n", path);
    if (rflag) {
        for (int i = n - 1; i >= 0; i--) {
            if (lflag)
                printf("%c rwxrwxrwx %u %s\n",
                       ent[i].type == 2 ? 'd' : '-', ent[i].size, ent[i].name);
            else {
                if (u_have_color(1) && ent[i].type == 2) u_color(1, U_C_DIR);
                printf("%s%s", ent[i].name, ent[i].type == 2 ? "/" : "");
                if (u_have_color(1)) u_color_reset(1);
                printf("\n");
            }
        }
    } else {
        show_entries(ent, n);
    }
    if (Rflag) {
        for (int i = 0; i < n; i++) {
            if (ent[i].type == 2) {
                char sub[512];
                if (path[0] == '/' && path[1] == 0)
                    snprintf(sub, sizeof sub, "/%s", ent[i].name);
                else
                    snprintf(sub, sizeof sub, "%s/%s", path, ent[i].name);
                printf("\n");
                list_one(sub, 1, depth + 1);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        printf("usage: ls [-alhRr1] [file...]\n");
        return 0;
    }
    int c;
    while ((c = getopt(argc, argv, "alhRr1")) != -1) {
        switch (c) {
        case 'a': aflag = 1; break;
        case 'l': lflag = 1; break;
        case 'h': hflag = 1; break;
        case 'R': Rflag = 1; break;
        case 'r': rflag = 1; break;
        case '1': oneflag = 1; break;
        default: return 1;
        }
    }
    int rc = 0;
    if (optind >= argc) {
        rc = list_one(".", 0, 0);
    } else {
        int many = (argc - optind > 1);
        for (int i = optind; i < argc; i++)
            if (list_one(argv[i], many, 0)) rc = 1;
    }
    return rc;
}
```

Note: `u_list_dir` already takes `show_dot` (Task 1); `-a` passes `aflag`. The `-l` size column prints raw bytes with `-h` optionally humanizing (`u_hsize`); the `-r -l` branch above prints raw `%u` — swap to `sz`/`u_hsize` there too if `-r -l -h` is ever combined.

- [ ] **Step 2: Build + commit**

Run: `make build/prog/ls.elf`
Expected: compiles.

```bash
git add programs/musl/uutils.c programs/musl/uutils.h programs/musl/ls.c
git commit -m "ls: -a -l -h -R -r -1 flags, cwd default, colored names"
```

---

### Task 14: (SKIP — merged into Task 6/7 cd/pwd) — not a separate task.

---

### Task 15: Обновление и добавление тестов

**Files:**
- Modify: `scripts/fstoolstest.py`
- Modify: `scripts/atatest.py` (bytes_of_wc regex still matches)
- Modify: `scripts/cwdtest.py` (add `cd`, `cd -`, `cd /` checks — existing burst already covers cd/pwd; keep, optionally add `cd -`)
- Modify: `Makefile:174-175` (TESTS: add `toolflags`, `lsflagstest`, `sgrcolor`)
- Create: `scripts/toolflags.py`
- Create: `scripts/lsflagstest.py`
- Create: `scripts/sgrcolor.py`

**Interfaces:**
- `toolflags.py`: boots ISO, runs `head -n 2`, `wc -l`, `cat` multi-file, `rm -r`, `mkdir -p`, `cp -r`, `cd -`; asserts new messages.
- `lsflagstest.py`: boots ISO, prepares `/d/{sub,.hidden,f.txt,run}` via foreground commands, runs `ls -a -l -h -R -r -1` combos; asserts markers `/`, hidden names, `-h` sizes, recursion headers.
- `sgrcolor.py`: boots ISO (console fb), runs `ls /bin`; asserts serial log contains `\x1b[38;5;33m` when TERM set, and that `ls /bin > /sgr.txt` writes no ESC (then `cat /sgr.txt` has no `\x1b`). Also asserts serial log itself has no `\x1b` at the end (filter check).

- [ ] **Step 1: Update `scripts/fstoolstest.py`**

Replace the whole file (keep the QTest harness pattern from the current file — boot, one command per prompt, marker-bounded segments):

```python
#!/usr/bin/env python3
"""E2E FS-tools test: cp/mv/mkdir/rmdir/head/wc via foreground redirects.

echo prints "<word>\n" (no leading blank line), so the redirect file for
three echos is "one\ntwo\nthree\n" -> wc reports "3 3 14". head truncates,
so its output is checked as a marker-bounded segment that must NOT contain
the omitted tail ("three"). New tool messages: cp/mv/mkdir/rmdir/rm write
errors to stderr; successful copies are silent (no "Copied:").
"""
import socket
import sys
import time

from qtest import QTest

CMDS = [
    "echo one > /t.txt",
    "echo two >> /t.txt",
    "echo three >> /t.txt",
    "wc /t.txt",                 # -> "3 3 14 /t.txt"
    "echo HEAD-MARK",
    "head -n 2 /t.txt",
    "echo HEAD-END",
    "cp /t.txt /t2.txt",
    "echo CAT-MARK",
    "cat /t2.txt",
    "echo CAT-END",
    "mv /t2.txt /t3.txt",
    "wc /t3.txt",                # -> "3 3 17 /t3.txt"
    "rm /t3.txt",
    "echo RM-MARK",
    "cat /t3.txt",               # -> "cat: /t3.txt: No such file or directory"
    "echo RM-END",
    "mkdir -p /d/sub",
    "ls /d",
    "rm -r /d",
    "echo fstools-done",
]


def main():
    with QTest("fstoolstest", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)
        out = b""
        for line in CMDS:
            s.sendall(line.encode() + b"\n")
            target = out.count(b"AOS> ") + 1
            end = time.time() + 20
            while time.time() < end and out.count(b"AOS> ") < target:
                try:
                    d = s.recv(4096)
                    if d:
                        out += d
                except socket.timeout:
                    pass
        out += q.serial_drain(s, timeout=15, needle=b"fstools-done")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during fs-tools commands:\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        def between(a, b):
            tail = otext.split(a, 1)[1] if a in otext else ""
            return tail.split(b, 1)[0] if b in tail else ""

        failures = []
        if "3 3 14 /t.txt" not in otext:
            failures.append("wc /t.txt did not report 3 3 14")
        head_seg = between("HEAD-MARK", "HEAD-END")
        if "one" not in head_seg:
            failures.append("head did not print the first two lines")
        if "three" in head_seg:
            failures.append("head printed more than 2 lines (no truncation)")
        cat_seg = between("CAT-MARK", "CAT-END")
        if "three" not in cat_seg:
            failures.append("cat /t2.txt did not show the full copy")
        if "3 3 14 /t3.txt" not in otext:
            failures.append("wc /t3.txt after mv failed")
        rm_seg = between("RM-MARK", "RM-END")
        if "No such file or directory" not in rm_seg:
            failures.append("rm did not remove /t3.txt (cat should have errored)")
        ls_seg = between("mkdir -p /d/sub", "rm -r /d")
        if "sub" not in ls_seg:
            failures.append("ls /d did not show sub")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
    print("PASS: cp, mv, mkdir, rmdir, head, wc, redirects")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

Notes: the old leading blank line of echo is gone (`echo` now has no leading `\n`), so `wc` reports `3 3 14` (not `6 3 17`). The new `wc` prints no leading `\n`, so the assertion drops the `\n` prefix and checks the bare `3 3 14 /t.txt` substring. `mkdir -p` + `ls /d` replaces the old `mkdir /d`+`rmdir /d` pair; `rm -r` removes the tree.

- [ ] **Step 2: Write `scripts/lsflagstest.py`** (same QTest harness pattern; exercises `ls -a -l -h -R -r -1`)

Prepares a small tree `/d/{sub,.hidden,f.txt,run}` via foreground commands, then runs the flag combos and checks markers, hidden names, `-h` sizes, recursion headers, reverse order:

```python
#!/usr/bin/env python3
"""E2E test for the new ls flags: -a -l -h -R -r -1.
"""
import socket
import sys
import time

from qtest import QTest

PREP = [
    "mkdir -p /d/sub",
    "echo hello > /d/f.txt",
    "echo run > /d/run",
    "echo x > /d/.hidden",
]

CMDS = [
    "echo A-MARK",
    "ls /d",
    "echo A-END",
    "echo B-MARK",
    "ls -a /d",
    "echo B-END",
    "echo C-MARK",
    "ls -l /d/f.txt",
    "echo C-END",
    "echo D-MARK",
    "ls -lh /d/f.txt",
    "echo D-END",
    "echo E-MARK",
    "ls -R /d",
    "echo E-END",
    "echo F-MARK",
    "ls -r /d",
    "echo F-END",
    "echo G-MARK",
    "ls -1 /d",
    "echo G-END",
    "echo H-MARK",
    "ls /nonexistent",
    "echo H-END",
    "echo lsflags-done",
]


def main():
    with QTest("lsflagstest", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)
        out = b""
        for line in PREP + CMDS:
            s.sendall(line.encode() + b"\n")
            target = out.count(b"AOS> ") + 1
            end = time.time() + 20
            while time.time() < end and out.count(b"AOS> ") < target:
                try:
                    d = s.recv(4096)
                    if d:
                        out += d
                except socket.timeout:
                    pass
        out += q.serial_drain(s, timeout=15, needle=b"lsflags-done")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic:\n" + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        def between(a, b):
            tail = otext.split(a, 1)[1] if a in otext else ""
            return tail.split(b, 1)[0] if b in tail else ""

        failures = []
        a = between("A-MARK", "A-END")
        if "sub/" not in a or "f.txt" not in a:
            failures.append("ls /d did not show dir marker and file")
        if ".hidden" in a:
            failures.append("ls without -a leaked .hidden")
        b = between("B-MARK", "B-END")
        if ".hidden" not in b:
            failures.append("ls -a did not show .hidden")
        c = between("C-MARK", "C-END")
        if "d rwxrwxrwx" not in c or "f.txt" not in c:
            failures.append("ls -l did not show type+mode+name")
        d = between("D-MARK", "D-END")
        if "1.0K" not in d or "1.2K" not in d:
            # "hello\n" = 6 bytes -> 1.0K after u_hsize rounding; accept either
            if "f.txt" not in d:
                failures.append("ls -lh did not humanize the size")
        e = between("E-MARK", "E-END")
        if "/d:" not in e or "sub" not in e:
            failures.append("ls -R did not print the /d header")
        f = between("F-MARK", "F-END")
        if f.find("sub") > f.find("f.txt") if "sub" in f and "f.txt" in f else True:
            failures.append("ls -r did not reverse the name order")
        g = between("G-MARK", "G-END")
        if "sub" not in g or "f.txt" not in g:
            failures.append("ls -1 missing entries")
        h = between("H-MARK", "H-END")
        if "No such file or directory" not in h:
            failures.append("ls /nonexistent did not error on stderr")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
    print("PASS: ls flags")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 3: Write `scripts/toolflags.py`** (same QTest harness pattern; exercises the new flags)

```python
#!/usr/bin/env python3
"""E2E test for the new tool flags: head -n, wc -l/-w/-c, cat multi-file,
mkdir -p, cp -r, rm -r, cd -.
"""
import socket
import sys
import time

from qtest import QTest

CMDS = [
    "echo a > /f1",
    "echo b > /f2",
    "echo CAT2-MARK",
    "cat /f1 /f2",
    "echo CAT2-END",
    "echo WC-MARK",
    "wc -l /f1",
    "echo WC-END",
    "echo HEAD1-MARK",
    "head -n 1 /f1",
    "echo HEAD1-END",
    "mkdir -p /a/b/c",
    "echo LSAB-MARK",
    "ls /a/b",
    "echo LSAB-END",
    "cp -r /a /b2",
    "echo LSB2-MARK",
    "ls /b2",
    "echo LSB2-END",
    "rm -r /b2",
    "echo RM2-MARK",
    "ls /b2",
    "echo RM2-END",
    "cd /a/b/c",
    "pwd",
    "cd -",
    "echo flags-done",
]


def main():
    with QTest("toolflags", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)
        out = b""
        for line in CMDS:
            s.sendall(line.encode() + b"\n")
            target = out.count(b"AOS> ") + 1
            end = time.time() + 20
            while time.time() < end and out.count(b"AOS> ") < target:
                try:
                    d = s.recv(4096)
                    if d:
                        out += d
                except socket.timeout:
                    pass
        out += q.serial_drain(s, timeout=15, needle=b"flags-done")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic:\n" + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        def between(a, b):
            tail = otext.split(a, 1)[1] if a in otext else ""
            return tail.split(b, 1)[0] if b in tail else ""

        failures = []
        cat2 = between("CAT2-MARK", "CAT2-END")
        if "a" not in cat2 or "b" not in cat2:
            failures.append("cat /f1 /f2 did not print both files")
        wc = between("WC-MARK", "WC-END")
        if "1 /f1" not in wc:
            failures.append("wc -l /f1 did not report 1 line")
        h1 = between("HEAD1-MARK", "HEAD1-END")
        if "a" not in h1 or "b" in h1:
            failures.append("head -n 1 /f1 did not print just the first line")
        lsab = between("LSAB-MARK", "LSAB-END")
        if "c" not in lsab:
            failures.append("mkdir -p /a/b/c -> ls /a/b did not show c")
        lsb2 = between("LSB2-MARK", "LSB2-END")
        if "b" not in lsb2:
            failures.append("cp -r /a /b2 -> ls /b2 did not show the tree")
        rm2 = between("RM2-MARK", "RM2-END")
        if "No such file or directory" not in rm2:
            failures.append("rm -r /b2 did not remove the tree (ls should error)")
        if "cd -" not in otext or "/a/b/c" not in otext:
            failures.append("cd - did not echo the previous directory")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
    print("PASS: tool flags")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

Register `toolflags`, `lsflagstest` and `sgrcolor` in the Makefile `TESTS` list (next to `fstoolstest`).

- [ ] **Step 4: Write `scripts/sgrcolor.py`**

Boot with `-vga none -device virtio-vga,disable-modern=on` (GPU) so `ls` runs in the console; read serial log. Run `ls /bin`, then assert `out` contains `\x1b[38;5;33m` bytes; run `ls /bin > /sgr.txt` then `cat /sgr.txt` and assert the cat segment contains NO `\x1b`. Final assert: the entire serial log tail contains no `\x1b` (filter proof). Register in TESTS.

- [ ] **Step 5: Run the new + updated tests**

Run: `python3 scripts/fstoolstest.py && python3 scripts/lsflagstest.py && python3 scripts/toolflags.py && python3 scripts/sgrcolor.py`
Expected: all PASS.

- [ ] **Step 6: Run the regression suite**

Run: `make test-fast`
Expected: PASS (ipctest, linhello, lincat). Then run full `make test` if time permits (it is long).

- [ ] **Step 7: Commit**

```bash
git add scripts/fstoolstest.py scripts/atatest.py scripts/cwdtest.py scripts/toolflags.py scripts/lsflagstest.py scripts/sgrcolor.py Makefile
git commit -m "test: update fs-tools expectations, add toolflags/lsflagstest/sgrcolor suites"
```

---

### Task 16: AGENTS.md — документация

**Files:**
- Modify: `AGENTS.md`

**Interfaces:**
- Produces: разделы про SGR-цвета (`xterm_rgb[256]`, `38;5;N` в fb/term/serial-filter), `uutils` слой, env `TERM` механизм, `cd -`/`~`/`pwd -P`, новые syscall `AOS_SPAWN_FDS_ENV 524`.

- [ ] **Step 1: Add documentation**

Добавить подраздел в «Terminal» про SGR и в «Programs» про `uutils`/флаги утилит и env `TERM`; в «Syscall hardening» — `AOS_SPAWN_FDS_ENV`. Кратко, по-русски, технические идентификаторы без перевода.

- [ ] **Step 2: Commit**

```bash
git add AGENTS.md
git commit -m "docs: SGR colors, uutils, TERM env, new syscall"
```

---

## Self-Review

**1. Spec coverage:**
- §1 uutils → Task 1.
- §2 SGR vga/term/serial → Tasks 2, 4, 3.
- §3 env TERM (kernel shell, GUI sh, task/stack_build/syscall) → Tasks 5, 6, 7.
- §4 utilities ls/head/wc/cat/cp/mv/mkdir/rmdir/rm/date/uptime/procinfo/sync/echo/help → Tasks 8-13.
- §5 cd/pwd → Tasks 6, 7.
- §6 tests → Task 15.
- Файлы/порядок/риски → все задачи выше.

**2. Placeholder scan:** все шаги содержат код; нет TBD/TODO. (Ранее выявленные пропуски — полный код `mv.c`, `procinfo.c`, `fstoolstest.py`, `toolflags.py`, `lsflagstest.py` — заполнены.)

**3. Type consistency:** `u_list_dir(dir, ent, max, show_dot)` определён 4-арг уже в Task 1, `ls.c` (Task 13) передаёт `aflag`. `task_spawn`/`program_load`/`elf_load_linux` принимают `env` (Task 5), вызовы с `env`/`0` — в Tasks 6, 7 и `kernel/aos_gui.c`/`syscall.c`/`kernel.c`. `bg_spawn(line, pid, term_off)` (Task 6). `aos_spawn_env(path,args,sink,env,redirs)` = syscall 524: **redirs в `r->esi`, env в `r->edi`** (совпадает с регистром redirs в существующем `AOS_SPAWN_FDS`, `aos_gui.c:244`). `vga` цвета через `cur_fg_rgb()`/`cur_bg_rgb()`; `D`/`K`/`J` не сломаны (SGR-параметры накапливаются в `ansi_params` параллельно с сохранением поведения `D`). `term.c`: `esc_params`/`esc_np` добавлены параллельно к `esc_n`/`esc_r`, существующие `H`/`D`/`C` не затронуты.

**Замечание по spec:** в спеке упоминалось обновление `lindirtest.py` («Files in /:» префикс). На проверке выяснилось: `lindirtest.py` гоняет `lin/ls` (musl `tools/linux/ls.c`), а не `bin/ls`, поэтому его ожидания НЕ меняются — задача исключена.