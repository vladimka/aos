#!/usr/bin/env python3
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-virtiotest.sock"
SER = "/tmp/aos-virtiotest.log"
IMG = "/tmp/aos-virtiotest.img"


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


def boot(expect_new_disk):
    for path in (MON, SER):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
        "-drive", "file=" + IMG + ",format=raw,if=none,id=d0",
        "-device", "virtio-blk-pci,disable-modern=on,drive=d0",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        tag = "SFS formatting new disk." if expect_new_disk else "SFS mounted from disk."
        if not wait_for_serial(tag):
            raise AssertionError("expected serial %r" % tag)
        if not wait_for_serial("Terminal ready."):
            raise AssertionError("kernel did not finish booting "
                                 "(format flush incomplete)")
        log = open(SER, "r", errors="replace").read()
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during virtio boot")
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()


def main():
    try:
        os.unlink(IMG)
    except FileNotFoundError:
        pass
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)
    boot(expect_new_disk=True)
    data = open(IMG, "rb").read()
    if data[:4] != b"SFS1":
        raise AssertionError("disk image does not start with SFS1 after boot A")
    if b"bin/help" not in data:
        raise AssertionError("embedded program name not flushed to disk")
    boot(expect_new_disk=False)
    print("PASS: SFS persists on the virtio disk across reboots")
    return 0


if __name__ == "__main__":
    sys.exit(main())
