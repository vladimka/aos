#!/usr/bin/env python3
"""E2E shell test: env vars ($VAR), exit status ($?), background jobs (&)."""
import sys

from qtest import QTest


def main():
    with QTest("shelltest", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)

        burst = (b"export AOSHOME=/home/test\n"
                 b"echo $AOSHOME\n"
                 b"lin/hello\n"
                 b"echo $?\n"
                 b"exitto\n"
                 b"echo $?\n"
                 b"echo bgtag > /bg.txt &\n"
                 b"cat /bg.txt\n"
                 b"echo done\n"
                 b"cat /bg.txt\n"
                 b"uptime &\n"
                 b"echo $?\n")
        s.sendall(burst)
        out = q.serial_drain(s, timeout=30, needle=b"Uptime:")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during shell commands:\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        failures = []
        if "\n/home/test\n" not in otext:
            failures.append("echo $AOSHOME did not print /home/test")
        if "\n0\n" not in otext:
            failures.append("echo $? after lin/hello did not print 0")
        if "\n7\n" not in otext:
            failures.append("echo $? after exitto did not print 7")
        if "bg: pid" not in otext:
            failures.append("uptime & did not print 'bg: pid'")
        if "Uptime:" not in otext:
            failures.append("background uptime output missing")
        if "bgtag" not in otext:
            failures.append("cat /bg.txt did not show the bg-redirected file")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
    print("PASS: env expansion, $?, background jobs, bg redirect")
    return 0


if __name__ == "__main__":
    sys.exit(main())
