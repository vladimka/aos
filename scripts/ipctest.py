#!/usr/bin/env python3
import sys
import time

from qtest import QTest, count_bright

TXT_X0, TXT_X1 = 21, 660
TXT_Y0, TXT_Y1 = 39, 70
TXT_THRESHOLD = 100


def main():
    with QTest("ipc") as q:
        q.boot_and_ready()
        q.dock_spawn_term()
        q.screenshot("/tmp/aos-ipc-before.ppm")
        q.type_text("ipctest\n")
        time.sleep(3)
        log = q.serial_read()
        if "KERNEL PANIC" in log:
            raise AssertionError("ipctest triggered a kernel panic")
        ppm = q.screenshot("/tmp/aos-ipc.ppm")
        before = count_bright("/tmp/aos-ipc-before.ppm", TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        after = count_bright(ppm, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        if after - before <= TXT_THRESHOLD:
            raise AssertionError(
                "terminal did not render IPC TEST PASS (band text grew %d, want > %d)"
                % (after - before, TXT_THRESHOLD))
        q.mouse_move(1, 0)
        time.sleep(0.3)
        q.screenshot("/tmp/aos-ipc-after.ppm")
    print("PASS: IPC exit notification and WM regression")
    return 0


if __name__ == "__main__":
    sys.exit(main())
