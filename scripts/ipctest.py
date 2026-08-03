#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-ipc.sock"
SER = "/tmp/aos-ipc.log"
PPM = "/tmp/aos-ipc.ppm"
BEFORE = "/tmp/aos-ipc-before.ppm"

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
    keys = {"\n": "ret", " ": "spc"}
    for ch in text:
        key = keys.get(ch, ch)
        hmp("sendkey " + key)
        time.sleep(0.04)

def serial_text():
    try:
        with open(SER, "r", errors="replace") as f: return f.read()
    except FileNotFoundError:
        return ""

def ppm_pixel(path, x, y):
    with open(path, "rb") as f:
        f.readline()
        w = int(f.readline().split()[0])
        f.readline()
        f.seek((y * w + x) * 3, 1)
        return tuple(f.read(3))

def main():
    for path in (MON, SER, PPM, BEFORE):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        time.sleep(5)
        hmp("mouse_move -39 341")
        time.sleep(0.3)
        hmp("mouse_button 1")
        time.sleep(0.4)
        hmp("mouse_button 0")
        time.sleep(1)
        hmp("screendump " + BEFORE)
        wait_for(BEFORE)
        if ppm_pixel(BEFORE, 30, 30) == (26, 32, 48):
            raise AssertionError("dock click did not spawn a terminal window")
        send_text("ipctest\n")
        time.sleep(3)
        if "KERNEL PANIC" in serial_text():
            raise AssertionError("ipctest triggered a kernel panic")
        hmp("screendump " + PPM)
        wait_for(PPM)
        if os.path.getsize(PPM) <= 1024:
            raise AssertionError("window manager did not produce a framebuffer dump")
        with open(BEFORE, "rb") as f: before = f.read()
        with open(PPM, "rb") as f: after = f.read()
        if before == after:
            raise AssertionError("terminal did not process the ipctest command")
        print("PASS: IPC exit notification and WM regression")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
