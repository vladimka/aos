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
AFTER = "/tmp/aos-ipc-after.ppm"

TXT_X0, TXT_X1 = 21, 660          # term text band, x range (content left..right)
TXT_Y0, TXT_Y1 = 39, 70           # term text band, y range (content rows 0..1)
TXT_THRESHOLD = 500               # AFTER band must grow by more than this (pixels)

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

def ppm_data(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise AssertionError("not a P6 PPM: " + path)
        dim = f.readline().split()
        f.readline()                       # maxval
        return int(dim[0]), int(dim[1]), f.read()

def count_text_pixels(path, x0, y0, x1, y1):
    # Text color is COL_FG 0xD8D8D8 on COL_BG 0x101010; count pixels where
    # every channel is >= 0xC0 (bright) in the given x/y rectangle.
    w, h, data = ppm_data(path)
    n = 0
    for y in range(y0, y1 + 1):
        row = data[(y * w + x0) * 3:(y * w + x1 + 1) * 3]
        for i in range(0, len(row), 3):
            if row[i] >= 0xC0 and row[i + 1] >= 0xC0 and row[i + 2] >= 0xC0:
                n += 1
    return n

def main():
    for path in (MON, SER, PPM, BEFORE, AFTER):
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
        before_txt = count_text_pixels(BEFORE, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        after_txt = count_text_pixels(PPM, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        if after_txt - before_txt <= TXT_THRESHOLD:
            raise AssertionError(
                "terminal did not render IPC TEST PASS (band text grew %d, want > %d)"
                % (after_txt - before_txt, TXT_THRESHOLD))
        hmp("mouse_move 1 0")
        time.sleep(0.3)
        hmp("screendump " + AFTER)
        wait_for(AFTER)
        with open(AFTER, "rb") as f: moved = f.read()
        if moved == after:
            raise AssertionError("window manager stopped responding after the run")
        print("PASS: IPC exit notification and WM regression")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
