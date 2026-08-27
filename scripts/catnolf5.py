#!/usr/bin/env python3
"""Differential: count text rows. withnl should have ONE MORE text row (the 'hello')."""
import sys, time
from qtest import QTest, ppm_data

MOUSE_STATE = "/tmp/aos-catnolf.state"
PPM = "/tmp/aos-catnolf.ppm"
GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

def filled_rows(path, x0=21, x1=661, y0=39, thresh=40):
    """rows (16px each) of the term content with total bright px >= thresh."""
    w, h, data = ppm_data(path)
    res = []
    for r in range(25):
        yy0, yy1 = y0 + r*16, y0 + r*16 + 15
        n = 0
        for y in range(yy0, yy1+1):
            for x in range(x0, x1):
                off = (y*w + x)*3
                if data[off] >= 0x90 and data[off+1] >= 0x90 and data[off+2] >= 0x90:
                    n += 1
        if n >= thresh:
            res.append((r, n))
    return res

def main():
    with QTest("catnolf", mouse_state=MOUSE_STATE, ppm=PPM, boot_wait=6,
               extra_args=GPU_ARGS) as q:
        q.dock_spawn_term()
        time.sleep(1)
        q.type_text("clear\n")
        time.sleep(2)
        # withnl: file WITH trailing newline
        q.type_text("echo hello > wnl\n")
        time.sleep(1)
        q.type_text("cat wnl\n")
        time.sleep(3)
        q.screenshot("/tmp/wnl55.ppm")
        print("=== withnl rows:", filled_rows("/tmp/wnl55.ppm"))
        # nolf: file WITHOUT trailing newline
        q.type_text("clear\n")
        time.sleep(2)
        q.type_text("echo -n hello > nolf\n")
        time.sleep(1)
        q.type_text("cat nolf\n")
        time.sleep(3)
        q.screenshot("/tmp/nolf55b.ppm")
        print("=== nolf rows:", filled_rows("/tmp/nolf55b.ppm"))

if __name__ == "__main__":
    sys.exit(main())
