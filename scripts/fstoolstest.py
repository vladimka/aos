#!/usr/bin/env python3
"""E2E FS-tools test: cp/mv/mkdir/rmdir/head/wc via foreground redirects."""
import sys

from qtest import QTest


def main():
    with QTest("fstoolstest", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)

        burst = (b"echo one > /t.txt\n"
                 b"echo two >> /t.txt\n"
                 b"echo three >> /t.txt\n"
                 b"wc /t.txt\n"
                 b"head /t.txt 1\n"
                 b"cp /t.txt /t2.txt\n"
                 b"cat /t2.txt\n"
                 b"mv /t2.txt /t3.txt\n"
                 b"cat /t3.txt\n"
                 b"rm /t3.txt\n"
                 b"cat /t3.txt\n"
                 b"mkdir /d\n"
                 b"rmdir /d\n"
                 b"echo fstools-done\n")
        s.sendall(burst)
        out = q.serial_drain(s, timeout=30, needle=b"fstools-done")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during fs-tools commands:\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        failures = []
        if "\n3 3 14 /t.txt\n" not in otext:
            failures.append("wc counted wrong lines/words/bytes")
        if otext.count("\none\n") < 2:   # head + cat /t2.txt (cat /t3.txt даёт 3-ю)
            failures.append("head did not print first line (or cat lost it)")
        if "Copied: /t.txt -> /t2.txt" not in otext:
            failures.append("cp failed")
        if "\nthree\n" not in otext:
            failures.append("cat /t2.txt did not show full copy")
        if "Moved: /t2.txt -> /t3.txt" not in otext:
            failures.append("mv failed")
        if "\n3 3 14 /t3.txt\n" not in otext:
            failures.append("wc /t3.txt after mv failed")
        if "File not found: /t3.txt" not in otext:
            failures.append("rm did not remove /t3.txt")
        if "Created: /d" not in otext:
            failures.append("mkdir failed")
        if "Removed: /d" not in otext:
            failures.append("rmdir failed")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
    print("PASS: cp, mv, mkdir, rmdir, head, wc, redirects")
    return 0


if __name__ == "__main__":
    sys.exit(main())
