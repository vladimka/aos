# Boot Screen, Gradient Wallpaper, System Config Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Print an ASCII "AOS" logo at boot, make the WM desktop a vertical gradient from configurable top/bottom colors, and add a user-editable `sys/config.cfg` (timezone + wallpaper colors) applied at boot.

**Architecture:** A new `kernel/config.c` reads/creates `sys/config.cfg` on the SFS right after `fs_init()` and applies the timezone to RTC reads via `rtc_set_tz()` (new epoch conversion in `drivers/rtc.c`). The WM reads the same file for the two wallpaper colors and fills the desktop with a per-row interpolated gradient instead of a flat fill. A two-boot `scripts/configtest.py` regression asserts the logo, config creation/loading, gradient pixels, and the applied timezone.

**Tech Stack:** Freestanding C11, x86 i386, QEMU headless + monitor socket + PPM screendumps, Python 3 test harness.

**Spec:** `docs/superpowers/specs/2026-08-05-boot-screen-wallpaper-config-design.md`

## Global Constraints

- Build with `make`; verification is `make test` plus boot-time serial output.
- `sys/config.cfg` is a flat SFS name (the `/` is part of the 13-char string, like `lin/test.txt`); name limit is 27 chars.
- Config keys parsed: `timezone` (signed minutes), `wallpaper_top`, `wallpaper_bot` (`0xRRGGBB`). Unknown keys, blank lines, `#` comments ignored. Invalid/missing values fall back to defaults: timezone 0, top `0x1A2030`, bottom `0x0E1620`.
- `COL_DESKTOP 0x1A2030` stays the default gradient **top** color so desktop pixels at `y=0` remain `(26,32,48)` and existing pixel tests keep passing.
- The kernel's `printf()` writes to both VGA and COM1, so banners land in the serial log.
- All kernel code compiled `-ffreestanding -nostdlib`; programs use the ring-3 libaos API (no kernel headers).
- No commits until each task's build + relevant check passes.

---

### Task 1: Boot screen ASCII logo

**Files:**
- Modify: `kernel/kernel.c` (add the logo right after `vga_init()`)
- Test: boot serial log (wired into `configtest.py` in Task 5)

**Interfaces:**
- Consumes: `vga_init()` (already called); `printf()` from `kernel/printf.h` (already included).
- Produces: a 5-line ASCII "AOS" block in the VGA output and COM1 serial log, before the existing banners.

- [ ] **Step 1: Add the logo to `kernel_main`**

In `kernel/kernel.c`, replace lines 80-82:

```c
    serial_init();
    vga_init();

    printf("=== AOS Kernel v0.3 ===\n");
```

with:

```c
    serial_init();
    vga_init();

    // Boot logo (text phase, before the WM takes over the framebuffer).
    // One printf call with a string literal; also lands in the COM1 log.
    printf("\n"
           "  AAA    OOO    SSS \n"
           " A   A  O   O  S    \n"
           " AAAAA  O   O   SSS \n"
           " A   A  O   O      S\n"
           " A   A   OOO    SSS \n");
    printf("=== AOS Kernel v0.3 ===\n");
```

- [ ] **Step 2: Build**

Run: `make`
Expected: compiles clean; `aos.iso` rebuilt.

- [ ] **Step 3: Boot smoke test**

Run: `make` then:
```bash
timeout 30 qemu-system-i386 -m 256 -cdrom aos.iso -display none -serial file:/tmp/aos-logo.log &
sleep 12
cat /tmp/aos-logo.log
```
Expected: the serial log starts with a blank line, the five logo lines (`  AAA    OOO    SSS ` … ` A   A   OOO    SSS `), then `=== AOS Kernel v0.3 ===`. Kill the QEMU process afterwards.

- [ ] **Step 4: Commit**

```bash
git add kernel/kernel.c
git commit -m "kernel: ASCII AOS boot logo"
```

---

### Task 2: RTC timezone offset

