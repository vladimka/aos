#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-strace.sock"
SER = "/tmp/aos-strace.sock"

def wait_for(path, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path): return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)

def drain(s, log, end, needle=b""):
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: break
            log += d
            if needle and needle in log:
                break
        except socket.timeout:
            pass
    return log

def main():
    for path in (MON, SER):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "unix:" + SER + ",server,nowait",
        "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(SER)
        time.sleep(0.5)              # connect before the boot log is emitted
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        # The shell runs `strace linrun` in-place as task 0; it must be queued
        # the moment AOS> appears, before the WM registers as the event consumer
        # and starts capturing serial input.
        drain(s, b"", time.time() + 40, b"AOS>")

        s.sendall(b"strace linrun\n")
        out = drain(s, b"", time.time() + 45, b"== pid 2 ==")
        tail = out[-1200:]
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during strace linrun:\n"
                                 + tail.decode(errors="replace"))
        if b"== pid 0 ==" not in out:
            raise AssertionError("strace session did not dump == pid 0 ==\n" + tail)
        if b"== pid 2 ==" not in out:
            raise AssertionError("strace did not collect the spawned child (lin/hello is pid 2)\n" + tail)
        # musl stdout goes through writev(146) (its __stdio_write); the session
        # covers AOS_EXT spawn + Linux writev + Linux exit_group.
        for probe in ("spawn(", "writev(", "exit_group("):
            if probe.encode() not in out:
                raise AssertionError("strace output missing %r\n%s" % (probe, tail))
        print("PASS: strace shows AOS_EXT spawn + linux writev/exit_group, child collected")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
