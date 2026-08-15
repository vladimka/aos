#!/usr/bin/env python3
import os
import subprocess
import sys

from qtest import QTest

IMG = "/tmp/aos-ahcitest.img"


def main():
    try:
        os.unlink(IMG)
    except FileNotFoundError:
        pass
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)
    extra = [
        "-drive", "file=" + IMG + ",format=raw,if=none,id=d0",
        "-device", "ich9-ahci,id=ahci",
        "-device", "ide-hd,drive=d0,bus=ahci.0",
    ]
    with QTest("ahcitest", extra_args=extra) as q:
        q.boot_and_ready()
        log = q.serial_read()
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during AHCI boot")
        if "ahci: found" not in log:
            raise AssertionError("AHCI controller/disk was not detected")
        if "ahci: selftest FAIL" in log:
            raise AssertionError("AHCI selftest reported FAIL")
        if "ahci: selftest OK" not in log:
            raise AssertionError("AHCI selftest did not report OK")
        if "ahci: selftest multi OK" not in log:
            raise AssertionError("AHCI multi-sector selftest did not report OK")
        if "block: ahci backend, 8192 sectors" not in log:
            raise AssertionError("block layer did not select AHCI backend with full capacity")
        if "SFS2 mounted (disk)" not in log and "SFS2 formatting new disk" not in log:
            raise AssertionError("SFS2 did not mount from the AHCI disk")
    print("PASS: AHCI/SATA reads and writes sectors")
    return 0


if __name__ == "__main__":
    sys.exit(main())