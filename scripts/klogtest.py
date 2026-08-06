#!/usr/bin/env python3
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-klog.sock"
SER = "/tmp/aos-klog.sock"

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

        # The WM registers for events shortly after the prompt and then owns all
        # serial input, so `cat /proc/klog` must be queued the moment AOS> shows.
        log = drain(s, b"", time.time() + 40, b"AOS>")
        text = log.decode(errors="replace")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + text[-400:])
        for mark in BOOT_MARKS:
            if mark not in text:
                raise AssertionError("serial log missing %r; tail:\n%s"
                                     % (mark, text[-400:]))

        s.sendall(b"cat /proc/klog\n")
        out = drain(s, b"", time.time() + 30, b"klog: ready")
        otext = out.decode(errors="replace")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during cat /proc/klog:\n"
                                 + otext[-400:])

        lines = otext.splitlines()
        tstamped = [l for l in lines if re.match(r"^\[\w{8}\] [IWE] ", l)]
        if len(tstamped) < 5:
            raise AssertionError("cat /proc/klog produced few timestamped lines (%d)"
                                 % len(tstamped))
        if not any("GDT" in l or "PMM" in l for l in tstamped):
            raise AssertionError("klog missing a boot line (GDT/PMM)")
        if not any("klog: ready" in l for l in tstamped):
            raise AssertionError("klog missing the 'klog: ready' INFO line")
        print("PASS: /proc/klog shows timestamped INFO boot lines")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
