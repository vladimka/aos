#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-rngtest.sock"
SER = "/tmp/aos-rngtest.log"

def wait_for(path, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path): return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)

def wait_for_serial(text, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        try:
            with open(SER, "r", errors="replace") as f: log = f.read()
            if text in log: return True
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    return False

def main():
    for path in (MON, SER):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
        "-device", "virtio-rng-pci,disable-modern=on",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        if not wait_for_serial("rng: selftest OK"):
            raise AssertionError("rng selftest did not report OK")
        log = open(SER, "r", errors="replace").read()
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during rng boot")
        if "rng: selftest FAIL" in log:
            raise AssertionError("rng selftest reported FAIL")
        print("PASS: virtio-rng provides entropy")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