**Files:**
- Modify: `drivers/rtc.h` (declare `rtc_set_tz`)
- Modify: `drivers/rtc.c` (tz state, epoch conversion, apply offset in `rtc_get`)
- Test: `configtest.py` (Task 5) asserts the serial-log line; compile check here.

**Interfaces:**
- Consumes: existing `rtc_get` fields; nothing new.
- Produces: `void rtc_set_tz(int minutes)` — sets a module-level offset; `rtc_get()` now returns local wall time (CMOS + offset). Every `SYS_RTC` consumer (`clock`, `date`) gets local time automatically.

- [ ] **Step 1: Declare `rtc_set_tz` in `drivers/rtc.h`**

Add after `int rtc_get(struct aos_time *t);`:

```c
void rtc_set_tz(int minutes);
```

- [ ] **Step 2: Add tz state and epoch conversion to `drivers/rtc.c`**

After the `#include` lines add:

```c
static int tz_min;

void rtc_set_tz(int minutes) { tz_min = minutes; }

// Howard Hinnant's days-from-civil / civil-from-days (proleptic Gregorian).
static int days_from_civil(int y, unsigned int m, unsigned int d) {
    y -= (int)(m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned int yoe = (unsigned int)(y - era * 400);
    unsigned int doy = (153 * (m + (m > 2 ? 0u : 9u)) + 2) / 5 + d - 1;
    unsigned int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static void civil_from_days(int z, int *y, unsigned int *m, unsigned int *d) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned int doe = (unsigned int)(z - era * 146097);
    unsigned int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = (int)yoe + era * 400;
    unsigned int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3u : 9u - 12u);
    if (*m <= 2) *y += 1;
}
```

Note: `m > 2 ? 0u : 9u` and `mp < 10 ? 3u : 9u - 12u` keep the arithmetic unsigned-clean; `(9u - 12u)` wraps to a large unsigned value intended to be subtracted, so write it as `mp + (mp < 10 ? 3u : -9)` using a signed literal instead:

```c
    *m = mp + (mp < 10 ? 3u : (unsigned int)-9);
```

(For readability use exactly that last form.)

- [ ] **Step 3: Apply the offset in `rtc_get`**

At the end of `rtc_get`, right before `return 0;`, add:

```c
    if (tz_min != 0) {
        long total = (long)days_from_civil(year, (unsigned int)mon,
                                           (unsigned int)day) * 86400L +
                     (long)hr * 3600L + (long)min * 60L + (long)sec + tz_min;
        if (total < 0) total = 0;
        int days = (int)(total / 86400L);
        int rem = (int)(total % 86400L);
        hr = (unsigned char)(rem / 3600);
        min = (unsigned char)((rem % 3600) / 60);
        sec = (unsigned char)(rem % 60);
        unsigned int mo, da;
        civil_from_days(days, &year, &mo, &da);
        month = (int)mo;
        day = (int)da;
    }
```

- [ ] **Step 4: Build**

Run: `make`
Expected: compiles clean (no warnings).

- [ ] **Step 5: Commit**

```bash
git add drivers/rtc.h drivers/rtc.c
git commit -m "rtc: timezone offset applied to rtc_get (TODO 1.5)"
```

---

### Task 3: `kernel/config.c` — system config

**Files:**
- Create: `kernel/config.h`
- Create: `kernel/config.c`
- Modify: `Makefile` (add `kernel/config.o` to `KERNEL_OBJS`)
- Modify: `kernel/kernel.c` (include `config.h`, call `config_load()` after `fs_init()`)
- Test: boot serial log (`config: created`) wired into `configtest.py` (Task 5).

**Interfaces:**
- Consumes: `fs_exists`/`fs_write`/`fs_read` from `kernel/fs.h`; `strncmp` from `kernel/string.h`; `kmalloc`/`kfree` from `kernel/kmm.h`; `printf` from `kernel/printf.h`; `rtc_set_tz` from `drivers/rtc.h`.
- Produces:
  - `void config_load(void)` — creates `sys/config.cfg` with defaults if absent, parses it, calls `rtc_set_tz()`, prints `config: created|loaded` and `config: timezone <N>` banners.
  - `int config_tz_min(void)`
  - `unsigned int config_wallpaper_top(void)`
  - `unsigned int config_wallpaper_bot(void)`

