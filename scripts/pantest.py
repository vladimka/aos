#!/usr/bin/env python3
"""pantest.py — VBE pan scrollback E2E regression.

Boots aos.iso in console-only mode on a virtual VGA (Bochs VBE for the pan
window), then proves that PgUp/PgDn pan through the scrollback history via
Y_OFFSET and return to a live view pixel-identical to the pre-scroll one.

Marker: two consecutive blank rows planted mid-stream (a cat of a file
containing ``\\n\\n``), between the boot log and a flood of x's. Blank rows
survive the scrollback ring (it stores characters); when scrolled into view
they render as two adjacent near-black 16 px bands, which the x-soup never
produces. So "an adjacent blank band pair appears mid-climb then the final
view matches live" is the whole assertion.

Kernel facts relied on:
  * `hwaccel: VBE pan 32 scrollback rows (Y_OFFSET)` banner in the boot log.
  * Every PgUp/PgDn emits a serial line `vga_scroll: d=.. off=A->B ..`;
    the history top is `off == count`, live is `off == 0`.

Usage: python3 scripts/pantest.py
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from configtest import build_sfs
from qtest import QTest, ppm_data

IMG = "/tmp/aos-pantest.img"
BLANK = 60  # lit pixels per 16px band below this = blank row


def band_lit(d, w):
    """Lit-pixel count per 16px text row (screen is max_y+1 == 49 rows)."""
    bands = []
    stride = w * 3
    h = len(d) // stride
    for by in range(0, h, 16):
        row_end = min(by + 16, h)
        n = 0
        for y in range(by, row_end):
            base = y * stride
            for x in range(w):
                i = base + x * 3
                if d[i] != 0 or d[i + 1] != 0 or d[i + 2] != 0:
                    n += 1
        if n:
            bands.append(n)
    return bands


def blank_pairs(bands):
    return sum(1 for i in range(len(bands) - 1)
               if bands[i] < BLANK and bands[i + 1] < BLANK)


def serial_off(data):
    m = re.search(rb'off=(\d+)->(\d+) count=(\d+)', data)
    return m and (int(m.group(1)), int(m.group(2)))


def main():
    try:
        os.unlink(IMG)
    except FileNotFoundError:
        pass
    open(IMG, "wb").write(build_sfs([
        ("sys/config.cfg", b"console_only=1\n"),
        ("/mark", b"\n\nlintestmark\n"),  # two leading blank lines (the marker)
    ]))
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)

    with QTest("pantest", serial_mode="socket", extra_args=[
                   "-vga", "std",
                   "-drive", "file=" + IMG + ",format=raw,if=none,id=d0",
                   "-device", "virtio-blk-pci,disable-modern=on,drive=d0",
               ]) as q:
        s = q.serial_socket()
        boot = q.serial_drain(s, timeout=60, needle=b"AOS>")
        if b"VBE pan 32 scrollback rows" not in boot:
            print("FAIL: no VBE pan banner in boot log")
            return 1
        print("VBE pan banner: OK")

        def cmd(c, timeout=60):
            s.sendall(c + b"\n")
            return q.serial_drain(s, timeout=timeout, needle=b"AOS>")

        cmd(b"cat /mark")
        cmd(b"lin/piptest gen 20000 | lin/cat", timeout=90)
        time.sleep(0.6)

        q.screenshot("/tmp/pantest-live.ppm")
        w, h, d = ppm_data("/tmp/pantest-live.ppm")
        live = d
        live_bands = band_lit(d, w)
        if any(b < BLANK for b in live_bands):
            print("FAIL: live after flood contains a blank row")
            return 1
        print("live is all-dense (%d rows): OK" % len(live_bands))

        # PgUp until the scrollback top is reached; detect the blank marker
        # pair somewhere in the climb (it sits between boot and the flood).
        up_blank_pairs = []
        up_first_blank = None
        reached_top = False
        for _ in range(30):
            q.key("pgup")
            time.sleep(0.45)
            off = serial_off(q.serial_drain(s, timeout=1.0))
            q.screenshot("/tmp/pantest-up.ppm")
            w, h, d = ppm_data("/tmp/pantest-up.ppm")
            pairs = blank_pairs(band_lit(d, w))
            up_blank_pairs.append(pairs)
            if up_first_blank is None:
                up_first_blank = pairs
            if off and off[0] == off[1]:
                reached_top = True
        if not reached_top:
            print("FAIL: never reached scrollback top")
            return 1
        if up_first_blank:
            print("FAIL: first PgUp view (pure flood) shows a blank pair")
            return 1
        if max(up_blank_pairs) < 1:
            print("FAIL: blank marker never panned into view")
            return 1
        print("marker panned into view (%d adjacent blanks): OK"
              % max(up_blank_pairs))

        # PgDn back to live; the final screen must match the pre-scroll live.
        down_blank_pairs = []
        back = None
        for _ in range(30):
            q.key("pgdn")
            time.sleep(0.45)
            off = serial_off(q.serial_drain(s, timeout=1.0))
            q.screenshot("/tmp/pantest-down.ppm")
            w, h, d = ppm_data("/tmp/pantest-down.ppm")
            down_blank_pairs.append(blank_pairs(band_lit(d, w)))
            back = d
            if off and off[1] == 0:
                break
        if back is None:
            print("FAIL: never returned to live")
            return 1
        if max(down_blank_pairs) > 0:
            print("FAIL: blank pair still visible after returning to live")
            return 1

        diff = sum(1 for i in range(0, len(live) - 2, 3)
                   if live[i] != back[i] or live[i + 1] != back[i + 1]
                   or live[i + 2] != back[i + 2])
        if diff > 2000:
            print("FAIL: scrollback return not pixel-identical "
                  "(%d px differ)" % diff)
            return 1
        print("PgDn returned to pixel-identical live (%d px): OK" % diff)
    return 0


if __name__ == "__main__":
    sys.exit(main())
