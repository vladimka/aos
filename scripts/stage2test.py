#!/usr/bin/env python3
"""stage2test: E2E regression for Stage-2 Linux syscalls.

Boots in TEXT mode (console-only) so program stdout reliably returns over
serial, then runs lin/stage2test, which verifies:
  - wait4 (114): specific-pid reap, (exit_code<<8) status layout, WNOHANG
  - socketpair (360): bidirectional write/read over one shared stream
  - setsid (66) / getpgrp / getpgid / getsid: return own pid
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
    with QTest("stage2test", serial_mode="socket", autoboot_grub=False) as q:
        s = q.serial_socket()
        boot_text_entry(q, s)

        s.sendall(b"lin/stage2test\r")
        resp = b""
        deadline = time.time() + 60
        while time.time() < deadline:
            resp += q.serial_drain(s, timeout=2, needle=b"STAGE2 OK")
            if b"STAGE2 OK" in resp or b"STAGE2 FAIL" in resp or \
               b"KERNEL PANIC" in resp:
                break
        resp += q.serial_drain(s, timeout=3)
        if b"KERNEL PANIC" in resp:
            raise AssertionError("kernel panic in lin/stage2test:\n"
                                 + resp[-500:].decode(errors="replace"))
        if b"STAGE2 FAIL" in resp or b"FAIL " in resp:
            raise AssertionError("lin/stage2test reported failures:\n"
                                 + resp[-900:].decode(errors="replace"))
        if b"STAGE2 OK" not in resp:
            raise AssertionError("lin/stage2test missing STAGE2 OK; out:\n"
                                 + resp[-900:].decode(errors="replace"))
        print("PASS: lin/stage2test (wait4 + socketpair + setsid)")
        return 0


if __name__ == "__main__":
    sys.exit(main())
