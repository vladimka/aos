#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-strace-live.sock"
SER = "/tmp/aos-strace-live.sock"

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

        # `strace bgspawn` runs in-place as task 0; bgspawn waits for the WM to
        # register, spawns bin/clock (pid 2, inherits trace_on), lets it render,
        # echoes /proc/2/trace itself, and exits — then the session dumps both
        # live tasks to the serial log. Must be queued before the WM captures
        # serial input.
        drain(s, b"", time.time() + 40, b"AOS>")

        s.sendall(b"strace bgspawn\n")
        # First == pid 2 == comes from bgspawn's own echo of /proc/2/trace; the
        # second comes from the session dump. Keep reading until the dump's pid-2
        # block is done.
        out = drain(s, b"", time.time() + 60, b"== pid 2 ==")
        end = time.time() + 30
        while time.time() < end and out.count(b"== pid 2 ==") < 2:
            out = drain(s, out, end)
        tail = out[-1500:]
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during strace bgspawn:\n"
                                 + tail.decode(errors="replace"))
        if b"== pid 0 ==" not in out:
            raise AssertionError("strace session did not dump == pid 0 ==\n" + tail)
        if b"== pid 2 ==" not in out:
            raise AssertionError("strace did not collect the live clock child (pid 2)\n" + tail)
        # The live trace must show the clock's AOS_EXT render calls (its own
        # /proc/2/trace echo and/or the session dump both carry them).
        for probe in ("fill(", "text(", "send("):
            if probe.encode() not in out:
                raise AssertionError("live trace missing %r\n%s" % (probe, tail))
        print("PASS: /proc/2/trace + dump show live AOS_EXT fill/text/send of the running clock")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
