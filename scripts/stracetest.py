#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-strace.sock"
SER = "/tmp/aos-strace.log"

def wait_for(path, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path): return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)

def hmp(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(3)
        s.connect(MON)
        data = b""
        while b"(qemu)" not in data:
            data += s.recv(4096)
        s.sendall(command.encode() + b"\n")
        data = b""
        while b"(qemu)" not in data:
            data += s.recv(4096)
        return data.decode(errors="replace")

def send_text(text):
    keys = {"\n": "ret", " ": "spc", "/": "slash"}
    for ch in text:
        key = keys.get(ch, ch) or ch
        hmp("sendkey " + key)
        time.sleep(0.04)

def read_log():
    try:
        with open(SER, "r", errors="replace") as f: return f.read()
    except FileNotFoundError:
        return ""

def main():
    for path in (MON, SER):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        time.sleep(5)
        send_text("strace linrun\n")
        end = time.time() + 25
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = read_log()
            if "== pid 0 ==" in log and "== pid 2 ==" in log: break
        tail = log[-900:]
        if "== pid 0 ==" not in log:
            raise AssertionError("strace session did not dump == pid 0 ==\n" + tail)
        if "== pid 2 ==" not in log:
            raise AssertionError("strace did not collect the spawned child (lin/hello is pid 2)\n" + tail)
        for probe in ("spawn(", "write(", "exit_group("):
            if probe not in log:
                raise AssertionError("strace output missing %r\n%s" % (probe, tail))
        if "KERNEL PANIC" in log:
            raise AssertionError("strace linrun triggered a kernel panic")
        print("PASS: strace shows AOS_EXT spawn + linux write/exit_group, child collected")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
