#!/usr/bin/env python3
"""Minimal GUI tester for AOS WM tests (QEMU monitor + screendump).

Usage:
  guitester.py click <x> <y>          move cursor to (x,y) and left-click
  guitester.py rclick <x> <y>         move cursor to (x,y) and right-click
  guitester.py snap <file.ppm>        screendump to file
  guitester.py pixel <x> <y>          print "r g b" of the pixel
  guitester.py check <x> <y> <r> <g> <b>   assert pixel color, exit 1 on mismatch
  guitester.py resetmouse <x> <y>     set tracked cursor position
"""
import socket, sys, time

MON = "/tmp/aos-gui.sock"
PPM = "/tmp/aos-gui-px.ppm"
# QEMU's HMP "mouse_move dx dy" is RELATIVE (deltas), so absolute clicks must
# be turned into deltas from the last known position. Tracked in MOUSE_STATE.
# The kernel boots the cursor at (511,383) (screen center, see mouse_init).
# After EACH QEMU restart the guest cursor re-centers at (511,383) but the
# state file still holds the last position, so run
#   guitester.py resetmouse 511 383
# (or delete /tmp/aos-mouse.state) before the first click.
MOUSE_STATE = "/tmp/aos-mouse.state"
MOUSE_BOOT = (511, 383)


def cmd(c):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2)
    try:
        s.connect(MON)
    except Exception:
        sys.exit("cannot connect to QEMU monitor %s — is QEMU running?" % MON)
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            if b"(qemu) " in chunk:
                break
    except Exception:
        pass
    data = b""
    try:
        s.sendall(c.encode() + b"\n")
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if b"(qemu) " in data:
                break
    except Exception:
        pass
    s.close()
    return data.decode()


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
        cmd("mouse_move %d %d" % (sx, sy))
        time.sleep(0.02)
        dx -= sx
        dy -= sy
    write_state(x, y)


def read_pixel(x, y):
    cmd("screendump " + PPM)
    time.sleep(0.15)
    try:
        f = open(PPM, "rb")
    except FileNotFoundError:
        sys.exit("no screendump %s — is QEMU running?" % PPM)
    with f:
        f.readline()                       # P6
        w = int(f.readline().split()[0])   # width
        f.readline()                       # maxval
        f.read((y * w + x) * 3)
        px = f.read(3)
    if len(px) != 3:
        sys.exit("pixel out of bounds (%d,%d)" % (x, y))
    return tuple(px)


def main():
    a = sys.argv[1:]
    if a[0] == "click":
        x, y = int(a[1]), int(a[2])
        move_abs(x, y)
        time.sleep(0.3)
        cmd("mouse_button 1")
        time.sleep(0.4)
        cmd("mouse_button 0")
    elif a[0] == "rclick":
        x, y = int(a[1]), int(a[2])
        move_abs(x, y)
        time.sleep(0.3)
        cmd("mouse_button 2")
        time.sleep(0.4)
        cmd("mouse_button 0")
    elif a[0] == "resetmouse":
        write_state(int(a[1]), int(a[2]))
    elif a[0] == "snap":
        cmd("screendump " + a[1])
    elif a[0] == "pixel":
        r, g, b = read_pixel(int(a[1]), int(a[2]))
        print("%d %d %d" % (r, g, b))
    elif a[0] == "check":
        x, y = int(a[1]), int(a[2])
        want = tuple(int(v) for v in a[3:6])
        got = read_pixel(x, y)
        if got != want:
            print("FAIL pixel(%d,%d)=%s want %s" % (x, y, got, want))
            sys.exit(1)
        print("OK pixel(%d,%d)=%s" % (x, y, got))
    else:
        sys.exit("usage: guitester click|rclick|snap|pixel|check|resetmouse ...")


if __name__ == "__main__":
    main()
