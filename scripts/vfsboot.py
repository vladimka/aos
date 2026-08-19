#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-vfsboot.sock"
SER = "/tmp/aos-vfsboot-ser.sock"

BOOT_MARKS = [
    "VFS: / [sfs2] root=1",
    "VFS: /proc [procfs]",
    "Loading programs... done",
    "Filesystem ready.",
]

def wait_for(path, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path): return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)

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

        log = b""
        end = time.time() + 40
        while b"AOS>" not in log and b"KERNEL PANIC" in (b"") or b"AOS>" not in log:
            if time.time() > end:
                raise AssertionError("boot timeout; log tail:\n" + log[-400:].decode(errors="replace"))
            try:
                d = s.recv(4096)
                if not d: break
                log += d
            except socket.timeout:
                pass
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + log[-400:].decode(errors="replace"))
        text = log.decode(errors="replace")
        for mark in BOOT_MARKS:
            if mark not in text:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, text[-400:]))
        print("PASS: VFS mounts + embedded payload seeded (boot marks OK)")

        s.settimeout(1)
        s.sendall(b"ls\n")
        out = b""
        end = time.time() + 20
        while time.time() < end:
            if b"demo.ico" in out and (b"KERNEL PANIC" in out or b"AOS>" in out):
                break
            try:
                d = s.recv(4096)
                if not d: break
                out += d
            except socket.timeout:
                pass
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during ls:\n" + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")
        for name in ("bin", "lin", "demo.ico", "sys"):
            if name not in otext:
                raise AssertionError("ls output missing %r:\n%s" % (name, otext))
        print("PASS: ls lists bin/lin/sys/demo.ico over VFS")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
