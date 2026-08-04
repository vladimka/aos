#!/usr/bin/env python3
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-rtctest.sock"
SER = "/tmp/aos-rtctest.log"
PPM = "/tmp/aos-rtctest.ppm"
BEFORE = "/tmp/aos-rtctest-before.ppm"

TXT_X0, TXT_X1 = 21, 660          # term text band, x range
TXT_Y0, TXT_Y1 = 39, 39 + 26 * 16 # term text band, y range (all 26 rows)
TXT_THRESHOLD = 500               # band must grow by more than this (pixels)

DATE_RE = re.compile(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}")

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
        key = keys.get(ch, ch) or ch
        hmp("sendkey " + key)
        time.sleep(0.04)

def ppm_data(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise AssertionError("not a P6 PPM: " + path)
        dim = f.readline().split()
        f.readline()                       # maxval
        return int(dim[0]), int(dim[1]), f.read()

def count_text_pixels(path, x0, y0, x1, y1):
    w, h, data = ppm_data(path)
    n = 0
    for y in range(y0, y1 + 1):
        row = data[(y * w + x0) * 3:(y * w + x1 + 1) * 3]
        for i in range(0, len(row), 3):
            if row[i] >= 0xC0 and row[i + 1] >= 0xC0 and row[i + 2] >= 0xC0:
                n += 1
    return n

def serial_text():
    try:
        with open(SER, "r", errors="replace") as f: return f.read()
    except FileNotFoundError:
        return ""

def main():
    for path in (MON, SER, PPM, BEFORE):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        time.sleep(5)
        hmp("mouse_move -39 341")    # dock launcher spawns a terminal
        hmp("mouse_button 1")
        hmp("mouse_button 0")
        time.sleep(1)
        hmp("screendump " + BEFORE)
        wait_for(BEFORE)
        send_text("date\n")
        end = time.time() + 25
        log = ""
        while time.time() < end:
            time.sleep(1)
            log = serial_text()
            if "KERNEL PANIC" in log: break
        if "KERNEL PANIC" in log:
            raise AssertionError("date triggered a kernel panic")
        if "Unknown command" in log or "cannot run command" in log:
            raise AssertionError("date did not launch: %r"
                                 % log.strip().splitlines()[-1])
        if not DATE_RE.search(log):
            raise AssertionError("date did not print wall-clock time; log tail:\n"
                                 + log[-500:])
        hmp("screendump " + PPM)
        wait_for(PPM)
        if os.path.getsize(PPM) <= 1024:
            raise AssertionError("window manager did not produce a framebuffer dump")
        before_txt = count_text_pixels(BEFORE, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        after_txt = count_text_pixels(PPM, TXT_X0, TXT_Y0, TXT_X1, TXT_Y1)
        if after_txt - before_txt <= TXT_THRESHOLD:
            raise AssertionError(
                "terminal did not render date output (band text grew %d, want > %d)"
                % (after_txt - before_txt, TXT_THRESHOLD))
        print("PASS: RTC wall-clock date command")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
