#!/usr/bin/env python3
import os
import subprocess
import sys

from qtest import QTest

IMG = "/tmp/aos-blktest.img"


def main():
    try:
        os.unlink(IMG)
    except FileNotFoundError:
        pass
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)
    extra = [
        "-drive", "file=" + IMG + ",format=raw,if=none,id=d0",
        "-device", "virtio-blk-pci,disable-modern=on,drive=d0",
    ]
    with QTest("blktest", extra_args=extra) as q:
        q.boot_and_ready()
        log = q.serial_read()
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during vblk boot")
        if "blk: selftest FAIL" in log:
            raise AssertionError("vblk selftest reported FAIL")
        if "blk: selftest OK" not in log:
            raise AssertionError("vblk selftest did not report OK")
        if "blk: selftest multi OK" not in log:
            raise AssertionError("vblk multi-sector selftest did not report OK")
    print("PASS: virtio-blk reads and writes sectors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
