#!/usr/bin/env python3
import sys
import time

from qtest import QTest


def main():
    with QTest("strace-live", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)

        s.sendall(b"strace bgspawn\n")
        out = q.serial_drain(s, timeout=60, needle=b"== pid 2 ==")
        end = time.time() + 30
        while time.time() < end and out.count(b"== pid 2 ==") < 2:
            out = q.serial_drain(s, timeout=30)
        tail = out[-1500:]
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during strace bgspawn:\n"
                                 + tail.decode(errors="replace"))
        if b"== pid 0 ==" not in out:
            raise AssertionError("strace session did not dump == pid 0 ==\n"
                                 + (tail.decode(errors="replace") if isinstance(tail, bytes) else tail))
        if b"== pid 2 ==" not in out:
            raise AssertionError("strace did not collect the live clock child (pid 2)\n"
                                 + (tail.decode(errors="replace") if isinstance(tail, bytes) else tail))
        for probe in ("fill(", "text(", "send("):
            if probe.encode() not in out:
                raise AssertionError("live trace missing %r\n%s" % (probe, tail))
    print("PASS: /proc/2/trace + dump show live AOS_EXT fill/text/send of the running clock")
    return 0


if __name__ == "__main__":
    sys.exit(main())