- [ ] **Step 1: Write `kernel/config.h`**

```c
#ifndef CONFIG_H
#define CONFIG_H

void config_load(void);
int config_tz_min(void);
unsigned int config_wallpaper_top(void);
unsigned int config_wallpaper_bot(void);

#endif
```

- [ ] **Step 2: Write `kernel/config.c`**

```c
#include "config.h"
#include "fs.h"
#include "rtc.h"
#include "printf.h"
#include "string.h"
#include "kmm.h"

#define CONFIG_PATH "sys/config.cfg"

#define DEFAULT_TZ   0
#define DEFAULT_TOP  0x1A2030
#define DEFAULT_BOT  0x0E1620

static int tz_min;
static unsigned int wp_top;
static unsigned int wp_bot;

static int parse_int(const char *s) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static unsigned int parse_hex(const char *s) {
    unsigned int v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;) {
        char c = *s++;
        unsigned int d;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
    }
    return v;
}

static void apply_line(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0 || *line == '#') return;
    const char *eq = line;
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') return;
    int klen = 0;
    while (line[klen] != '=') klen++;
    while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t')) klen--;
    const char *val = eq + 1;
    while (*val == ' ' || *val == '\t') val++;
    if (klen == 8 && strncmp(line, "timezone", 8) == 0)
        tz_min = parse_int(val);
    else if (klen == 13 && strncmp(line, "wallpaper_top", 13) == 0)
        wp_top = parse_hex(val);
    else if (klen == 13 && strncmp(line, "wallpaper_bot", 13) == 0)
        wp_bot = parse_hex(val);
}

void config_load(void) {
    tz_min = DEFAULT_TZ;
    wp_top = DEFAULT_TOP;
    wp_bot = DEFAULT_BOT;

    if (!fs_exists(CONFIG_PATH)) {
        static const char def[] =
            "# AOS system config\n"
            "timezone=0\n"
            "wallpaper_top=0x1A2030\n"
            "wallpaper_bot=0x0E1620\n";
        if (fs_write(CONFIG_PATH, def, sizeof(def) - 1) >= 0)
            printf("config: created %s\n", CONFIG_PATH);
        else
            printf("config: create %s failed\n", CONFIG_PATH);
    } else {
        printf("config: loaded %s\n", CONFIG_PATH);
    }

    char *buf = kmalloc(512);
    if (buf) {
        int sz = fs_read(CONFIG_PATH, buf, 511);
        if (sz > 0) {
            buf[sz] = 0;
            char *p = buf;
            while (p && *p) {
                char *eol = p;
                while (*eol && *eol != '\n') eol++;
                if (eol > p && eol[-1] == '\r') eol[-1] = 0;
                char saved = *eol;
                *eol = 0;
                apply_line(p);
                *eol = saved;
                if (saved == '\n') p = eol + 1;
                else break;
            }
        }
        kfree(buf);
    }

    rtc_set_tz(tz_min);
    if (tz_min != 0)
        printf("config: timezone %d\n", tz_min);
}

int config_tz_min(void) { return tz_min; }
unsigned int config_wallpaper_top(void) { return wp_top; }
unsigned int config_wallpaper_bot(void) { return wp_bot; }
```

- [ ] **Step 3: Add `kernel/config.o` to the Makefile**

In `KERNEL_OBJS` add `kernel/config.o` after `kernel/progload.o`.

- [ ] **Step 4: Wire `config_load()` into `kernel_main`**

In `kernel/kernel.c`, add `#include "config.h"` after `#include "progload.h"` (line 15), and replace:

```c
    fs_init();
    printf("Filesystem ready.\n");

    load_embedded_programs();
    load_embedded_data();
```

with:

