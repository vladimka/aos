#!/usr/bin/env python3
import os
import sys
import time

from qtest import QTest, count_bright

TXT_X0, TXT_X1 = 21, 660
TXT_Y0, TXT_Y1 = 39, 39 + 26 * 16
TXT_THRESHOLD = 100


def main():
    with QTest("many") as q:
        q.boot_and_ready()
        q.dock_spawn_term()
        before = q.screenshot("/tmp/aos-many-before.ppm")
        q.type_text("many\n")
        end = time.time() + 25
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = q.serial_read()
            if "KERNEL PANIC" in log:
                break
            if "MANY FAIL" in log:
                break
            time.sleep(1)
        if "KERNEL PANIC" in log:
            raise AssertionError("many triggered a kernel panic")
        if "MANY FAIL" in log:
            raise AssertionError("many failed to spawn/collect tasks")
        ppm = q.screenshot("/tmp/aos-many.ppm")
        if os.path.getsize(ppm) <= 1024:
            raise AssertionError("window manager did not produce a framebuffer dump")
        before_txt = count_bright(before, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        after_txt = count_bright(ppm, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        if after_txt - before_txt <= TXT_THRESHOLD:
            raise AssertionError(
                "terminal did not render MANY PASS (band text grew %d, want > %d)"
                % (after_txt - before_txt, TXT_THRESHOLD))
    print("PASS: many-task stress (10 concurrent x 4 rounds)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
