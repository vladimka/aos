# Design: boot screen ASCII logo, gradient wallpaper, system config

Date: 2026-08-05
Status: approved (design review complete)

## Goal

Three related features for AOS:

1. **Boot screen** — a monochrome ASCII-art "AOS" logo printed by the kernel
   during early boot (text phase, before the WM takes over the screen).
2. **Wallpaper** — the WM desktop background becomes a procedural vertical
   gradient (top color → bottom color) instead of a flat fill.
3. **System config** — a user-editable `sys/config.cfg` on the SFS ramdisk
   holding `timezone` (UTC offset in minutes), `wallpaper_top`, and
   `wallpaper_bot`. Created with defaults on first boot, applied by kernel
   (timezone) and WM (wallpaper) at startup.

virtio-gpu is explicitly out of scope (postponed).

## Context (verified in repo)

- `kernel_main` (`kernel/kernel.c`) prints plain-text banners to VGA + COM1
  (`printf("=== AOS Kernel v0.3 ===\n")` and per-subsystem lines) before
  spawning `bin/wm`, which takes over the framebuffer.
- `drivers/rtc.c` `rtc_get()` reads CMOS wall-clock (BCD/bin, UIP wait) and
  returns it raw through `SYS_RTC` (34). `clock`/`date` programs read it via
  `get_rtc()` (`programs/libaos.c:72`).
- `programs/wm.c` fills the desktop with flat `COL_DESKTOP 0x1A2030`
  (`#define COL_DESKTOP 0x1A2030`, used at wm.c:398 in `composite_rect` and in
  the dock corners). Icon grid and dock draw on top.
- SFS ramdisk holds programs + embedded data (e.g. `scripts/demo.ico`) written
  by `load_embedded_data()` on first boot. `gen_progs.py --data` embeds
  build-time files into the ISO.
- Pixel tests (`guitester.py`, `notepadtest.py`) assert desktop color
  `(26,32,48)` = `0x1A2030` at desktop pixels.

## Feature 1: system config (`kernel/config.c`)

### File format

`sys/config.cfg` on the SFS ramdisk, text, `key=value` lines, `#` comments:

```
# AOS system config
timezone=+180        # UTC offset in minutes (+3h = +180, -2h30 = -150)
wallpaper_top=0x1A2030
wallpaper_bot=0x0E1620
```

- Keys parsed: `timezone`, `wallpaper_top`, `wallpaper_bot`. Unknown keys and
  comment lines are ignored.
- Numeric parsing: timezone is a signed integer (minutes), colors are
  `0xRRGGBB` hex. Invalid/missing values fall back to defaults.
- Timezone expressed in **minutes**, not hours, so half-hour offsets
  (India +330, Nepal +345) work.

### Creation on first boot

- New `kernel/config.c` / `kernel/config.h`.
- `config_load()`:
  1. If `sys/config.cfg` does not exist, create it with default content
     (timezone=0, wallpaper_top=0x1A2030, wallpaper_bot=0x0E1620) via the
     existing SFS write API.
  2. Read the file, parse `key=value` lines, cache values in static globals.
  3. Called from `kernel_main` right after `fs_init()` and before
     `load_embedded_programs()`.
- Getters: `int config_tz_min(void)`, `unsigned int config_wallpaper_top(void)`,
  `unsigned int config_wallpaper_bot(void)`.
- Defaults if the file is absent/unparseable: timezone 0, top 0x1A2030,
  bottom 0x0E1620.

## Feature 2: timezone (kernel-side)

- `drivers/rtc.c` gains a module-level `tz_min` (signed int, default 0) and a
  setter `rtc_set_tz(int minutes)`.
- `rtc_get()` applies the offset to the raw CMOS time before returning:
  - Convert Y/M/D/h/m → minutes-since-epoch using civil-from-days math
    (Howard Hinnant algorithm), add `tz_min`, convert back.
  - The returned `struct aos_time` is then local wall time.
- Every consumer of `SYS_RTC` (`clock`, `date`, future apps) automatically gets
  local time; no caller changes.
- `config_load()` calls `rtc_set_tz(config_tz_min())` at boot, so the offset is
  active before any user program runs.

## Feature 3: gradient wallpaper (WM)

- `programs/wm.c` `composite_rect` desktop fill (wm.c:398) becomes a vertical
  gradient from `wallpaper_top` to `wallpaper_bot` instead of flat
  `COL_DESKTOP`.
- Implementation: `draw_desktop_gradient(x0,y0,x1,y1)` — one u32 fill per pixel
  row; per-row color interpolated by fixed-point (top + (bot-top)*y/height),
  computed per row with integer math (delta = (bot-top)/height, add per row).
  No per-pixel division in the hot loop.
- Colors come from the config. The WM reads `sys/config.cfg` (SFS) at
  initialization and caches the two colors.
- SFS is a flat filesystem: `sys/config.cfg` is just a 27-char flat name
  (the `/` is part of the string, like `lin/test.txt`). The WM's
  `refresh_files()` (wm.c:619-622) already hides `bin/` and `lin/` prefixes
  from the desktop icon grid; extend it to also skip a `sys/` prefix so the
  config file does not appear as a `K_OTHER` desktop icon.
- `COL_DESKTOP` remains the default `wallpaper_top` so pixel tests asserting
  `(26,32,48)` at desktop pixels still pass on a fresh boot (top color ==
  old flat color).
- Dock border corners (wm.c:466-469), menu, and window borders keep solid
  colors. Icon labels draw on top of the gradient unchanged.
- Only the base desktop fill changes; `MSG_UPDATE`/dirty-rect paths reuse the
  same `composite_rect` fill, so damage rendering keeps working.

## Feature 4: boot screen ASCII logo (kernel)

- `kernel_main` prints a monochrome (current terminal text color) multi-line
  ASCII-art "AOS" logo right after `vga_init()`, before the existing banners.
- One `printf` call with a string literal constant; also lands in the COM1
  serial log (existing `printf` behavior).
- Version line (`v0.3`) printed below the logo as an ordinary banner.
- No ANSI color, no framebuffer drawing — pure text in the early boot phase.

## Out of scope

- virtio-gpu (postponed).
- Real image-file wallpapers (PPM/ICO decode for the background).
- Wallpaper hot-reload without reboot (config applied at boot/init only).

## Testing

- New/extended headless QEMU harness (e.g. `scripts/configtest.py`):
  1. Boot; assert the serial log contains the ASCII "AOS" logo block and the
     config-created banner (`config: created` / `config: loaded`).
  2. Assert `sys/config.cfg` exists on the ramdisk after first boot
     (`cat sys/config.cfg` via the shell, or SFS read).
  3. Pixel assert: desktop pixel still `(26,32,48)` at a desktop coordinate
     (gradient top == default top color).
- Timezone test: boot with a config containing `timezone=+180`, run `date`,
  assert the shown time is +3h over a boot without config (or over CMOS in
  QEMU `-rtc base=localtime`); simplest robust check: serial-log the offset
  applied (`config: timezone +180`).
- Regression: `make test` (ipctest, manytest, notepadtest, sleeptest,
  rngtest, blktest, virtiotest, netlooptest, rtctest, linux tests) stays green.

## Files touched

- New: `kernel/config.c`, `kernel/config.h`.
- Modified: `drivers/rtc.c` (+`rtc_set_tz`, epoch conversion), `kernel/kernel.c`
  (config_load call + ASCII logo), `programs/wm.c` (gradient desktop +
  config read), `Makefile` (add `kernel/config.o` to `KERNEL_OBJS`),
  `scripts/` (config test harness).