```c
    fs_init();
    printf("Filesystem ready.\n");

    config_load();

    load_embedded_programs();
    load_embedded_data();
```

- [ ] **Step 5: Build**

Run: `make`
Expected: compiles clean; serial log on boot shows `config: created sys/config.cfg` after `Filesystem ready.` (check with the Task 1 smoke-test command).

- [ ] **Step 6: Commit**

```bash
git add kernel/config.c kernel/config.h Makefile kernel/kernel.c
git commit -m "config: sys/config.cfg parsing, defaults, timezone apply"
```

---

### Task 4: WM gradient wallpaper + config read + hide `sys/` icons

**Files:**
- Modify: `programs/wm.c`
- Test: `configtest.py` (Task 5) asserts the gradient top pixel and the absence of a `sys/` desktop icon.

**Interfaces:**
- Consumes: `fs_read` from libaos; existing `composite_rect`, `refresh_files`, `fb_addr`/`fb_w`/`fb_h`/`fb_pitch` globals.
- Produces: `draw_desktop_gradient(x0,y0,x1,y1)` filling the desktop with a vertical gradient; desktop fill in `composite_rect` uses it; `refresh_files()` skips any name starting `sys/`.

- [ ] **Step 1: Add wallpaper color globals and a small `strequal` helper**

In `programs/wm.c`, after the `COL_DESKTOP` define (line 9) add:

```c
// Desktop gradient colors; overridden from sys/config.cfg if present.
static unsigned int wp_top = COL_DESKTOP;
static unsigned int wp_bot = 0x0E1620;
```

In the "small helpers" section (after `int2str`, ~line 309) add:

```c
static int strequal(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
```

- [ ] **Step 2: Add config parsing + gradient fill functions**

After `fb_fill` (~line 323) add:

```c
static unsigned int parse_hex_cfg(const char *s) {
    unsigned int v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s;
        unsigned int d;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
        s++;
    }
    return v;
}

static void load_wallpaper_config(void) {
    char buf[512];
    int sz = fs_read("sys/config.cfg", buf, sizeof(buf) - 1);
    if (sz <= 0) return;
    buf[sz] = 0;
    char *p = buf;
    while (p && *p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char line[80];
        int n = (int)(eol - p);
        if (n > 79) n = 79;
        for (int i = 0; i < n; i++) line[i] = p[i];
        line[n] = 0;
        if (n > 0 && line[n - 1] == '\r') line[n - 1] = 0;
        int k = 0;
        while (line[k] == ' ' || line[k] == '\t') k++;
        if (line[k] != '#' && line[k] != 0) {
            char *eq = line;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') {
                *eq = 0;
                const char *val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                if (strequal(line + k, "wallpaper_top")) wp_top = parse_hex_cfg(val);
                else if (strequal(line + k, "wallpaper_bot")) wp_bot = parse_hex_cfg(val);
            }
        }
        if (*eol == '\n') p = eol + 1;
        else break;
    }
}

// Vertical gradient from wp_top to wp_bot over the full screen height.
// One u32 fill per row; per-row delta is accumulated (no per-pixel division).
// Partial damage rects are handled by seeding the fixed-point value at the
// rect's absolute y0, so MSG_UPDATE regions keep the correct colors.
static void draw_desktop_gradient(int x0, int y0, int x1, int y1) {
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    unsigned int *fb = (unsigned int *)fb_addr;
    unsigned int pitch = fb_pitch >> 2;
    int fbh = (int)fb_h;
    if (fbh <= 0) fbh = h;
    int rt = (int)((wp_top >> 16) & 0xFF), gt = (int)((wp_top >> 8) & 0xFF),
        bt = (int)(wp_top & 0xFF);
    int rb = (int)((wp_bot >> 16) & 0xFF), gb = (int)((wp_bot >> 8) & 0xFF),
        bb = (int)(wp_bot & 0xFF);
    int dr = (((rb - rt) << 16) / fbh);
    int dg = (((gb - gt) << 16) / fbh);
    int db = (((bb - bt) << 16) / fbh);
    int cr = (rt << 16) + dr * y0;
    int cg = (gt << 16) + dg * y0;
    int cb = (bt << 16) + db * y0;
    for (int y = 0; y < h; y++) {
        unsigned int rgb = ((unsigned int)((cr >> 16) & 0xFF) << 16) |
                           ((unsigned int)((cg >> 16) & 0xFF) << 8) |
                           ((unsigned int)((cb >> 16) & 0xFF));
        unsigned int *row = fb + (unsigned)(y0 + y) * pitch + (unsigned)x0;
        for (int x = 0; x < w; x++) row[x] = rgb;
        cr += dr; cg += dg; cb += db;
    }
}
```

