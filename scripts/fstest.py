#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-fstest.sock"
SER = "/tmp/aos-fstest-ser.sock"

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

        # The WM registers for events shortly after the prompt and then steals
        # serial input (it is forwarded to the GUI mailbox instead of the
        # terminal), so the command must be typed the moment AOS> appears.
        log = drain(s, b"", time.time() + 40, b"AOS>")
        text = log.decode(errors="replace")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + text[-400:])
        for mark in BOOT_MARKS:
            if mark not in text:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, text[-400:]))
        print("PASS: boot OK")

        s.sendall(b"fstest\n")
        out = drain(s, b"", time.time() + 30, b"FSTEST PASS")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during fstest:\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")
        if b"FSTEST PASS" not in out:
            raise AssertionError("fstest did not pass; output:\n" + otext[-600:])
        if b"FSTEST FAIL" in out:
            raise AssertionError("fstest reported FAIL; output:\n" + otext[-600:])
        print("PASS: fstest (mkdir -p, write/read, lseek, readdir, stat, unlink, rmdir)")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
