#!/usr/bin/env python3
"""Row-by-row compare: withnl vs nolf, matching scroll position."""
import sys, time
from qtest import QTest, ppm_data

MOUSE_STATE = "/tmp/aos-catnolf.state"
PPM = "/tmp/aos-catnolf.ppm"
GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

def rows(path, nrow=(416-16)//16, thresh=4):
    w, h, data = ppm_data(path)
    out = []
    for r in range(nrow):
        y0, y1 = 39 + r*16, 39 + r*16 + 15
        cols = []
        for x in range(21, 661):
            n = 0
            for y in range(y0, y1+1):
                off = (y*w + x)*3
                if data[off] >= 0x90 and data[off+1] >= 0x90 and data[off+2] >= 0x90:
                    n += 1
            cols.append(1 if n >= thresh else 0)
        # runs
        runs = []
        i = 0
        while i < len(cols):
            if cols[i]:
                j = i
                while j < len(cols) and cols[j]:
                    j += 1
                runs.append((i, j))
                i = j
            else:
                i += 1
        out.append(runs)
    return out

def main():
    with QTest("catnolf", mouse_state=MOUSE_STATE, ppm=PPM, boot_wait=6,
               extra_args=GPU_ARGS) as q:
        q.dock_spawn_term()
        q.type_text("echo -n hello > nolf.txt\n")
        time.sleep(1)
        q.type_text("cat nolf.txt\n")
        time.sleep(3)
        q.screenshot("/tmp/nolf55.ppm")
        q.type_text("echo -n hello > withnl.txt\n")
        time.sleep(1)
        # NOTE: file is fresh, then cat it
        q.type_text("cat withnl.txt\n")
        time.sleep(3)
        q.screenshot("/tmp/withnl55.ppm")
        print("=== nolf (cat of newline-less file) ===")
        for r, rr in enumerate(rows("/tmp/nolf55.ppm")):
            if rr:
                print("row %2d: %s" % (r, rr))
        print("=== withnl (cat of \\n-terminated file) ===")
        for r, rr in enumerate(rows("/tmp/withnl55.ppm")):
            if rr:
                print("row %2d: %s" % (r, rr))

if __name__ == "__main__":
    sys.exit(main())
