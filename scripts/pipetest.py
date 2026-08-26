#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-pipetest.sock"
SER = "/tmp/aos-pipetest.serial"

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
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        # Skip the 60-s GRUB menu: fire fallback Enters through the monitor
        # (the first ones land in SeaBIOS, one hits the menu).
        m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        m.connect(MON)
        for _ in range(3):
            time.sleep(1.5)
            m.sendall(b"sendkey ret\n")
        m.close()

        # The WM registers for events shortly after the prompt and then
        # captures serial input, so the whole command sequence must be
        # queued the moment AOS> appears.
        log = drain(s, b"", time.time() + 40, b"AOS>")
        text = log.decode(errors="replace")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + text[-400:])
        for mark in BOOT_MARKS:
            if mark not in text:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, text[-400:]))
        print("PASS: boot OK")

        # pipe() syscall test: musl falls back from pipe2 to pipe (42).
        # Drain past PIPETEST OK to the next shell prompt so lin/piptest has
        # fully exited before the next command is sent (serial bytes sent
        # while it still runs are diverted to the key queue and lost).
        s.sendall(b"lin/piptest\n")
        out = drain(s, b"", time.time() + 30, b"AOS>")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic in pipe():\n"
                                 + out[-400:].decode(errors="replace"))
        if b"PIPETEST OK" not in out:
            raise AssertionError("lin/piptest did not print PIPETEST OK; out:\n"
                                 + out[-600:].decode(errors="replace"))
        print("PASS: pipe() syscall (lin/piptest)")

        # Pipeline: AOS writer (ls /bin) -> Linux reader (lin/cat) through a
        # pipe. `ls /bin` lists the bin directory (contains uptime), so the
        # listing flowing through the pipe proves the writer->reader wiring.
        s.sendall(b"ls /bin | lin/cat\n")
        out = drain(s, b"", time.time() + 30, b"AOS>")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic in 'ls /bin | lin/cat':\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")
        if b"uptime" not in out:
            raise AssertionError("'ls /bin | lin/cat' did not print the bin "
                                 "list; out:\n" + otext[-600:])
        print("PASS: ls /bin | lin/cat")

        # Pipeline with a procfs source.
        s.sendall(b"cat /proc/uptime | lin/cat\n")
        out = drain(s, b"", time.time() + 30, b"AOS>")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic in 'cat /proc/uptime | lin/cat':\n"
                                 + out[-400:].decode(errors="replace"))
        if b"uptime" not in out or b"ticks" not in out:
            raise AssertionError("'cat /proc/uptime | lin/cat' missing output; "
                                 "out:\n" + out[-600:].decode(errors="replace"))
        print("PASS: cat /proc/uptime | lin/cat")

        # Stress: 20000 bytes through a 4096-byte pipe buffer. The writer
        # blocks when full; lin/cat drains until EOF.
        s.sendall(b"lin/piptest gen 20000 | lin/cat\n")
        out = drain(s, b"", time.time() + 60, b"AOS>")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic in pipe stress:\n"
                                 + out[-400:].decode(errors="replace"))
        if b"PIPETEST" in out:
            raise AssertionError("unexpected PIPETEST marker in stress output")
        print("PASS: lin/piptest gen 20000 | lin/cat (blocking writer)")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
