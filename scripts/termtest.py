#!/usr/bin/env python3
"""GUI smoke test for the VT-emulator terminal (bin/sh over pipes).

Boots the AOS ISO headless, spawns a terminal from the dock launcher,
waits for the bin/sh ``AOS>`` prompt to render, then types ``pwd`` and
``echo hi`` and asserts the bright-text pixel count in the terminal band
grows after each command. Fails on a kernel panic or if the shell exits
during the test.

Run with ``make`` (needs aos.iso built):

    python3 scripts/termtest.py
"""
import sys
import time

from qtest import QTest

MOUSE_STATE = "/tmp/aos-termtest-mouse.state"

# Terminal text band (see QTest.term_text_band): 26 rows of 16 px from y=39.
BAND = (21, 39, 660, 455)

PPM_PREFIX = "/tmp/termtest-%d.ppm"
_pnum = [0]


def shot(q):
    _pnum[0] += 1
    return q.screenshot(PPM_PREFIX % _pnum[0])


def band_bright(q, path):
    x0, y0, x1, y1 = BAND
    return q.count_text_pixels(path, x0, y0, x1, y1)


def wait_bright(q, predicate, seconds, label):
    end = time.time() + seconds
    n = 0
    while time.time() < end:
        n = band_bright(q, shot(q))
        if predicate(n):
            return n
        time.sleep(0.5)
    raise AssertionError("%s: band bright=%d not reached in %ds" % (label, n, seconds))


def main():
    with QTest("termtest", mouse_state=MOUSE_STATE, boot_wait=6) as q:
        q.boot_and_ready()
        if "KERNEL PANIC" in q.serial_read():
            raise AssertionError("kernel panic during boot")

        q.dock_spawn_term()

        # Wait for the bin/sh prompt (AOS> ) to render in the terminal band.
        wait_bright(q, lambda n: n > 0, 30, "sh prompt")
        print("  ok: sh prompt rendered")

        before = band_bright(q, shot(q))
        q.type_text("pwd\n")
        wait_bright(q, lambda n: n > before, 20, "pwd output")
        print("  ok: pwd output rendered")

        before = band_bright(q, shot(q))
        q.type_text("echo hi\n")
        wait_bright(q, lambda n: n > before, 20, "echo output")
        print("  ok: echo hi rendered")

        final = band_bright(q, shot(q))
        if final <= 50:
            raise AssertionError("final band bright=%d not > 50" % final)

        log = q.serial_read()
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during test")
        if "[sh exited]" in log:
            raise AssertionError("sh exited during test")

        print("PASS: TERMTEST OK (final band bright=%d, screenshot %s)"
              % (final, q._last_ppm))
        return 0


if __name__ == "__main__":
    sys.exit(main())
