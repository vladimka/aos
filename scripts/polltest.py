#!/usr/bin/env python3
"""polltest: E2E test for the kernel poll(2) (syscall 168).

Boots in TEXT mode (console-only: no WM/sh to fight for serial) so the
program's stdout reliably returns over serial, then runs lin/polltest.

lin/polltest covers:
  1. poll(fd=1, POLLOUT, timeout=0) -> immediately ready
  2. poll(bogus fd, POLLIN) -> POLLNVAL (reported regardless of events mask)
  3. poll(empty pipe read end, timeout=0) -> not ready (0)
  4. poll(same pipe, timeout=300) -> 0 after the deadline (blocking path)
  5. poll(pipe write end, POLLOUT) -> ready
"""
import sys
import time

from qtest import QTest


def boot_text_entry(q, s):
    """Select the second GRUB entry ("AOS (text)") and wait for a prompt."""
    out = b""
    for _ in range(6):
        time.sleep(1.5)
        q.key("down")
        q.key("ret")
        out += q.serial_drain(s, timeout=3)
        if len(out.strip()) > 20:
            break
    out += q.serial_drain(s, timeout=60)
    if b"AOS>" not in out and b"@aos:" not in out:
        raise AssertionError("shell prompt missing after text-mode boot:\n"
                             + out[-400:].decode(errors="replace"))
    return out


def main():
    with QTest("polltest", serial_mode="socket", autoboot_grub=False) as q:
        s = q.serial_socket()
        boot_text_entry(q, s)

        # The text-mode serial console needs \r as the Enter terminator.
        s.sendall(b"lin/polltest\r")
        resp = q.serial_drain(s, timeout=40, needle=b"POLLTEST OK")
        if b"POLLTEST OK" not in resp:
            resp += q.serial_drain(s, timeout=10, needle=b"POLLTEST")
        if b"KERNEL PANIC" in resp:
            raise AssertionError("kernel panic in lin/polltest:\n"
                                 + resp[-500:].decode(errors="replace"))
        if b"POLLTEST OK" not in resp:
            raise AssertionError("lin/polltest did not print POLLTEST OK; out:\n"
                                 + resp[-800:].decode(errors="replace"))
        if b"FAIL" in resp:
            raise AssertionError("lin/polltest reported failures:\n"
                                 + resp[-500:].decode(errors="replace"))
        print("PASS: lin/polltest (poll syscall 168)")
        return 0


if __name__ == "__main__":
    sys.exit(main())
