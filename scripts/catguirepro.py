#!/usr/bin/env python3
"""GUI repro for the 'cat with a one-line file' bug - edge cases."""
import sys, time
from qtest import QTest, count_bright

MOUSE_STATE = "/tmp/aos-catgui.state"
PPM = "/tmp/aos-catgui.ppm"
GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

def main():
    with QTest("catgui", mouse_state=MOUSE_STATE, ppm=PPM, boot_wait=6,
               extra_args=GPU_ARGS) as q:
        q.dock_spawn_term()
        band = q.term_text_band()

        def run(cmds, label, min_delta=80):
            before = count_bright(q.screenshot("/tmp/cg-%s-0.ppm" % label), band[0], band[1], band[2], band[3])
            for c in cmds:
                q.type_text(c)
            after = before
            for _ in range(60):
                time.sleep(0.25)
                q.screenshot("/tmp/cg-%s-1.ppm" % label)
                after = count_bright(q.screenshot("/tmp/cg-%s-1.ppm" % label), band[0], band[1], band[2], band[3])
                if after - before >= min_delta:
                    break
            print("%s: delta=%d" % (label, after - before))
            return after - before

        run(["echo -n X > nolf.txt\n"], "nolf")
        d = run(["cat nolf.txt\n"], "cat-nolf", min_delta=10)
        if d < 10:
            raise AssertionError("cat of no-trailing-newline file failed")

        run(["echo line1 > ml.txt\n", "echo line2 >> ml.txt\n"], "ml")
        d = run(["cat ml.txt\n"], "cat-ml", min_delta=80)
        if d < 80:
            raise AssertionError("cat multi-line failed")

        d = run(["cat -n nolf.txt\n"], "catn-nolf", min_delta=10)
        if d < 10:
            raise AssertionError("cat -n single-line failed")

        d = run(["cat -n ml.txt\n"], "catn-ml", min_delta=80)
        if d < 80:
            raise AssertionError("cat -n multi-line failed")
        print("ALL PASS")
        return 0

if __name__ == "__main__":
    sys.exit(main())
