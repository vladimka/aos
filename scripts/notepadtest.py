#!/usr/bin/env python3
"""End-to-end desktop-icons + notepad regression for the AOS WM.

Boots the AOS ISO headless, then:
  1. asserts the demo.ico green disc renders on the desktop,
  2. right-clicks the desktop, picks "Новый файл", types "note.txt",
  3. asserts the new icon appears,
  4. clicks it to open notepad, types text, hits Ctrl+S,
  5. closes notepad, opens term, runs `cat note.txt`, asserts the output.

Every step is a QEMU monitor command + screendump + pixel assertion, so a
failure exits nonzero with a message. Run with `make` (needs aos.iso built):

    python3 scripts/notepadtest.py
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-notepad.sock"
SER = "/tmp/aos-notepad.log"
PPM = "/tmp/aos-notepad.ppm"
MOUSE_STATE = "/tmp/aos-notepad.state"
MOUSE_BOOT = (511, 383)

DESKTOP = (26, 32, 48)            # COL_DESKTOP 0x1A2030
MENU_BG = (32, 40, 58)            # COL_MENU_BG  0x20283A
CONTENT_BG = (16, 16, 16)         # COL_BG      0x101010
GREEN = (0, 255, 0)

NOTEPAD_X, NOTEPAD_Y = 20, 20     # first window slot
STATUS_Y = NOTEPAD_Y + 1 + 18 + 25 * 16   # status bar row (25) screen y


def wait_for(path, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path):
            return
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


def read_state():
    try:
        with open(MOUSE_STATE) as f:
            x, y = f.read().split()
            return int(x), int(y)
    except Exception:
        return MOUSE_BOOT


def write_state(x, y):
    with open(MOUSE_STATE, "w") as f:
        f.write("%d %d\n" % (x, y))


def move_abs(x, y):
    cx, cy = read_state()
    dx, dy = x - cx, y - cy
    step = 100
    while dx or dy:
        sx = max(-step, min(step, dx))
        sy = max(-step, min(step, dy))
        hmp("mouse_move %d %d" % (sx, sy))
        time.sleep(0.02)
        dx -= sx
        dy -= sy
    write_state(x, y)


def click(x, y):
    move_abs(x, y)
    time.sleep(0.3)
    hmp("mouse_button 1")
    time.sleep(0.4)
    hmp("mouse_button 0")


def rclick(x, y):
    move_abs(x, y)
    time.sleep(0.3)
    hmp("mouse_button 2")
    time.sleep(0.4)
    hmp("mouse_button 0")


def sendkey(key):
    hmp("sendkey " + key)
    time.sleep(0.04)


def send_text(text):
    keys = {"\n": "ret", " ": "spc"}
    for ch in text:
        sendkey(keys.get(ch, ch))


def snap(name):
    hmp("screendump " + name)
    wait_for(name)


def ppm_data(path):
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise AssertionError("not a P6 PPM: " + path)
        dim = f.readline().split()
        f.readline()                       # maxval
        return int(dim[0]), int(dim[1]), f.read()


def pixel(path, x, y):
    w, _, data = ppm_data(path)
    if x < 0 or y < 0 or x >= w or y * w + x >= len(data) // 3:
        raise AssertionError("pixel out of bounds (%d,%d) in %s" % (x, y, path))
    off = (y * w + x) * 3
    return tuple(data[off:off + 3])


def count_bright(path, x0, y0, x1, y1):
    # count pixels where every channel >= 0xC0 in the rect [x0..x1]x[y0..y1]
    w, _, data = ppm_data(path)
    n = 0
    for y in range(y0, y1 + 1):
        row = data[(y * w + x0) * 3:(y * w + x1 + 1) * 3]
        for i in range(0, len(row), 3):
            if row[i] >= 0xC0 and row[i + 1] >= 0xC0 and row[i + 2] >= 0xC0:
                n += 1
    return n


def assert_pixel(path, x, y, want, what):
    got = pixel(path, x, y)
    if got != want:
        raise AssertionError(
            "%s: pixel(%d,%d)=%s want %s" % (path, x, y, got, want))
    print("  ok: %s at (%d,%d)=%s" % (what, x, y, got))


def main():
    for path in (MON, SER, PPM, MOUSE_STATE):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    write_state(*MOUSE_BOOT)
    qemu = subprocess.Popen([
        "qemu-system-i386", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        time.sleep(6)

        # 1. demo.ico green disc on the desktop (icon 0 at grid (16,24)).
        snap("/tmp/notepad-0-ico.ppm")
        assert_pixel("/tmp/notepad-0-ico.ppm", 31, 39, GREEN,
                     "demo.ico green disc")

        # 2. Right-click desktop opens the context menu.
        rclick(500, 400)
        time.sleep(0.5)
        snap("/tmp/notepad-1-menu.ppm")
        assert_pixel("/tmp/notepad-1-menu.ppm", 520, 402, MENU_BG,
                     "context menu background")

        # 3. Pick "Новый файл", type a name, press Enter.
        click(590, 411)
        time.sleep(0.5)
        snap("/tmp/notepad-2-dialog.ppm")
        assert_pixel("/tmp/notepad-2-dialog.ppm", 400, 285, MENU_BG,
                     "create-file dialog background")
        send_text("note.txt")
        sendkey("ret")
        time.sleep(1)
        snap("/tmp/notepad-3-icon.ppm")
        if count_bright("/tmp/notepad-2-dialog.ppm", 68, 24, 99, 55) != 0:
            raise AssertionError("note.txt icon area was bright before creation")
        if count_bright("/tmp/notepad-3-icon.ppm", 68, 24, 99, 55) == 0:
            raise AssertionError("note.txt icon did not appear on the desktop")
        print("  ok: note.txt icon appeared")

        # 4. Open it in notepad, type text, save with Ctrl+S.
        click(84, 40)
        time.sleep(1)
        snap("/tmp/notepad-4-window.ppm")
        assert_pixel("/tmp/notepad-4-window.ppm", 300, 100, CONTENT_BG,
                     "notepad content background")
        send_text("hello world")
        sendkey("ret")
        send_text("second line")
        time.sleep(0.3)
        before = count_bright("/tmp/notepad-4-window.ppm", 21, STATUS_Y,
                              659, STATUS_Y + 15)
        sendkey("ctrl-s")
        time.sleep(0.5)
        snap("/tmp/notepad-5-saved.ppm")
        after = count_bright("/tmp/notepad-5-saved.ppm", 21, STATUS_Y,
                             659, STATUS_Y + 15)
        if after == 0:
            raise AssertionError("status bar shows no saved indicator")
        if after == before:
            raise AssertionError(
                "status bar did not change after Ctrl+S (%d bright px both)"
                % after)
        print("  ok: notepad status bar updated after Ctrl+S")

        # 5. Close notepad, open term, cat the file back.
        click(650, 30)                       # close button of notepad
        time.sleep(0.5)
        click(472, 724)                      # dock: term launcher
        time.sleep(1)
        click(300, 30)                       # term title bar -> focus
        time.sleep(0.3)
        snap("/tmp/notepad-6-term.ppm")
        before = count_bright("/tmp/notepad-6-term.ppm", 21, 39, 660, 70)
        send_text("cat note.txt\n")
        time.sleep(1)
        snap("/tmp/notepad-7-cat.ppm")
        after = count_bright("/tmp/notepad-7-cat.ppm", 21, 39, 660, 70)
        if after - before < 200:
            raise AssertionError(
                "term did not print cat note.txt (band bright %d -> %d)"
                % (before, after))
        print("  ok: `cat note.txt` rendered in the terminal")

        print("PASS: desktop icons, context menu, notepad save regression")
        return 0
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
