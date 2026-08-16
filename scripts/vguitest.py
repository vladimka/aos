#!/usr/bin/env python3
"""Boot the ISO with virtio-vga and assert the virtio-gpu driver initializes."""
import os, sys, time, re
sys.path.insert(0, os.path.dirname(__file__))
from qtest import QTest, ROOT

ISO = os.path.join(ROOT, "aos.iso")

def main():
    with QTest("vgu", serial_mode="file") as q:
        q.start(extra_args=["-vga", "none", "-device", "virtio-vga,disable-modern=on"])
        log = q.serial_read()
    assert "vgu: active" in log, "virtio-gpu driver did not activate"
    assert "vgu: flip ok" in log, "vgu selftest flip did not run"
    assert "vgu: cursor ok" in log, "vgu cursor selftest did not run"
    print("VGU TEST OK")

if __name__ == "__main__":
    main()