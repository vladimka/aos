#!/usr/bin/env python3
import os
import subprocess
import sys

from qtest import QTest

IMG = "/tmp/aos-virtiotest.img"


def boot_and_check(q, expect_new_disk):
    q.boot_and_ready()
    tag = "SFS2 formatting new disk." if expect_new_disk else "SFS2 mounted (disk)."
    if not q.serial_wait(tag):
        raise AssertionError("expected serial %r" % tag)
    if not q.serial_wait("Terminal ready."):
        raise AssertionError("kernel did not finish booting (format flush incomplete)")
    log = q.serial_read()
    if "KERNEL PANIC" in log:
        raise AssertionError("kernel panic during virtio boot")


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
    # Boot A: new disk
    with QTest("virtiotest", extra_args=extra) as q:
        boot_and_check(q, expect_new_disk=True)
    data = open(IMG, "rb").read()
    if data[:4] != b"SFS2":
        raise AssertionError("disk image does not start with SFS2 after boot A")
    if b"help" not in data:
        raise AssertionError("embedded program name not flushed to disk")
    # Boot B: existing disk
    with QTest("virtiotest", extra_args=extra, boot_wait=6) as q:
        boot_and_check(q, expect_new_disk=False)
    print("PASS: SFS persists on the virtio disk across reboots")
    return 0


if __name__ == "__main__":
    sys.exit(main())
