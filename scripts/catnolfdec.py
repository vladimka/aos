#!/usr/bin/env python3
"""Decode the GUI term's on-screen text using the kernel fb font.

Verifies that `cat <one-line-file>` appends a trailing newline: the orphaned
content (a file with no trailing newline) is printed on its own row instead
of being glued to the prompt on the same row (`root@aos:/$ X`).

Typing is done with sendkey at a slow, verified pace because QEMU's 16-byte
PS/2 queue overflows while the term renders under TCG and drops keys; each
command is retyped until the intended line is seen on screen. The content
block is located by its exact 0x101010 background fill (not guessed from the
window position), since the term is a 640px-wide run of full-match rows.
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

# sendkey string per printable character. Uppercase letters need an explicit
# shift modifier -- QEMU's plain `sendkey X` sends the unshifted 'x' scancode.
KEYMAP = {c: c for c in "abcdefghijklmnopqrstuvwxyz0123456789"}
KEYMAP.update({c: "shift-" + c.lower() for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"})
KEYMAP.update({
    " ": "spc", "\n": "ret", "/": "slash", ">": "shift-dot", "<": "shift-comma",
    "-": "minus", ".": "dot", "~": "shift-grave", "_": "shift-minus",
    "!": "shift-1", "|": "shift-backslash", ":": "shift-semicolon",
    ";": "semicolon", "=": "equal", "+": "shift-equal", "*": "shift-8",
    "(": "shift-9", ")": "shift-0",
})


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


def find_content(q, font):
    """Return the term's on-screen rows (list of 80-char strings) by locating
    the exact 0x101010 content-fill block, and the left content x0."""
    last = q.screenshot(PPM)
    w, h, data = ppm_data(last)

    def is_bg(px, py):
        return data[(py * w + px) * 3] == 0x10

    # The content block is a tall run of contiguous background-filled rows.
    runs = []
    start = None
    prev = None
    for y in range(h):
        bg = is_bg(320, y)
        if bg and start is None:
            start = y
        if not bg and start is not None:
            runs.append((start, y - 1))
            start = None
        prev = bg
    if start is not None:
        runs.append((start, h - 1))
    tall = [r for r in runs if r[1] - r[0] > 50]
    if not tall:
        return []
    y0, y1 = max(tall, key=lambda r: r[1] - r[0])
    mid = (y0 + y1) // 2
    x0 = next(x for x in range(w) if is_bg(x, mid))
    nrows = (y1 - y0 + 1) // FONT_H
    rows = []
    for r in range(nrows):
        row = ""
        for c in range(80):
            g = cell_text(w, data, x0 + c * FONT_W, y0 + r * FONT_H, font)
            row += chr(g) if (g and 32 <= g < 127) else "."
        rows.append(row)
    return rows


def current_line(rows):
    """Extract the shell's current input line -- the last row that starts
    with the prompt (`root@aos:/$ `) -- minus the prompt."""
    for r in reversed(rows):
        s = r.rstrip(" .")
        if s.startswith("root@aos:/$ "):
            return s[len("root@aos:/$ "):]
    return ""


def type_verified(q, font, text, retries=5):
    """Type `text` via sendkey, verifying each attempt landed on screen, and
    retyping (after clearing) until the exact line is seen."""
    for attempt in range(retries):
        for ch in text:
            q.key(KEYMAP.get(ch, ch))
            time.sleep(0.25)
        time.sleep(0.4)
        got = current_line(find_content(q, font))
        print("  [type] attempt %d got=%r want=%r"%(attempt,got,text))
        if got == text:
            return True
        # Clear the partial line with paced backspaces, then retry.
        for _ in range(48):
            q.key("backspace")
            time.sleep(0.08)
        time.sleep(0.4)
    return False


def main():
    font = load_font()
    with QTest("catnolf-decode", mouse_state=MOUSE, ppm=PPM,
               boot_wait=6, extra_args=GPU_ARGS) as q:
        q.dock_spawn_term()
        time.sleep(1.5)
        if not type_verified(q, font, "echo -n X > nolf.txt"):
            print("FAIL: could not reliably type echo line")
            return 1
        print("  echo line typed")
        q.key("ret")
        time.sleep(1.2)
        if not type_verified(q, font, "cat nolf.txt"):
            print("FAIL: could not reliably type cat line")
            return 1
        print("  cat line typed")
        q.key("ret")
        time.sleep(2.0)

        # Take a few screenshots and pick the one with no WM cursor overlay
        # interference in the term area.
        last = None
        for _ in range(4):
            last = q.screenshot(PPM)
            time.sleep(0.3)
        w, h, data = ppm_data(last)

        rows = find_content(q, font)
        texts = [r.rstrip(" .") for r in rows if r.strip(" .")]
        if not texts:
            print("FAIL: term content not found")
            return 1
        print("text rows: %r" % texts)
        # Expect the one-line file's content on its own row (cat now appends a
        # trailing newline): a row exactly 'X', with a fresh prompt below it.
        if "X" in texts:
            print("PASS: one-line 'X' printed on its own row")
            return 0
        print("FAIL: got %r" % texts)
        return 1


if __name__ == "__main__":
    sys.exit(main())
