#!/usr/bin/env python3
"""forktest: E2E test for AOS Linux fork(2) (syscall 2) + dup2(2) (syscall 63).

Boots in TEXT mode (console-only, no WM/sh fighting for serial) so program
stdout reliably returns over serial, then runs lin/forktest.

lin/forktest covers:
  1. dup2 : pipe[p1,p2], dup2(p1 -> 7), write(7,"Z"), read(p2) == 'Z'
  2. fork : returns 0 in the child, the child's pid in the parent
  3. both processes run independently (separate getpid) and print markers
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
    with QTest("forktest", serial_mode="socket", autoboot_grub=False) as q:
        s = q.serial_socket()
        boot_text_entry(q, s)

        s.sendall(b"lin/forktest\r")
        # The child is a separately-scheduled task: its "CHILD pid=" line may
        # arrive AFTER the parent finished and printed FORKTEST OK. Drain until
        # we have seen the child's marker (and the OK), not just the first OK.
        resp = b""
        deadline = time.time() + 40
        while time.time() < deadline:
            chunk = q.serial_drain(s, timeout=2, needle=b"CHILD pid=")
            resp += chunk
            if b"CHILD pid=" in resp and b"FORKTEST OK" in resp:
                break
        resp += q.serial_drain(s, timeout=3)
        if b"KERNEL PANIC" in resp:
            raise AssertionError("kernel panic in lin/forktest:\n"
                                 + resp[-500:].decode(errors="replace"))

        need = [b"dup2: OK", b"CHILD pid=", b"PARENT pid=", b"FORKTEST OK"]
        for nd in need:
            if nd not in resp:
                raise AssertionError(
                    "lin/forktest missing %r; out:\n%s"
                    % (nd, resp[-900:].decode(errors="replace")))
        if b"FORKTEST FAIL" in resp:
            raise AssertionError("lin/forktest reported failures:\n"
                                 + resp[-500:].decode(errors="replace"))
        print("PASS: lin/forktest (fork 2 + dup2 63 + getppid 64)")
        return 0


if __name__ == "__main__":
    sys.exit(main())
