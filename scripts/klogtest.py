#!/usr/bin/env python3
import re
import sys

from qtest import QTest

BOOT_MARKS = [
    "VFS: / [sfs2] root=1",
    "Filesystem ready.",
    "AOS>",
]


def main():
    with QTest("klog", serial_mode="socket") as q:
        s = q.serial_socket()
        log = q.boot_and_ready(socket=s).decode(errors="replace")
        for mark in BOOT_MARKS:
            if mark not in log:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, log[-400:]))

        s.sendall(b"cat /proc/klog\n")
        out = q.serial_drain(s, timeout=30, needle=b"klog: ready")
        otext = out.decode(errors="replace")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during cat /proc/klog:\n"
                                 + otext[-400:])

        lines = otext.splitlines()
        tstamped = [l for l in lines if re.match(r"^\[\w{8}\] [IWE] ", l)]
        if len(tstamped) < 5:
            raise AssertionError("cat /proc/klog produced few timestamped lines (%d)"
                                 % len(tstamped))
        if not any("GDT" in l or "PMM" in l for l in tstamped):
            raise AssertionError("klog missing a boot line (GDT/PMM)")
        if not any("klog: ready" in l for l in tstamped):
            raise AssertionError("klog missing the 'klog: ready' INFO line")
    print("PASS: /proc/klog shows timestamped INFO boot lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
