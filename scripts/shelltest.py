#!/usr/bin/env python3
"""E2E shell test: env vars ($VAR), exit status ($?), background jobs (&).

Queue a serial burst right at the AOS> prompt (before the WM captures
serial input) and assert on the accumulated serial log.
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
SER = "/tmp/aos-shelltest.ser"

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
            if needle and needle in log: break
        except socket.timeout:
            pass
    return log

def main():
    try: os.unlink(SER)
    except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-no-reboot",
        "-serial", "unix:" + SER + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = b""
    try:
        wait_for(SER)
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        log = drain(s, b"", time.time() + 40, b"AOS>")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot:\n" + log[-400:].decode(errors="replace"))

        burst = (b"export AOSHOME=/home/test\n"
                 b"echo $AOSHOME\n"
                 b"lin/hello\n"
                 b"echo $?\n"
                 b"exitto\n"
                 b"echo $?\n"
                 b"echo bgtag > /bg.txt &\n"
                 b"cat /bg.txt\n"
                 b"echo done\n"
                 b"cat /bg.txt\n"
                 b"uptime &\n"
                 b"echo $?\n")
        s.sendall(burst)
        # Drain until the async background uptime output lands: everything else
        # (bgtag, 0/7, bg: pid) is emitted before it, so stopping here cannot
        # miss the assertions.
        out = drain(s, b"", time.time() + 30, b"Uptime:")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during shell commands:\n" + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        failures = []
        # $VAR expansion: `echo $AOSHOME` prints "/home/test" on its own line.
        if "\n/home/test\n" not in otext:
            failures.append("echo $AOSHOME did not print /home/test")
        # lin/hello exits 0 -> `echo $?` prints 0.
        if "\n0\n" not in otext:
            failures.append("echo $? after lin/hello did not print 0")
        # exitto exits 7 -> `echo $?` prints 7 (deterministic nonzero).
        if "\n7\n" not in otext:
            failures.append("echo $? after exitto did not print 7")
        # uptime & spawns a background task: bg: pid N line + $? = 0.
        if "bg: pid" not in otext:
            failures.append("uptime & did not print 'bg: pid'")
        # The bg uptime output appears asynchronously.
        if "Uptime:" not in otext:
            failures.append("background uptime output missing")
        # bg redirect: `echo bgtag > /bg.txt &` then foreground cats. The
        # second cat runs after `echo done`, giving the bg echo time to
        # finish; an empty early cat cannot false-positive.
        if "bgtag" not in otext:
            failures.append("cat /bg.txt did not show the bg-redirected file")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
        print("PASS: env expansion, $?, background jobs, bg redirect")
        return 0
    finally:
        try:
            with open("/tmp/aos-shelltest.log", "wb") as f:
                f.write(out)
        except (NameError, OSError):
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
