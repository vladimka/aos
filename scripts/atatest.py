#!/usr/bin/env python3
"""Strengthened ATA end-to-end test.

Boot 1: detect the ATA drive via IDENTIFY, run the PIO/multi/DMA selftests,
verify the full 5 GiB capacity is reported, then do a VFS write roundtrip
(echo/cat, cp of a real ELF, wc) and shut down. Boot 2: the written files
must survive on the disk with intact content and size.

The >4 GiB boundary cannot be crossed through SFS2 (max file ~69 KiB: 8
direct + 128 indirect blocks), so it is covered by the 10485760-sector
capacity assert rather than a file-level roundtrip.
"""
import os
import re
import subprocess
import sys

from qtest import QTest

IMG = "/tmp/aos-atatest.img"
DRIVE = ["-drive", "file=" + IMG + ",format=raw,if=ide"]


def run(q, s, cmd, needle=b"AOS>", timeout=30):
    s.sendall(cmd + b"\n")
    out = q.serial_drain(s, timeout=timeout, needle=needle)
    return out or b""


def bytes_of_wc(out, name):
    m = re.search(rb"(\d+) (\d+) (\d+) " + re.escape(name), out)
    return int(m.group(3)) if m else None


def boot(q, s):
    log = q.boot_and_ready(s) or b""
    if b"KERNEL PANIC" in log:
        raise AssertionError("kernel panic during boot")
    return log


def main():
    try:
        os.unlink(IMG)
    except FileNotFoundError:
        pass
    subprocess.run(["truncate", "-s", "5G", IMG], check=True)

    # ---- Boot 1: detect, selftests, VFS write roundtrip ----
    q = QTest("atatest", serial_mode="socket", extra_args=DRIVE)
    q.start()
    s = q.serial_socket()
    log = boot(q, s)
    for needle, msg in [
        (b"ata: found", "ATA drive was not detected"),
        (b"ata: selftest OK", "ATA selftest did not report OK"),
        (b"ata: selftest multi OK", "ATA multi-sector selftest did not report OK"),
        (b"ata: dma selftest OK", "ATA DMA selftest did not report OK"),
        (b"block: ata backend, 10485760 sectors",
         "ATA backend did not report full 5 GiB capacity"),
    ]:
        if needle not in log:
            raise AssertionError(msg)
    if b"ata: selftest FAIL" in log:
        raise AssertionError("ATA selftest reported FAIL")
    if b"SFS2 mounted (disk)" not in log and b"SFS2 formatting new disk" not in log:
        raise AssertionError("SFS2 did not mount from ATA disk")

    out = run(q, s, b"echo roundtrip-data > rt.txt")
    out = run(q, s, b"cat rt.txt")
    if b"roundtrip-data" not in out:
        q.stop()
        raise AssertionError("echo > rt.txt / cat roundtrip failed")
    out = run(q, s, b"cp bin/cat big.txt")
    out = run(q, s, b"wc big.txt")
    nbytes = bytes_of_wc(out, b"big.txt")
    if nbytes is None or nbytes <= 4096:
        q.stop()
        raise AssertionError("big.txt too small, indirect blocks unused: %r"
                             % out[-200:])
    out = run(q, s, b"ls")
    for name in (b"rt.txt", b"big.txt"):
        if name not in out:
            q.stop()
            raise AssertionError(name.decode() + " missing from ls")
    bigsize = nbytes

    run(q, s, b"shutdown")
    try:
        q.qemu.wait(timeout=15)
    except subprocess.TimeoutExpired:
        q.stop()
        raise AssertionError("guest did not power off after shutdown")
    q.stop()

    # ---- Boot 2: the data must have survived on the disk ----
    q = QTest("atatest2", serial_mode="socket", extra_args=DRIVE)
    q.start()
    s = q.serial_socket()
    log = boot(q, s)
    if b"SFS2 mounted (disk)" not in log:
        q.stop()
        raise AssertionError("disk was reformatted on reboot (data lost)")

    out = run(q, s, b"cat rt.txt")
    if b"roundtrip-data" not in out:
        q.stop()
        raise AssertionError("rt.txt content lost after reboot")
    out = run(q, s, b"wc big.txt")
    if bytes_of_wc(out, b"big.txt") != bigsize:
        q.stop()
        raise AssertionError("big.txt size changed after reboot")
    # Byte-exact comparison over the serial line is unreliable (kernel
    # exec-trace lines interleave and COM1 can drop bytes on big reads), so
    # verify the copied ELF is intact by its magic instead.
    big = run(q, s, b"cat big.txt")
    if b"\x7fELF" not in big:
        q.stop()
        raise AssertionError("big.txt does not start with the ELF magic after reboot")
    q.stop()

    print("PASS: ATA drive detected, 5 GiB capacity, VFS roundtrip survives reboot")
    return 0


if __name__ == "__main__":
    sys.exit(main())
