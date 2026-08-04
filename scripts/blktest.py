#!/usr/bin/env python3
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-blktest.sock"
SER = "/tmp/aos-blktest.log"
IMG = "/tmp/aos-blktest.img"


def wait_for(path, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)


def wait_for_serial(text, seconds=25):
    end = time.time() + seconds
    while time.time() < end:
        try:
            with open(SER, "r", errors="replace") as f:
                if text in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    return False


def main():
    for path in (MON, SER):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
        "-drive", "file=" + IMG + ",format=raw,if=none,id=d0",
        "-device", "virtio-blk-pci,disable-modern=on,drive=d0",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        if not wait_for_serial("blk: selftest OK"):
            raise AssertionError("vblk selftest did not report OK")
        log = open(SER, "r", errors="replace").read()
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during vblk boot")
        if "blk: selftest FAIL" in log:
            raise AssertionError("vblk selftest reported FAIL")
        print("PASS: virtio-blk reads and writes sectors")
        return 0
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
