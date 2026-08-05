#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-cwdtest.sock"
SER = "/tmp/aos-cwdtest.sock"

BOOT_MARKS = [
    "VFS: / [sfs2] root=1",
    "Filesystem ready.",
    "AOS>",
]

def wait_for(path, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path): return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)

def drain(s, log, end, needle=b""):
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: break
            log += d
            if needle and needle in log:
                break
        except socket.timeout:
            pass
    return log

def main():
    for path in (MON, SER):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "unix:" + SER + ",server,nowait",
        "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(SER)
        time.sleep(0.5)              # connect before the boot log is emitted
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        # The WM registers for events shortly after the prompt and then
        # captures all serial input (it is forwarded to the GUI mailbox, not
        # the terminal), so the whole command sequence must be queued the
        # moment AOS> appears. All commands here are shell builtins processed
        # inline in the timer IRQ except the final cat (a short program).
        log = drain(s, b"", time.time() + 40, b"AOS>")
        text = log.decode(errors="replace")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + text[-400:])
        for mark in BOOT_MARKS:
            if mark not in text:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, text[-400:]))
        print("PASS: boot OK")

        burst = (b"cd /proc\n"
                 b"pwd\n"
                 b"cd /\n"
                 b"pwd\n"
                 b"cd /bin\n"
                 b"pwd\n"
                 b"cd /\n"
                 b"cat /proc/uptime\n")
        s.sendall(burst)
        out = drain(s, b"", time.time() + 30, b"ticks")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during cwd commands:\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        failures = []
        # pwd after cd /proc must print /proc (as its own line).
        if b"\n/proc\n" not in out:
            failures.append("cd /proc; pwd did not print /proc")
        # pwd after cd / (root) must print a standalone "/" line.
        if b"\n/\n" not in out:
            failures.append("cd /; pwd did not print /")
        # pwd after cd /bin must print /bin.
        if b"\n/bin\n" not in out:
            failures.append("cd /bin; pwd did not print /bin")
        # cat /proc/uptime prints "uptime N ticks".
        if b"uptime" not in out or b"ticks" not in out:
            failures.append("cat /proc/uptime did not print 'uptime N ticks'")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-600:])
        print("PASS: cd/pwd (/, /bin, /proc) and cat /proc/uptime")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())