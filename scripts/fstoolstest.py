#!/usr/bin/env python3
"""E2E FS-tools test: cp/mv/mkdir/rmdir/head/wc via foreground redirects.

echo prints "<word>\n" (no leading blank line), so the redirect file for
three echos is "one\ntwo\nthree\n" -> wc reports "3 3 14". head truncates,
so its output is checked as a marker-bounded segment that must NOT contain
the omitted tail ("three"). New tool messages: cp/mv/mkdir/rmdir/rm write
errors to stderr; successful copies are silent (no "Copied:").
"""
import socket
import sys
import time

from qtest import QTest

CMDS = [
    "echo one > /t.txt",
    "echo two >> /t.txt",
    "echo three >> /t.txt",
    "wc /t.txt",                 # -> "3 3 14 /t.txt"
    "echo HEAD-MARK",
    "head -n 2 /t.txt",
    "echo HEAD-END",
    "cp /t.txt /t2.txt",
    "echo CAT-MARK",
    "cat /t2.txt",
    "echo CAT-END",
    "mv /t2.txt /t3.txt",
    "wc /t3.txt",                # -> "3 3 14 /t3.txt"
    "rm /t3.txt",
    "echo RM-MARK",
    "cat /t3.txt",               # -> "cat: /t3.txt: No such file or directory"
    "echo RM-END",
    "mkdir -p /d/sub",
    "echo LS-MARK",
    "ls /d",
    "echo LS-END",
    "rm -r /d",
    "echo fstools-done",
]


def main():
    with QTest("fstoolstest", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)
        out = b""
        for line in CMDS:
            s.sendall(line.encode() + b"\n")
            target = out.count(b"AOS> ") + 1
            end = time.time() + 20
            while time.time() < end and out.count(b"AOS> ") < target:
                try:
                    d = s.recv(4096)
                    if d:
                        out += d
                except socket.timeout:
                    pass
        out += q.serial_drain(s, timeout=15, needle=b"fstools-done")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during fs-tools commands:\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        def between(a, b):
            tail = otext.split(a, 1)[1] if a in otext else ""
            return tail.split(b, 1)[0] if b in tail else ""

        failures = []
        if "3 3 14 /t.txt" not in otext:
            failures.append("wc /t.txt did not report 3 3 14")
        head_seg = between("HEAD-MARK", "HEAD-END")
        if "one" not in head_seg:
            failures.append("head did not print the first two lines")
        if "three" in head_seg:
            failures.append("head printed more than 2 lines (no truncation)")
        cat_seg = between("CAT-MARK", "CAT-END")
        if "three" not in cat_seg:
            failures.append("cat /t2.txt did not show the full copy")
        if "3 3 14 /t3.txt" not in otext:
            failures.append("wc /t3.txt after mv failed")
        rm_seg = between("RM-MARK", "RM-END")
        if "No such file or directory" not in rm_seg:
            failures.append("rm did not remove /t3.txt (cat should have errored)")
        ls_seg = between("LS-MARK", "LS-END")
        if "sub" not in ls_seg:
            failures.append("ls /d did not show sub")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
    print("PASS: cp, mv, mkdir, rmdir, head, wc, redirects")
    return 0


if __name__ == "__main__":
    sys.exit(main())
