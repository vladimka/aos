#!/usr/bin/env python3
"""Decode the GUI term's on-screen text using the kernel fb font.

Verifies the sh post-command prompt fix: after `cat <one-line-file>` the
orphaned content (no trailing newline) must remain visible, glued to the
prompt on the same row (e.g. `Xroot@aos:/$ `), instead of being erased by
the old `\r\x1b[K` redraw.
"""
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from qtest import QTest, ppm_data  # noqa: E402

FONT_HDR = os.path.join(ROOT, "drivers", "fb_font.h")

PPM = "/tmp/aos-catnolf-decode.ppm"
MOUSE = "/tmp/aos-catnolf-decode.state"
GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

FONT_W, FONT_H = 8, 16
FG = 0xD8D8D8
BG = 0x101010


def load_font():
    src = []
    started = False
    with open(FONT_HDR) as f:
        for line in f:
            if line.startswith("static unsigned char fb_font"):
                started = True
            if started:
                src.append(line)
                if line.rstrip().endswith("};"):
                    break
    data = "".join(src)
    data = data[data.index("{"):data.rindex("}") + 1]
    vals = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", data)]
    assert len(vals) == 256 * 16, "font parse: got %d bytes" % len(vals)
    font = {}
    for c in range(256):
        bits = vals[c * 16:(c + 1) * 16]
        rows = [((bits[r] & (0x80 >> col)) and 1 or 0)
                for r in range(16) for col in range(8)]
        font[c] = rows
    return font


def cell_text(w, data, x0, y0, font):
    """Decode one 8x16 cell at pixel (x0, y0). Returns glyph index or None."""
    best, best_n = None, -1
    for g, rows in font.items():
        hit = 0
        for i, bit in enumerate(rows):
            px = (y0 + (i // 8)) * w + (x0 + (i % 8))
            b = data[px * 3]
            on = b >= 0xC0
            if on == bit:
                hit += 1
        if hit > best_n:
            best_n = hit
            best = g
    return best


def decode_region(w, data, x0, y0, cols, rows, font):
    out = []
    for r in range(rows):
        row = ""
        for c in range(cols):
            g = cell_text(w, data, x0 + c * FONT_W, y0 + r * FONT_H, font)
            row += chr(g) if (g and 32 <= g < 127) else "."
        out.append(row)
    return out


def main():
    font = load_font()
    with QTest("catnolf-decode", mouse_state=MOUSE, ppm=PPM,
               boot_wait=6, extra_args=GPU_ARGS) as q:
        q.dock_spawn_term()
        time.sleep(1)
        q.type_text("echo -n X > nolf.txt\n")
        time.sleep(0.5)
        q.type_text("cat nolf.txt\n")
        time.sleep(1.5)
        # Take a few screenshots and pick the one with no WM cursor overlay
        # interference in the term area (WM cursor idles at the dock, y=724).
        last = None
        for _ in range(4):
            last = q.screenshot(PPM)
            time.sleep(0.3)
        w, h, data = ppm_data(last)

        # Locate the term content block: rows whose pixels are dominated by
        # term colors (0x101010 fill, 0x000000/0xFFFFFF glyph cells).
        def is_fill(px, py):
            raw = data[(py * w + px) * 3]
            return raw == 0x10 or raw == 0x00 or raw == 0xFF

        def row_score(y):
            return sum(1 for x in range(w) if is_fill(x, y))

        # The term content is a tall 640px-wide run of ~full-match rows.
        y0 = None
        for y in range(20, h - 26 * FONT_H + 1):
            window = [row_score(y + k * 8) for k in range(52)]  # every 8px
            if sum(wn >= 400 for wn in window) >= 45:
                y0 = y
                break
        if y0 is None:
            print("FAIL: term content not found")
            return 1
        # Snap to the 16px glyph grid anchored at a window slot.
        y0 = (y0 + 15) // 16 * 16
        # Left edge: leftmost term-color pixel on a content row.
        base = y0 + 2 * FONT_H + 8
        x0 = 0
        for x in range(w):
            if is_fill(x, base):
                x0 = x
                break
        print("content top y = %d  x0 = %d" % (y0, x0))
        rows = decode_region(w, data, x0, y0, 80, 26, font)
        last_text = rows[-1].rstrip('.')
        print("last text row: %r" % last_text)
        nonempty = [r for r in rows if r.strip('.')]
        print("last nonempty: %r" % nonempty[-1].rstrip('.'))
        got = nonempty[-1].strip('.') if nonempty else ""
        # Expect the one-line file's content glued to the prompt: the row
        # starts with the orphan char 'X' immediately followed by the prompt.
        if got[0] == "X" and "aos:" in got and got.endswith("$"):
            print("PASS: orphaned content visible (glued after prompt)")
            return 0
        print("FAIL: got %r" % got)
        return 1


if __name__ == "__main__":
    sys.exit(main())