#!/usr/bin/env python3
import os
import subprocess
import sys

from qtest import QTest

IMG = "/tmp/aos-atatest.img"


def main():
    try:
        os.unlink(IMG)
    except FileNotFoundError:
        pass
    subprocess.run(["truncate", "-s", "5G", IMG], check=True)
    extra = [
        "-drive", "file=" + IMG + ",format=raw,if=ide",
    ]
    with QTest("atatest", extra_args=extra) as q:
        q.boot_and_ready()
        log = q.serial_read()
        if "ata: found" not in log:
            raise AssertionError("ATA drive was not detected")
        if "ata: selftest FAIL" in log:
            raise AssertionError("ATA selftest reported FAIL")
        if "ata: selftest OK" not in log:
            raise AssertionError("ATA selftest did not report OK")
        if "ata: selftest multi OK" not in log:
            raise AssertionError("ATA multi-sector selftest did not report OK")
        if "block: ata backend" not in log:
            raise AssertionError("block layer did not select ATA backend")
        if "block: ata backend, 10485760 sectors" not in log:
            raise AssertionError("ATA backend did not report full 5 GiB capacity")
        if "SFS2 mounted (disk)" not in log and "SFS2 formatting new disk" not in log:
            raise AssertionError("SFS2 did not mount from ATA disk")
    print("PASS: ATA drive detected via IDENTIFY")
    return 0


if __name__ == "__main__":
    sys.exit(main())
