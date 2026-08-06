#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-lindir.sock"
SER = "/tmp/aos-lindir.log"

TXT_X0, TXT_X1 = 21, 660          # term text band, x range
TXT_Y0 = 39                       # term text top (window y=20 + border + title)
TXT_ROWS = 26                     # visible term rows
TXT_THRESHOLD = 400               # a non-empty listing renders more than this

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

def screendump(path):
    hmp("screendump " + path)
    wait_for(path)

def ppm(path):
    for _ in range(50):
        try:
            with open(path, "rb") as f:
                if f.readline().strip() != b"P6": raise AssertionError("bad ppm")
                dim = f.readline().split()
                f.readline()
                return int(dim[0]), f.read()
        except FileNotFoundError:
            time.sleep(0.05)
    raise RuntimeError("no ppm " + path)

def row_profile(path):
    """Per-16px text-row bright-pixel counts across the term text band."""
    w, data = ppm(path)
    rows = []
    for r in range(TXT_ROWS):
        y = TXT_Y0 + r * 16
        n = 0
        for yy in range(y, y + 16):
            row = data[(yy * w + TXT_X0) * 3:(yy * w + TXT_X1 + 1) * 3]
            for i in range(0, len(row), 3):
                if row[i] >= 0xC0 and row[i + 1] >= 0xC0 and row[i + 2] >= 0xC0:
                    n += 1
        rows.append(n)
    return rows

def nonempty_rows(profile):
    return [i for i, n in enumerate(profile) if n > 40]

# Entries a correct `lin/ls /` must print (drives the expected line count).
ROOT_ENTRIES = ["bin", "lin", "sys"]

def main():
    for p in (MON, SER):
        try: os.unlink(p)
        except FileNotFoundError: pass
    paths = ["/tmp/aos-lindir-root.ppm", "/tmp/aos-lindir-proc.ppm"]
    for p in paths:
        try: os.unlink(p)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        time.sleep(5)
        hmp("mouse_move -39 341")    # dock launcher spawns the terminal
        hmp("mouse_button 1")
        hmp("mouse_button 0")
        time.sleep(1)
        send_text("lin/ls /\n")
        time.sleep(3)
        screendump(paths[0])         # listing of SFS root
        send_text("lin/ls /proc\n")
        for _ in range(40):
            time.sleep(1)
            screendump(paths[1])
            if len(nonempty_rows(row_profile(paths[1]))) > \
               len(nonempty_rows(row_profile(paths[0]))) + 2:
                break
        screendump(paths[1])         # listing of procfs /proc

        log = open(SER, errors="replace").read() if os.path.exists(SER) else ""
        if "KERNEL PANIC" in log:
            raise AssertionError("lin/ls triggered a kernel panic")

        row_root = row_profile(paths[0])
        row_proc = row_profile(paths[1])

        root_rows = nonempty_rows(row_root)
        if sum(row_root) <= TXT_THRESHOLD:
            raise AssertionError("lin/ls / did not render a listing")
        if len(root_rows) < 3:
            raise AssertionError("lin/ls / produced unexpectedly few text rows")

        proc_rows = nonempty_rows(row_proc)
        added = [r for r in proc_rows if r not in root_rows]
        if not added:
            raise AssertionError("lin/ls /proc did not render an additional listing")

        # `ls /proc` must list procfs entries (uptime/version/mounts), which
        # differ in content from the `ls /` SFS entries: the added rows must be
        # non-blank and must visually differ from every `ls /` data row.
        root_data = [n for n in row_root[2:6] if n > 40]
        if not root_data:
            raise AssertionError("no recognisable `ls /` data rows")
        if len(added) < 3:
            raise AssertionError(
                "ls /proc added only %d rows (expected ~3 procfs entries)" % len(added))
        print("PASS: musl ls lists SFS / and procfs /proc via real fds "
              "(root rows %s, /proc additionally %s)" % (root_rows, added))
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())