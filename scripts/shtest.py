#!/usr/bin/env python3
"""E2E smoke test for the userland shell bin/sh (Task 4).

Boots the ISO headless and drives bin/sh over the serial console: prompt,
builtins (pwd), PATH lookup (ls /bin), $? status, unknown-command error,
redirection (echo > f, cat f), pipelines (ls | lin/cat, 3-stage with a
Linux reader), background jobs (&), and exit back to the kernel shell.

NOTE: bin/sh's line editor executes a line on Enter = 0x0D (\\r); a \\n
(0x0A) byte is dropped. The serial path pushes bytes raw to the key queue
while a user program runs, so every line must be sent with \\r.

NOTE: bin/sh's line editor executes a line on Enter = 0x0D (\\r); a \\n
(0x0A) byte is dropped. The serial path pushes bytes raw to the key queue
while a user program runs, so every line must be sent with \\r.
"""
import socket
import sys
import time

from qtest import QTest

BOOT_MARKS = [
    "VFS: / [sfs2] root=1",
    "Filesystem ready.",
    "AOS>",
]


def drain_quiet(s, timeout=12, settle=1.0):
    """Drain *s* until ~*settle* seconds of silence (or *timeout* elapses).

    Returns the accumulated bytes. Relies on the QTest socket timeout (1 s).
    """
    out = b""
    end = time.time() + timeout
    last = time.time()
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d:
                break
            out += d
            last = time.time()
        except socket.timeout:
            if time.time() - last >= settle:
                break
    return out


def cmd(s, line):
    """Send one line (Enter = \\r) to the active shell and drain to quiet."""
    s.sendall(line.encode() + b"\r")
    out = drain_quiet(s)
    if b"KERNEL PANIC" in out:
        raise AssertionError("kernel panic after %r:\n%s"
                             % (line, out[-400:].decode(errors="replace")))
    return out


def main():
    with QTest("shtest", serial_mode="socket") as q:
        s = q.serial_socket()
        log = q.boot_and_ready(socket=s) or b""
        text = log.decode(errors="replace")
        for mark in BOOT_MARKS:
            if mark not in text:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, text[-400:]))
        print("PASS: boot OK")

        out = cmd(s, "bin/sh")
        log += out
        if b"AOS> " not in out:
            raise AssertionError("bin/sh did not print its prompt; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: bin/sh prints prompt")

        out = cmd(s, "pwd")
        log += out
        if b"/\r\n" not in out:
            raise AssertionError("pwd did not print the cwd; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: pwd prints cwd")

        out = cmd(s, "ls /bin")
        log += out
        if b"Files in /bin" not in out or b"sh (" not in out:
            raise AssertionError("ls /bin did not list sh; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: ls /bin lists sh")

        out = cmd(s, "echo $?")
        log += out
        if b"\n0\n" not in out:
            raise AssertionError("$? != 0 after success; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: $? == 0 after success")

        out = cmd(s, "notacmd")
        log += out
        if b"Unknown command" not in out:
            raise AssertionError("notacmd not reported as unknown; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: unknown command reported")

        out = cmd(s, "echo $?")
        log += out
        if b"\n127\n" not in out:
            raise AssertionError("$? != 127 after unknown command; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: $? == 127 after unknown command")

        # ---- Task 4: redirects ----
        out = cmd(s, "echo hi > f")
        log += out
        out = cmd(s, "cat f")
        log += out
        if b"hi" not in out:
            raise AssertionError("cat f did not print the redirected content; "
                                 "out:\n%s" % out[-400:].decode(errors="replace"))
        print("PASS: redirect echo hi > f then cat f")

        # ---- Task 4: pipelines (AOS writer -> Linux reader) ----
        out = cmd(s, "ls /bin | lin/cat")
        log += out
        if b"Files in /bin" not in out or b"sh (" not in out:
            raise AssertionError("ls /bin | lin/cat did not list sh; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: pipeline ls /bin | lin/cat lists sh")

        out = cmd(s, "bin/ls / | lin/cat | lin/cat")
        log += out
        if b"Files in /:" not in out:
            raise AssertionError("3-stage pipeline did not list /; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: 3-stage pipeline bin/ls / | lin/cat | lin/cat")

        out = cmd(s, "echo $?")
        log += out
        if b"\n0\n" not in out:
            raise AssertionError("$? != 0 after successful pipeline; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: $? == 0 after successful pipeline")

        # ---- Task 4: background job ----
        out = cmd(s, "ls /bin &")
        log += out
        if b"bg: pid " not in out:
            raise AssertionError("ls /bin & did not print 'bg: pid'; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: background ls /bin & prints bg: pid")

        out = cmd(s, "exit")
        log += out
        if b"\nAOS> " not in out:
            raise AssertionError("no kernel-shell prompt after exit; out:\n%s"
                                 % out[-400:].decode(errors="replace"))
        print("PASS: exit returns to the kernel shell")

        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic in serial log:\n"
                                 + log[-400:].decode(errors="replace"))
    print("PASS: bin/sh smoke")
    return 0


if __name__ == "__main__":
    sys.exit(main())
