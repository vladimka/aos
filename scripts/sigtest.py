#!/usr/bin/env python3
"""sigtest: E2E regression for Stage-3 Linux signal syscalls.

Boots in TEXT mode (console-only) so program stdout reliably returns over
serial, then runs lin/sigtest, which verifies:
  - rt_sigaction (174): register a SIGUSR1 handler
  - tkill (238) / raise: synchronous self-signal delivery + handler frame
  - rt_sigreturn (173): restore context after the handler returns
  - rt_sigprocmask (175): SIGUSR2 blocked (stays pending), then delivered
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
    with QTest("sigtest", serial_mode="socket", autoboot_grub=False) as q:
        s = q.serial_socket()
        boot_text_entry(q, s)

        s.sendall(b"lin/sigtest\r")
        resp = b""
        deadline = time.time() + 60
        while time.time() < deadline:
            resp += q.serial_drain(s, timeout=2, needle=b"SIGTEST OK")
            if b"SIGTEST OK" in resp or b"SIGTEST FAIL" in resp or \
               b"KERNEL PANIC" in resp:
                break
        resp += q.serial_drain(s, timeout=3)
        if b"KERNEL PANIC" in resp:
            raise AssertionError("kernel panic in lin/sigtest:\n"
                                 + resp[-500:].decode(errors="replace"))
        if b"SIGTEST FAIL" in resp or b"FAIL " in resp:
            raise AssertionError("lin/sigtest reported failures:\n"
                                 + resp[-900:].decode(errors="replace"))
        if b"SIGTEST OK" not in resp:
            raise AssertionError("lin/sigtest missing SIGTEST OK; out:\n"
                                 + resp[-900:].decode(errors="replace"))
        print("PASS: lin/sigtest (rt_sigaction + tkill + sigprocmask)")
        return 0


if __name__ == "__main__":
    sys.exit(main())
