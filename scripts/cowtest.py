#!/usr/bin/env python3
"""cowtest: E2E test for AOS copy-on-write fork isolation.

Boots in TEXT mode (console-only) so program stdout reliably returns over
serial, then runs lin/cowtest.

lin/cowtest forks; the child and parent each write a distinct value to a
shared global and verify the other side's write never leaks in (COW isolation).
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
    with QTest("cowtest", serial_mode="socket", autoboot_grub=False) as q:
        s = q.serial_socket()
        boot_text_entry(q, s)

        s.sendall(b"lin/cowtest\r")
        # Both processes run: wait for CCHILD + CPARENT markers and the OK.
        resp = b""
        deadline = time.time() + 40
        while time.time() < deadline:
            resp += q.serial_drain(s, timeout=2, needle=b"CHILD g final")
            if b"CHILD g final" in resp and b"COWTEST OK" in resp:
                break
        resp += q.serial_drain(s, timeout=3)
        if b"KERNEL PANIC" in resp:
            raise AssertionError("kernel panic in lin/cowtest:\n"
                                 + resp[-500:].decode(errors="replace"))

        need = [b"CHILD g final", b"PARENT g final", b"COWTEST OK"]
        for nd in need:
            if nd not in resp:
                raise AssertionError(
                    "lin/cowtest missing %r; out:\n%s"
                    % (nd, resp[-900:].decode(errors="replace")))
        if b"COWTEST FAIL" in resp:
            raise AssertionError("lin/cowtest reported failures:\n"
                                 + resp[-500:].decode(errors="replace"))
        print("PASS: lin/cowtest (copy-on-write fork isolation)")
        return 0


if __name__ == "__main__":
    sys.exit(main())