- [ ] **Step 3: Use the gradient in `composite_rect`**

In `composite_rect` (wm.c:398) replace:

```c
    fb_fill(x0, y0, x1 - x0, y1 - y0, COL_DESKTOP);
```

with:

```c
    draw_desktop_gradient(x0, y0, x1, y1);
```

- [ ] **Step 4: Load the config in `main()`**

In `main()`, right after `register_events();` (line 907) add:

```c
    load_wallpaper_config();
```

- [ ] **Step 5: Hide `sys/` files from the desktop icon grid**

In `refresh_files` (wm.c:619-622) add after the `lin/` skip:

```c
        if (nm[0] == 's' && nm[1] == 'y' && nm[2] == 's' && nm[3] == '/')
            continue;
```

- [ ] **Step 6: Build**

Run: `make`
Expected: compiles clean.

- [ ] **Step 7: Manual boot check**

Boot headless (Task 1 smoke command), open a term from the dock, run `cat sys/config.cfg`; a PPM screendump (Task 5 wires assertions) should show the config text. The desktop should not show a `sys/` icon.

- [ ] **Step 8: Commit**

```bash
git add programs/wm.c
git commit -m "wm: gradient wallpaper from sys/config.cfg, hide sys/ icons"
```

---

### Task 5: `scripts/configtest.py` regression

**Files:**
- Create: `scripts/configtest.py`
- Modify: `Makefile` (add `configtest` to `TESTS`)
- Test: the harness itself.

**Interfaces:**
- Consumes: `aos.iso`, QEMU monitor + serial, the WM gradient, `bin/cat`, the disk-mount path (`virtio-blk`).
- Produces: a two-boot headless test. Boot A (no disk): asserts the logo and `config: created` in the serial log, the gradient top pixel `(26,32,48)` at `(700,2)`, no `sys/` icon at grid slot 1, and `cat sys/config.cfg` rendering in a term. Boot B (disk-seeded `timezone=+180` SFS): asserts `config: loaded` and `config: timezone +180`.

- [ ] **Step 1: Write `scripts/configtest.py`**

