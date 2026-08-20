#!/usr/bin/env python3
"""E2E test: `ls` emits SGR color when TERM is set, and not on redirect.

SGR bytes are stripped from COM1 by the kernel serial filter (Task 3), so
color can only be observed in the GUI terminal, which renders 38;5;33 as
rgb(0,135,255) (xterm_rgb[33] = 0x0087FF). bin/sh sets TERM=aos for child
commands unless the command has a redirect (sh_build_env term_off), so:

  - `ls /d`  in the GUI term -> "sub/" is drawn blue
  - `ls /d > /sgr.txt` then `cat /sgr.txt` -> no blue (file has no ESC)

Also asserts the serial log (whole boot) contains no 0x1b (filter proof).
"""
import sys
import time

from qtest import QTest, ppm_data

MOUSE_STATE = "/tmp/aos-sgrcolor.state"
PPM = "/tmp/aos-sgrcolor.ppm"

GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

BLUE = (0, 135, 255)          # xterm_rgb[33] = 0x0087FF
TEXT_BG = (16, 16, 16)        # 0x101010 term content background


def count_color(path, rgb, x0, y0, x1, y1):
    """Count pixels exactly matching ``rgb`` in the rectangle."""
    w, _, data = ppm_data(path)
    n = 0
    for y in range(y0, y1 + 1):
        row = data[(y * w + x0) * 3:(y * w + x1 + 1) * 3]
        for i in range(0, len(row), 3):
            if (row[i], row[i + 1], row[i + 2]) == rgb:
                n += 1
    return n


def main():
    with QTest("sgrcolor", mouse_state=MOUSE_STATE, ppm=PPM, boot_wait=6,
               extra_args=GPU_ARGS) as q:
        # 1. Open a terminal; the dirs it lists later are colored.
        q.dock_spawn_term()
        q.type_text("mkdir -p /d/sub\n")
        time.sleep(1)

        # 2. `ls /d` must render the dir name blue (TERM set by bin/sh).
        q.type_text("ls /d\n")
        blue = 0
        path = "/tmp/sgrcolor-1-ls.ppm"
        for _ in range(40):
            time.sleep(0.25)
            q.screenshot(path)
            blue = count_color(path, BLUE, 21, 39, 660, 455)
            if blue > 0:
                break
        if blue == 0:
            raise AssertionError(
                "ls /d drew no blue dir name in the GUI term (SGR not emitted)")

        # 3. Redirect must drop TERM -> /sgr.txt has no ESC -> cat draws no blue.
        q.type_text("ls /d > /sgr.txt\n")
        time.sleep(1)
        q.type_text("cat /sgr.txt\n")
        blue2 = 0
        path2 = "/tmp/sgrcolor-2-cat.ppm"
        for _ in range(40):
            time.sleep(0.25)
            q.screenshot(path2)
            blue2 = count_color(path2, BLUE, 21, 39, 660, 455)
            if blue2 >= 0:
                break
        # The first ls output may still be on screen, so compare growth.
        with open(q.ser, "rb") as f:
            slog = f.read()
        if b"\x1b" in slog:
            raise AssertionError("serial log contains ESC bytes (filter broken)")
        if blue2 > blue:
            raise AssertionError(
                "cat /sgr.txt grew the blue pixel count (%d -> %d): "
                "redirect kept TERM / file has ESC" % (blue, blue2))
        print("  ok: redirect stripped color, serial log has no ESC")
        print("PASS: sgr colors (TERM-set blue, redirect clean, serial filter)")
    return 0


if __name__ == "__main__":
    sys.exit(main())