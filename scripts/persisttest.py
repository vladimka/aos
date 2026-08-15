#!/usr/bin/env python3
"""End-to-end persistence test: data written to the ATA disk survives a reboot.

Boot 1: write ``p.txt`` via the shell, then shut down (SYS_SHUTDOWN must flush
the sector cache to the disk). Boot 2: the file must still be listed and its
content must be intact.
"""
import os
import subprocess
import sys

from qtest import QTest

IMG = "/tmp/aos-persist.img"
DRIVE = ["-drive", "file=" + IMG + ",format=raw,if=ide"]


def boot_once(name):
    q = QTest(name, serial_mode="socket", extra_args=DRIVE)
    q.start()
    s = q.serial_socket()
    out = q.boot_and_ready(s) or b""
    if b"KERNEL PANIC" in out:
        raise AssertionError("kernel panic during boot:\n"
                             + out[-400:].decode(errors="replace"))
    return q, s, out


def run_cmd(q, s, cmd, needle=b"AOS>", timeout=20):
    s.sendall(cmd + b"\n")
    out = q.serial_drain(s, timeout=timeout, needle=needle)
    return out or b""


def wait_exit(q, timeout=15):
    try:
        q.qemu.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        q.stop()
        raise AssertionError("guest did not power off after shutdown")


def main():
    try:
        os.unlink(IMG)
    except FileNotFoundError:
        pass
    subprocess.run(["truncate", "-s", "4M", IMG], check=True)

    # ---- Boot 1: write a file, then shut down ----
    q, s, _ = boot_once("persist1")
    out = run_cmd(q, s, b"echo persist-data > p.txt")
    if b"cannot open" in out or b"not found" in out:
        q.stop()
        raise AssertionError("echo > p.txt failed:\n"
                             + out[-400:].decode(errors="replace"))
    run_cmd(q, s, b"shutdown")
    wait_exit(q)
    q.stop()

    # ---- Boot 2: the file must survive ----
    q, s, boot = boot_once("persist2")
    if b"SFS2 mounted (disk)" not in boot:
        q.stop()
        raise AssertionError("disk was reformatted on reboot (data lost)")
    out = run_cmd(q, s, b"ls")
    if b"p.txt" not in out:
        q.stop()
        raise AssertionError("p.txt missing after reboot; ls tail:\n"
                             + out[-400:].decode(errors="replace"))
    out = run_cmd(q, s, b"cat p.txt")
    if b"persist-data" not in out:
        q.stop()
        raise AssertionError("p.txt content wrong after reboot; cat tail:\n"
                             + out[-400:].decode(errors="replace"))
    q.stop()

    print("PASS: ATA disk data survives reboot")
    return 0


if __name__ == "__main__":
    sys.exit(main())