```python
#!/usr/bin/env python3
"""Config / boot-logo / gradient-wallpaper regression for AOS.

Boot A (no disk, default ramdisk):
  1. serial log has the ASCII AOS logo and `config: created sys/config.cfg`,
  2. desktop gradient top pixel (700,2) is still (26,32,48) = 0x1A2030,
  3. no `sys/` icon is shown on the desktop (grid slot 1 is empty),
  4. a term spawned from the dock renders `cat sys/config.cfg`.

Boot B (disk image with a host-built SFS containing timezone=+180):
  5. serial log has `config: loaded sys/config.cfg` and `config: timezone +180`.
"""
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-config.sock"
SER = "/tmp/aos-config.log"
BEFORE = "/tmp/aos-config-before.ppm"
PPM = "/tmp/aos-config.ppm"
MOUSE_STATE = "/tmp/aos-config.state"
MOUSE_BOOT = (511, 383)
IMG = "/tmp/aos-config-disk.img"

DESKTOP = (26, 32, 48)
LOGO_LINE = "AAA    OOO    SSS"
CFG_CREATED = "config: created sys/config.cfg"
CFG_LOADED = "config: loaded sys/config.cfg"
CFG_TZ = "config: timezone +180"

TXT_X0, TXT_X1 = 21, 660          # term text band
TXT_Y0, TXT_Y1 = 39, 39 + 26 * 16
TXT_THRESHOLD = 300


def wait_for(path, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)


def hmp(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(3)
        s.connect(MON)
        data = b""
        while b"(qemu)" not in data:
            data += s.recv(4096)
        s.sendall(command.encode() + b"\n")
        data = b""
        while b"(qemu)" not in data:
            data += s.recv(4096)
        return data.decode(errors="replace")


def read_state():
    try:
        with open(MOUSE_STATE) as f:
            x, y = f.read().split()
            return int(x), int(y)
    except Exception:
        return MOUSE_BOOT


def write_state(x, y):
    with open(MOUSE_STATE, "w") as f:
        f.write("%d %d\n" % (x, y))


def move_abs(x, y):
    cx, cy = read_state()
    dx, dy = x - cx, y - cy
    step = 100
    while dx or dy:
        sx = max(-step, min(step, dx))
        sy = max(-step, min(step, dy))
        hmp("mouse_move %d %d" % (sx, sy))
        time.sleep(0.02)
        dx -= sx
        dy -= sy
    write_state(x, y)


def click(x, y):
    move_abs(x, y)
    time.sleep(0.3)
    hmp("mouse_button 1")
    time.sleep(0.4)
    hmp("mouse_button 0")


def send_text(text):
    keys = {"\n": "ret", " ": "spc"}
    for ch in text:
        key = keys.get(ch, ch)
        hmp("sendkey " + key)
        time.sleep(0.04)


def snap(name):
    hmp("screendump " + name)
    wait_for(name)


def ppm_data(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise AssertionError("not a P6 PPM: " + path)
        dim = f.readline().split()
        f.readline()                       # maxval
        return int(dim[0]), int(dim[1]), f.read()


def pixel(path, x, y):
    w, _, data = ppm_data(path)
    off = (y * w + x) * 3
    return tuple(data[off:off + 3])


def count_bright(path, x0, y0, x1, y1):
    w, _, data = ppm_data(path)
    n = 0
    for y in range(y0, y1 + 1):
        row = data[(y * w + x0) * 3:(y * w + x1 + 1) * 3]
        for i in range(0, len(row), 3):
            if row[i] >= 0xC0 and row[i + 1] >= 0xC0 and row[i + 2] >= 0xC0:
                n += 1
    return n


def serial_text():
    try:
        with open(SER, "r", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def wait_for_serial(text, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        if text in serial_text():
            return True
        time.sleep(0.2)
    return False


def assert_pixel(path, x, y, want, what):
    got = pixel(path, x, y)
    if got != want:
        raise AssertionError(
            "%s: pixel(%d,%d)=%s want %s (%s)" % (path, x, y, got, want, what))
    print("  ok: %s at (%d,%d)=%s" % (what, x, y, got))


def build_sfs(entries):
    """Build a 1 MB SFS image matching kernel/sfs.h layout exactly.

    header: magic[4] + total_size + entry_count = 12 bytes
    entry:  name[28] + size + offset + flags + pad[3] = 40 bytes
    data starts at 12 + 64 * 40 = 2572.
    """
    FS_SIZE = 1024 * 1024
    MAX_FILES = 64
    data_start = 12 + MAX_FILES * 40
    out = bytearray(FS_SIZE)
    struct.pack_into("<4sII", out, 0, b"SFS1", FS_SIZE - data_start, len(entries))
    off = data_start
    for i, (name, data) in enumerate(entries):
        e = 12 + i * 40
        struct.pack_into("<28sII", out, e, name.encode(), len(data), off)
        out[e + 36] = 1                    # flags = used
        out[off:off + len(data)] = data
        off += len(data)
    return out


def terminate(qemu):
    qemu.terminate()
    try:
        qemu.wait(timeout=5)
    except subprocess.TimeoutExpired:
        qemu.kill()


def boot_qemu(disk):
    cmd = [
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ]
    if disk:
        cmd += [
            "-drive", "file=" + IMG + ",format=raw,if=none,id=d0",
            "-device", "virtio-blk-pci,disable-modern=on,drive=d0",
        ]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def main():
    # ---- Boot A: default ramdisk ----
    for p in (MON, SER, BEFORE, PPM, MOUSE_STATE):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
    write_state(*MOUSE_BOOT)
    qemu = boot_qemu(disk=False)
    try:
        wait_for(MON)
        time.sleep(6)
        log = serial_text()
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot")
        if LOGO_LINE not in log:
            raise AssertionError("boot logo missing from serial log; tail:\n"
                                 + log[-300:])
        if not wait_for_serial(CFG_CREATED):
            raise AssertionError("config file was not created on first boot")
        print("  ok: boot logo + config: created")

        # Grid slot 1 would hold sys/config.cfg if not hidden; slot 0 is demo.ico.
        snap(BEFORE)
        if count_bright(BEFORE, 68, 24, 99, 55) != 0:
            raise AssertionError("sys/config.cfg shown as a desktop icon")
        assert_pixel(BEFORE, 700, 2, DESKTOP, "gradient top == 0x1A2030")
        print("  ok: no sys/ icon; gradient top color")

        click(472, 724)                    # dock term launcher
        time.sleep(1)
        hmp("screendump " + BEFORE)
        wait_for(BEFORE)
        before = count_bright(BEFORE, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        send_text("cat sys/config.cfg\n")
        time.sleep(1)
        snap(PPM)
        after = count_bright(PPM, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        if after - before <= TXT_THRESHOLD:
            raise AssertionError(
                "cat sys/config.cfg did not render (band grew %d)"
                % (after - before))
        print("  ok: cat sys/config.cfg rendered")
    finally:
        terminate(qemu)

    # ---- Boot B: disk-seeded timezone +180 ----
    try:
        os.unlink(SER)
    except FileNotFoundError:
        pass
    with open(IMG, "wb") as f:
        f.write(build_sfs([("sys/config.cfg", b"timezone=+180\n")]))
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)
    qemu = boot_qemu(disk=True)
    try:
        wait_for(MON)
        if not wait_for_serial(CFG_LOADED):
            raise AssertionError("config: loaded banner missing; log:\n"
                                 + serial_text()[-300:])
        if not wait_for_serial(CFG_TZ):
            raise AssertionError("config: timezone +180 not applied; log:\n"
                                 + serial_text()[-300:])
        if "KERNEL PANIC" in serial_text():
            raise AssertionError("kernel panic during config boot")
        print("  ok: disk-seeded timezone +180 applied")
    finally:
        terminate(qemu)

    print("PASS: boot logo, config, gradient wallpaper")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Add `configtest` to `TESTS` in the Makefile**

Change:

```make
TESTS = ipctest manytest notepadtest sleeptest rngtest blktest virtiotest netlooptest rtctest $(LINUX_TESTS)
```

to:

```make
TESTS = ipctest manytest notepadtest sleeptest rngtest blktest virtiotest netlooptest rtctest configtest $(LINUX_TESTS)
```

- [ ] **Step 3: Run the new test**

Run: `python3 scripts/configtest.py`
Expected: prints `  ok: boot logo + config: created`, `  ok: no sys/ icon; gradient top color`, `  ok: cat sys/config.cfg rendered`, `  ok: disk-seeded timezone +180 applied`, then `PASS: boot logo, config, gradient wallpaper`.

- [ ] **Step 4: Commit**

```bash
git add scripts/configtest.py Makefile
git commit -m "test: configtest regression (boot logo, config, gradient)"
```

---

### Task 6: Fix pre-existing `rtctest.py` serial assertion

`rtctest.py` asserts a date string appears in the **serial** log, but user-program output routes to the term framebuffer (never serial), so it can never pass; the pixel check right after it is the real verification.

**Files:**
- Modify: `scripts/rtctest.py`

- [ ] **Step 1: Drop the bogus serial check**

Replace the block (rtctest.py:99-104):

```python
        if "Unknown command" in log or "cannot run command" in log:
            raise AssertionError("date did not launch: %r"
                                 % log.strip().splitlines()[-1])
        if not DATE_RE.search(log):
            raise AssertionError("date did not print wall-clock time; log tail:\n"
                                 + log[-500:])
