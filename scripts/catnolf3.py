#!/usr/bin/env python3
"""Differential: cat of newline-less file vs newline-terminated file."""
import sys, time
from qtest import QTest, ppm_data

MOUSE_STATE = "/tmp/aos-catnolf.state"
PPM = "/tmp/aos-catnolf.ppm"
GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

def band_bright(path, y0=39, y1=39+416, x0=21, x1=661):
    w, h, data = ppm_data(path)
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            off = (y * w + x) * 3
            if data[off] >= 0x90 and data[off+1] >= 0x90 and data[off+2] >= 0x90:
                n += 1
    return n

def main():
    with QTest("catnolf", mouse_state=MOUSE_STATE, ppm=PPM, boot_wait=6,
               extra_args=GPU_ARGS) as q:
        q.dock_spawn_term()
        q.type_text("echo hello > withnl.txt\n")
        time.sleep(1)
        q.type_text("echo -n hello > nolf.txt\n")
        time.sleep(1)
        q.type_text("cat withnl.txt\n")
        time.sleep(3)
        q.screenshot("/tmp/cat-withnl.ppm")
        b_withnl = band_bright("/tmp/cat-withnl.ppm")
        q.type_text("cat nolf.txt\n")
        time.sleep(3)
        q.screenshot("/tmp/cat-nolf.ppm")
        b_nolf = band_bright("/tmp/cat-nolf.ppm")
        print("bright withnl=%d  nolf=%d  diff=%d" % (b_withnl, b_nolf, b_nolf - b_withnl))
        # Expected if nolf output is WIPED: nolf has one fewer text row.
        return 0

if __name__ == "__main__":
    sys.exit(main())
