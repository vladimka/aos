#!/usr/bin/env python3
import sys
import time

from qtest import QTest, count_bright

TXT_X0, TXT_X1 = 21, 660
TXT_Y0, TXT_Y1 = 39, 39 + 26 * 16
TXT_THRESHOLD = 100


def main():
    with QTest("linhello") as q:
        q.boot_and_ready()
        q.dock_spawn_term()
        before = q.screenshot("/tmp/aos-linhello-before.ppm")
        q.type_text("lin/hello\n")
        end = time.time() + 25
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = q.serial_read()
            if "KERNEL PANIC" in log:
                break
        if "KERNEL PANIC" in log:
            raise AssertionError("lin/hello triggered a kernel panic")
        ppm = q.screenshot("/tmp/aos-linhello.ppm")
        before_txt = count_bright(before, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        after_txt = count_bright(ppm, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        if "cannot run command" in log or "Unknown command" in log:
            raise AssertionError("spawn failed: %r" % log.strip().splitlines()[-1])
        if after_txt - before_txt <= TXT_THRESHOLD:
            raise AssertionError(
                "terminal did not render lin/hello output (band text grew %d, want > %d)"
                % (after_txt - before_txt, TXT_THRESHOLD))
    print("PASS: musl hello runs in the kernel shell")
    return 0


if __name__ == "__main__":
    sys.exit(main())
