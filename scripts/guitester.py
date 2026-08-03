#!/usr/bin/env python3
"""Minimal GUI tester for AOS WM tests (QEMU monitor + screendump).

Usage:
  guitester.py click <x> <y>          move cursor to (x,y) and left-click
  guitester.py snap <file.ppm>        screendump to file
  guitester.py pixel <x> <y>          print "r g b" of the pixel
  guitester.py check <x> <y> <r> <g> <b>   assert pixel color, exit 1 on mismatch
"""
import socket, sys, time

MON = "/tmp/aos-gui.sock"
PPM = "/tmp/aos-gui-px.ppm"
# QEMU's HMP "mouse_move dx dy" is RELATIVE (deltas), so absolute clicks must
# be turned into deltas from the last known position. Tracked in MOUSE_STATE.
# The kernel boots the cursor at (511,383) (screen center, see mouse_init).
MOUSE_STATE = "/tmp/aos-mouse.state"
MOUSE_BOOT = (511, 383)


def cmd(c):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2)
    s.connect(MON)
    s.sendall(c.encode() + b"\n")
    try:
        data = s.recv(65536)
    except Exception:
        data = b""
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
    cmd("mouse_move %d %d" % (x - cx, y - cy))
    write_state(x, y)


def read_pixel(x, y):
    cmd("screendump " + PPM)
    time.sleep(0.15)
    with open(PPM, "rb") as f:
        f.readline()                       # P6
        w = int(f.readline().split()[0])   # width
        f.readline()                       # maxval
        f.read((y * w + x) * 3)
        return tuple(f.read(3))


def main():
    a = sys.argv[1:]
    if a[0] == "click":
        x, y = int(a[1]), int(a[2])
        move_abs(x, y)
        time.sleep(0.3)
        cmd("mouse_button 1")
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
        sys.exit("usage: guitester click|snap|pixel|check ...")


if __name__ == "__main__":
    main()