```

with:

```python
        if "Unknown command" in log or "cannot run command" in log:
            raise AssertionError("date did not launch: %r"
                                 % log.strip().splitlines()[-1])
```

(the pixel band check below already verifies the date output rendered).

- [ ] **Step 2: Run the test**

Run: `python3 scripts/rtctest.py`
Expected: `PASS: RTC wall-clock date command` (relies on the term pixel band growth).

- [ ] **Step 3: Commit**

```bash
git add scripts/rtctest.py
git commit -m "test: rtctest asserts date output via pixels, not serial"
```

---

### Task 7: Full regression + docs

**Files:**
- Modify: `TODO.md`
- Modify: `AGENTS.md`

- [ ] **Step 1: Run the full regression suite**

Run: `make test`
Expected: all listed tests pass. (manytest flakiness is pre-existing; re-run if it flakes.)

- [ ] **Step 2: Update `TODO.md`**

- Under 2.2 WM, replace the `Обои/фон рабочего стола` item with a checked note that the gradient + `sys/config.cfg` colors are done and only real image-file wallpapers remain.
- Under 1.5, extend the RTC item's "готово" note with `rtc_set_tz` + timezone from config.

- [ ] **Step 3: Update `AGENTS.md`**

Add a short paragraph in the WM/`vga`/programs sections documenting `sys/config.cfg`, the boot logo, and the gradient (see spec for wording; keep it terse like the existing notes).

- [ ] **Step 4: Commit**

```bash
git add TODO.md AGENTS.md
git commit -m "docs: boot logo, config, gradient wallpaper"
```

---

## Self-review notes

- **Spec coverage:** Feature 4 logo → Task 1; Feature 2 timezone → Task 2; Feature 1 config → Task 3; Feature 3 gradient + `sys/` skip + WM config read → Task 4; testing → Tasks 5–6; regression + docs → Task 7. All spec files touched are covered (new `kernel/config.c`/`.h`, `drivers/rtc.c`, `kernel/kernel.c`, `programs/wm.c`, `Makefile`, `scripts/`).
- **Placeholder scan:** every step has concrete code or a concrete command; no TODOs.
- **Type consistency:** `config_tz_min()/config_wallpaper_top()/config_wallpaper_bot()` defined in Task 3 and consumed by nothing else yet (kernel-side, so the WM re-parses independently per spec). `rtc_set_tz(int)` declared in `drivers/rtc.h` (Task 2) and called from `config_load()` (Task 3). `draw_desktop_gradient(x0,y0,x1,y1)` used only inside `composite_rect` (Task 4).
- **Deliberate deviation from spec:** `config_load()` is called after `fs_init()` per spec, but before `load_embedded_programs()`; the timezone test seeds the config through a **disk-attached SFS** (`sfs_set_disk` is wired in `virtio_init`), not embedded data, so no ordering change was needed and the spec's ordering holds.
- **Gradient correctness for damage rects:** fixed-point delta accumulation is seeded at the rect's absolute `y0`, so `MSG_UPDATE` partial rects keep correct absolute colors (spec's "no per-pixel division" preserved — one division per color per fill).
- **Existing tests unaffected:** only `ipctest.py:100` compares against `(26,32,48)` and only as a negative check (must NOT equal); with the gradient a bare-desktop pixel at `(30,30)` is `(25,31,47)`, so the check still passes (it loses failure-detection power there, but the band-growth check below it remains the real test).
- **Pre-existing issue fixed:** `rtctest.py` asserted date output in the serial log, which can never happen (output routes to the term framebuffer); fixed in Task 6.
