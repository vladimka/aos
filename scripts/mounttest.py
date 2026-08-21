#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-mounttest.sock"
SER = "/tmp/aos-mounttest.sock"

BOOT_MARKS = [
    "VFS: / [sfs2] root=1",
    "VFS: /proc [procfs]",
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

        # Queue the whole command burst the moment AOS> appears: after the WM
        # spawns it captures all serial input into the GUI mailbox.
        log = drain(s, b"", time.time() + 40, b"AOS>")
        text = log.decode(errors="replace")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + text[-400:])
        for mark in BOOT_MARKS:
            if mark not in text:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, text[-400:]))
        print("PASS: boot OK")

        # The WM captures serial input once it registers for events, and while
        # a user program runs serial is diverted to its key queue, so only the
        # final program of an early burst reaches the shell. `procinfo` reads
        # /proc/uptime, /proc/version, /proc/mounts and lists /proc in one run.
        s.sendall(b"procinfo -a\n")
        out = drain(s, b"", time.time() + 30, b"PROCINFO PASS")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during procfs commands:\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        for needle in (b"\n[uptime]", b"ticks",
                       b"\n[version]", b"AOS",
                       b"\n[mounts]", b"rootfs", b"/proc",
                       b"\n[list]", b"uptime", b"version", b"mounts",
                       b"PROCINFO PASS"):
            if needle not in out:
                raise AssertionError("serial missing %r;\nout:\n%s"
                                     % (needle, otext[-600:]))
        print("PASS: /proc uptime/version/mounts readable and listed")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())