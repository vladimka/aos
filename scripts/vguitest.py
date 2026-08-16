#!/usr/bin/env python3
"""Boot the ISO with virtio-vga and assert the virtio-gpu driver initializes."""
import os, sys, time, re
sys.path.insert(0, os.path.dirname(__file__))
from qtest import QTest, ROOT, ppm_pixel

ISO = os.path.join(ROOT, "aos.iso")

# A pixel in the desktop gradient area (away from windows and the dock):
# term window occupies x=20..660 y=20..436, clock x=44..304 y=48..148,
# dock bottom y=708+. (700,400) is clear desktop gradient.
DESKTOP_PX = (700, 400)


def desktop_ok(q):
    q.screenshot(q.ppm)
    px = ppm_pixel(q.ppm, *DESKTOP_PX)
    print("  desktop pixel %s = %s" % (DESKTOP_PX, px))
    # the desktop gradient is wp_top 0x1A2030 -> wp_bot 0x0E1620, never black
    return px[0] > 15 and px[1] > 15 and px[2] > 15


def main():
    with QTest("vgu", serial_mode="file") as q:
        q.start(extra_args=["-vga", "none", "-device", "virtio-vga,disable-modern=on"])
        log = q.serial_read()
        time.sleep(4)   # give the WM time to start rendering through the GPU
        ok = desktop_ok(q)
        log = q.serial_read()
    assert "vgu: active" in log, "virtio-gpu driver did not activate"
    assert "vgu: flip ok" in log, "vgu selftest flip did not run"
    assert "vgu: cursor ok" in log, "vgu cursor selftest did not run"
    assert "wm: gpu mode, back=" in log, "WM did not enter GPU mode"
    assert ok, "desktop did not render through virtio-gpu scanout (black screen)"
    print("VGU TEST OK")

if __name__ == "__main__":
    main()