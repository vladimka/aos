#!/usr/bin/env python3
import sys

from qtest import QTest


def main():
    with QTest("rngtest", extra_args=["-device", "virtio-rng-pci,disable-modern=on"]) as q:
        q.boot_and_ready()
        log = q.serial_read()
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during rng boot")
        if "rng: selftest FAIL" in log:
            raise AssertionError("rng selftest reported FAIL")
        if "rng: selftest OK" not in log:
            raise AssertionError("rng selftest did not report OK")
    print("PASS: virtio-rng provides entropy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
