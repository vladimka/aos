#!/usr/bin/env python3
"""Minimal GUI tester for AOS WM tests (QEMU monitor + screendump).

Drives an already-running QEMU (start it with the GPU path, e.g.):
  qemu-system-i386 -m 256 -cdrom aos.iso -display none \
    -serial file:/tmp/aos-gui.log -monitor unix:/tmp/aos-gui.sock,server,nowait \
    -vga none -device virtio-vga,disable-modern=on

Usage:
  guitester.py click <x> <y>          move cursor to (x,y) and left-click
  guitester.py rclick <x> <y>         move cursor to (x,y) and right-click
  guitester.py snap <file.ppm>        screendump to file
  guitester.py pixel <x> <y>          print "r g b" of the pixel
  guitester.py check <x> <y> <r> <g> <b>   assert pixel color, exit 1 on mismatch
  guitester.py resetmouse <x> <y>     set tracked cursor position
"""
import sys

from qtest import QTest

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


def main():
    # Drives an already-running QEMU: no start()/stop(), just reuse the
    # monitor/screenshot/mouse helpers from the shared harness.
    q = QTest("gui", monitor_socket=MON, ppm=PPM, mouse_state=MOUSE_STATE)
    a = sys.argv[1:]
    if a[0] == "click":
        q.mouse_click(int(a[1]), int(a[2]))
    elif a[0] == "rclick":
        q.mouse_click(int(a[1]), int(a[2]), button="right")
    elif a[0] == "resetmouse":
        q.reset_mouse(int(a[1]), int(a[2]))
    elif a[0] == "snap":
        q.screenshot(a[1])
    elif a[0] == "pixel":
        q.screenshot(PPM)
        r, g, b = q.pixel(int(a[1]), int(a[2]), path=PPM)
        print("%d %d %d" % (r, g, b))
    elif a[0] == "check":
        x, y = int(a[1]), int(a[2])
        want = tuple(int(v) for v in a[3:6])
        q.screenshot(PPM)
        got = q.pixel(x, y, path=PPM)
        if got != want:
            print("FAIL pixel(%d,%d)=%s want %s" % (x, y, got, want))
            sys.exit(1)
        print("OK pixel(%d,%d)=%s" % (x, y, got))
    else:
        sys.exit("usage: guitester click|rclick|snap|pixel|check|resetmouse ...")


if __name__ == "__main__":
    main()
