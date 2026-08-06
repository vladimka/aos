#!/usr/bin/env python3
"""Baseline boot smoke: ISO boots to a stable serial 'Terminal ready.' banner."""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
SER = "/tmp/aos-wmhello.log"

def wait_serial(needle, seconds=30, expect="Terminal ready."):
    end = time.time() + seconds
    log = ""
    while time.time() < end:
        time.sleep(0.5)
        try:
            with open(SER, "r", errors="replace") as f: log = f.read()
        except FileNotFoundError:
            continue
        if needle in log:
            if "KERNEL PANIC" in log:
                raise AssertionError("kernel panic during boot:\n" + log[-2000:])
            return log
    raise AssertionError("timed out waiting for %r; log tail:\n%s"
                         % (needle, log[-2000:]))

def main():
    try: os.unlink(SER)
    except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        log = wait_serial("Terminal ready.")
        tails = log.splitlines()[-12:]
        if any(k in l for l in tails for k in
               ("panic", "double fault", "General Protection", "Page Fault")):
            raise AssertionError("unexpected fault in boot tail:\n" + "\n".join(tails))
        print("PASS: baseline boot reached a stable shell")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())