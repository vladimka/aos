#!/usr/bin/env python3
import sys
import time

from qtest import QTest, ppm_data

TXT_X0, TXT_X1 = 21, 660
TXT_Y0 = 39
TXT_ROWS = 26
TXT_THRESHOLD = 400


def row_profile(path):
    """Per-16px text-row bright-pixel counts across the term text band."""
    w, _, data = ppm_data(path)
    rows = []
    for r in range(TXT_ROWS):
        y = TXT_Y0 + r * 16
        n = 0
        for yy in range(y, y + 16):
            row = data[(yy * w + TXT_X0) * 3:(yy * w + TXT_X1 + 1) * 3]
            for i in range(0, len(row), 3):
                if row[i] >= 0xC0 and row[i + 1] >= 0xC0 and row[i + 2] >= 0xC0:
                    n += 1
        rows.append(n)
    return rows


def nonempty_rows(profile):
    return [i for i, n in enumerate(profile) if n > 40]


def main():
    root_ppm = "/tmp/aos-lindir-root.ppm"
    proc_ppm = "/tmp/aos-lindir-proc.ppm"
    with QTest("lindir") as q:
        q.boot_and_ready()
        q.dock_spawn_term()
        q.type_text("lin/ls /\n")
        time.sleep(3)
        q.screenshot(root_ppm)
        q.type_text("lin/ls /proc\n")
        for _ in range(40):
            time.sleep(1)
            q.screenshot(proc_ppm)
            if len(nonempty_rows(row_profile(proc_ppm))) > \
               len(nonempty_rows(row_profile(root_ppm))) + 2:
                break
        q.screenshot(proc_ppm)

        log = q.serial_read()
        if "KERNEL PANIC" in log:
            raise AssertionError("lin/ls triggered a kernel panic")

        row_root = row_profile(root_ppm)
        row_proc = row_profile(proc_ppm)

        root_rows = nonempty_rows(row_root)
        if sum(row_root) <= TXT_THRESHOLD:
            raise AssertionError("lin/ls / did not render a listing")
        if len(root_rows) < 3:
            raise AssertionError("lin/ls / produced unexpectedly few text rows")

        proc_rows = nonempty_rows(row_proc)
        added = [r for r in proc_rows if r not in root_rows]
        if not added:
            raise AssertionError("lin/ls /proc did not render an additional listing")

        root_data = [n for n in row_root[2:6] if n > 40]
        if not root_data:
            raise AssertionError("no recognisable `ls /` data rows")
        if len(added) < 3:
            raise AssertionError(
                "ls /proc added only %d rows (expected ~3 procfs entries)" % len(added))
    print("PASS: musl ls lists SFS / and procfs /proc via real fds "
          "(root rows %s, /proc additionally %s)" % (root_rows, added))
    return 0


if __name__ == "__main__":
    sys.exit(main())
