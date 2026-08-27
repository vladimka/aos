#!/usr/bin/env python3
"""Decode the term screen after cat of a no-newline file."""
import sys, time
from qtest import QTest, ppm_data

MOUSE_STATE = "/tmp/aos-catnolf.state"
PPM = "/tmp/aos-catnolf.ppm"
GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

def row_bright(path, r):
    w, h, data = ppm_data(path)
    y0, y1 = 39 + r * 16, 39 + r * 16 + 15
    cols = []
    for x in range(21, 661):
        n = 0
        for y in range(y0, y1 + 1):
            off = (y * w + x) * 3
            if data[off] >= 0x90 and data[off+1] >= 0x90 and data[off+2] >= 0x90:
                n += 1
        cols.append(1 if n >= 4 else 0)
    # compress into runs
    runs = []
    i = 0
    while i < len(cols):
        if cols[i]:
            j = i
            while j < len(cols) and cols[j]: j += 1
            runs.append((i, j))
            i = j
        else:
            i += 1
    return runs

def main():
    with QTest("catnolf", mouse_state=MOUSE_STATE, ppm=PPM, boot_wait=6,
               extra_args=GPU_ARGS) as q:
        q.dock_spawn_term()
        sh_h, sh_w = 0, 0
        for i in range(15):
            for r in range(6):
                pass
        q.type_text("echo -n X > nolf.txt\n")
        time.sleep(1)
        q.type_text("cat nolf.txt\n")
        time.sleep(3)
        q.screenshot("/tmp/catnolf-after.ppm")
        for r in range(8):
            print("row %d: %s" % (r, row_bright("/tmp/catnolf-after.ppm", r)))
        # Also render X? compare glyph
        return 0

if __name__ == "__main__":
    sys.exit(main())
