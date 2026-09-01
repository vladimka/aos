#!/usr/bin/env python3
# socktest: boots the AOS ISO headless (serial-only) and runs lin/socktest,
# the AF_UNIX echo smoke test (socket/bind/listen/connect/accept/send/recv).
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-socktest.sock"
SER = "/tmp/aos-socktest.serial"

BOOT_MARKS = [
    "socket: AF_UNIX ready",
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
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        m.connect(MON)
        for _ in range(3):
            time.sleep(1.5)
            m.sendall(b"sendkey ret\n")
        m.close()

        log = drain(s, b"", time.time() + 40, b"AOS>")
        text = log.decode(errors="replace")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + text[-400:])
        for mark in BOOT_MARKS:
            if mark not in text:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, text[-400:]))
        print("PASS: boot OK (AF_UNIX ready)")

        # The WM/sh registers events shortly after the kernel-shell AOS> prompt
        # and then captures serial input for its own line editor, so the
        # command must be queued the instant AOS> appears (same as pipetest).
        s.sendall(b"lin/socktest\n")
        out = drain(s, b"", time.time() + 40, b"AOS>")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic in lin/socktest:\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")
        if b"SOCKTEST OK" not in out:
            raise AssertionError("lin/socktest did not print SOCKTEST OK; out:\n"
                                 + otext[-800:])
        print("PASS: lin/socktest (AF_UNIX echo)")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
