#!/usr/bin/env python3
import re
import sys

from qtest import QTest


def main():
    with QTest("strace-live", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)

        s.sendall(b"strace bgspawn\n")
        out = q.serial_drain(s, timeout=90, needle=b"AOS>")
        tail = out[-1500:]
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during strace bgspawn:\n"
                                 + tail.decode(errors="replace"))
        headers = set(re.findall(rb"== pid (\d+) ==", out))
        if len(headers) < 2:
            raise AssertionError("strace did not collect bgspawn + its clock child\n"
                                 "(saw %r in the dump)\n%s"
                                 % ([l for l in out.split(b"\n") if b"==" in l and b"pid" in l],
                                    tail.decode(errors="replace")))
        for probe in ("fill(", "text(", "send("):
            if probe.encode() not in out:
                raise AssertionError("live trace missing %r\n%s"
                                     % (probe, tail))
    print("PASS: /proc/<pid>/trace shows live AOS_EXT fill/text/send of the running clock")
    return 0


if __name__ == "__main__":
    sys.exit(main())
