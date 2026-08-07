#!/usr/bin/env python3
"""E2E backtrace-symbol test: run `panic`, assert the serial log shows a
kernel panic and at least one resolved frame ("name+0x..")."""
import re
import sys

from qtest import QTest


def main():
    with QTest("panictest", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)

        s.sendall(b"panic\n")
        out = q.serial_drain(s, timeout=20, needle=b"--- end ---")

        failures = []
        if b"KERNEL PANIC" not in out:
            failures.append("KERNEL PANIC banner missing")
        if b"--- backtrace ---" not in out or b"--- end ---" not in out:
            failures.append("backtrace block missing")
        else:
            frame_re = re.compile(r"eip=0x([0-9a-f]+)\s+(\w+)\+0x([0-9a-f]+)")
            frames = []
            for line in out.decode(errors="replace").splitlines():
                m = frame_re.search(line)
                if m:
                    addr = int(m.group(1), 16)
                    if 0x100000 <= addr < 0x400000:
                        frames.append(line)
            if not frames:
                failures.append("no resolved (name+offset) kernel-text backtrace frame")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n"
                                 + out[-800:].decode(errors="replace"))
    print("PASS: panic backtrace resolves eip to symbol names")
    return 0


if __name__ == "__main__":
    sys.exit(main())
