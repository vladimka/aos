#!/usr/bin/env python3
"""Decisive: does a no-trailing-newline cat output survive in the GUI term?"""
import sys, time
from qtest import QTest, ppm_data

MOUSE_STATE = "/tmp/aos-catnolf.state"
PPM = "/tmp/aos-catnolf.ppm"
GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

def runs(path):
    w, h, data = ppm_data(path)
    rows = []
    for r in range(12):
        y0, y1 = 39 + r * 16, 39 + r * 16 + 15
        cols = []
        for x in range(21, 661):
            n = 0
            for y in range(y0, y1 + 1):
                off = (y * w + x) * 3
                if data[off] >= 0x90 and data[off+1] >= 0x90 and data[off+2] >= 0x90:
                    n += 1
            cols.append(1 if n >= 4 else 0)
        rr = []
        i = 0
        while i < len(cols):
            if cols[i]:
                j = i
                while j < len(cols) and cols[j]: j += 1
                rr.append((i, j))
                i = j
            else:
                i += 1
        rows.append(rr)
    return rows

def main():
    with QTest("catnolf", mouse_state=MOUSE_STATE, ppm=PPM, boot_wait=6,
               extra_args=GPU_ARGS) as q:
        q.dock_spawn_term()
        q.type_text("echo -n ABCDEFGHIJ > f\n")
        time.sleep(1)
        q.type_text("cat f\n")
        time.sleep(3)
        q.type_text("echo ===MARK===\n")
        time.sleep(3)
        q.screenshot("/tmp/catnolf2.ppm")
        for r, rr in enumerate(runs("/tmp/catnolf2.ppm")):
            print("row %d: %s" % (r, rr))

if __name__ == "__main__":
    sys.exit(main())
