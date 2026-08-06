#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-klog.sock"
SER = "/tmp/aos-klog.log"

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
        end = time.time() + 30
        while time.time() < end and "klog: ready" not in read_log():
            time.sleep(0.5)
        if "klog: ready" not in read_log():
            raise AssertionError("boot marker 'klog: ready' not on serial")
        send_text("cat /proc/klog\n")
        end = time.time() + 20
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = read_log()
            if "klog: ready" in log and log.count("[") > 10: break
        lines = [l for l in log.splitlines() if "[0" in l or "[1" in l or "[f" in l]
        import re
        tstamped = [l for l in log.splitlines() if re.match(r"^\[\w{8}\] [IWE] ", l)]
        if len(tstamped) < 5:
            raise AssertionError("cat /proc/klog produced few timestamped lines (%d)"
                                 % len(tstamped))
        if not any("GDT" in l or "PMM" in l for l in tstamped):
            raise AssertionError("klog missing a boot line (GDT/PMM)")
        if not any("klog: ready" in l for l in tstamped):
            raise AssertionError("klog missing the 'klog: ready' INFO line")
        if "KERNEL PANIC" in log:
            raise AssertionError("cat /proc/klog triggered a kernel panic")
        print("PASS: /proc/klog shows timestamped INFO boot lines")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
