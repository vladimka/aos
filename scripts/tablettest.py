#!/usr/bin/env python3
"""USB tablet (absolute pointer) regression for the AOS WM.

Boots the ISO headless with a USB UHCI controller + HID tablet device and
drives the guest through QMP ``input-send-event`` absolute events, so the whole
path is exercised: QEMU tablet device -> guest UHCI interrupt IN TD ->
``mouse_tablet_set()`` -> WM click handling. This is the regression for the
"click at (0,0)" bug caused by AnyDesk's relative PS/2 deltas desyncing from
the QEMU GTK pointer.

QEMU's HID layer compresses back-to-back pointer events when the guest has an
unconsumed event queued (``hid_pointer_sync`` event compression), which under
TCG can merge/absorb a click. The test therefore confirms each action by its
on-screen result (menu open/closed) and retries the click, which converges
because every retry is issued from a known UI state.
"""
import sys
import time

from qtest import QTest

GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]
TABLET_ARGS = GPU_ARGS + ["-device", "piix3-usb-uhci", "-device", "usb-tablet"]
QMP_SOCK = "/tmp/aos-tablet-qmp.sock"

PANEL_BG = (35, 44, 64)            # col_dock_bg 0x232C40
MENU_BG = (32, 40, 58)             # col_menu_bg 0x20283A
PWR_BTN = (1004, 13)               # top-right 24x24 power button centre
PWR_MENU_PIX = (880, 60)           # power menu interior bg
CLOSE_POS = (850, 500)             # clear desktop (right of the term window)
CTX_MENU_PIX = (1014, 534)         # ctx menu interior bg at the close pos
_BTN = {"left": 1, "right": 2}


def absx(sx):
    return sx * 0x7FFF // 1024


def absy(sy):
    return sy * 0x7FFF // 768


def main():
    with QTest("tablet", boot_wait=6, extra_args=TABLET_ARGS,
               qmp_socket=QMP_SOCK) as q:
        q.boot_and_ready()
        if not q.serial_wait("USB tablet enumerated.", timeout=15):
            raise AssertionError("tablet not enumerated; log:\n"
                                 + q.serial_read()[-400:])
        print("  ok: USB tablet enumerated")
        q.screenshot("/tmp/aos-tablet-0-panel.ppm")
        q.assert_pixel(700, 0, PANEL_BG, "top panel background")
        print("  ok: panel rendered")

        def qmp_ok(obj):
            r = q.qmp(obj)
            if "error" in r:
                raise AssertionError("QMP error: " + str(r["error"]))

        def send_abs(x, y):
            qmp_ok({"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": x}},
                {"type": "abs", "data": {"axis": "y", "value": y}}]}})

        # The tablet handler is activated by the guest on its first IN poll;
        # input injected before that can be dropped. Retry a plain abs move
        # until the guest confirms the tablet source.
        moved = False
        for _ in range(12):
            send_abs(absx(512), absy(384))
            if q.serial_wait("USB tablet mouse active.", timeout=2):
                moved = True
                break
        if not moved:
            raise AssertionError("tablet source not active; log:\n"
                                 + q.serial_read()[-400:])
        print("  ok: USB tablet mouse active (PS/2 bypassed)")

        def menu_open(pix):
            q.screenshot("/tmp/aos-tablet-poll.ppm")
            return q.pixel(pix[0], pix[1], "/tmp/aos-tablet-poll.ppm") == MENU_BG

        def tablet_click(sx, sy, button, pix, expect_open, what):
            """Press+release the tablet button at (sx,sy) until the UI state
            at *pix* reaches *expect_open*. The release resets the guest
            button latch between retries, so a dropped press/release cannot
            wedge the WM's button tracking."""
            bit = _BTN[button]
            x, y = absx(sx), absy(sy)
            for _ in range(10):
                # clear any stuck button, then press
                send_abs(x, y)
                qmp_ok({"execute": "input-send-event", "arguments": {"events": [
                    {"type": "abs", "data": {"axis": "x", "value": x}},
                    {"type": "abs", "data": {"axis": "y", "value": y}},
                    {"type": "btn", "data": {"button": button,
                                             "down": False}}]}})
                time.sleep(0.4)
                qmp_ok({"execute": "input-send-event", "arguments": {"events": [
                    {"type": "abs", "data": {"axis": "x", "value": x}},
                    {"type": "abs", "data": {"axis": "y", "value": y}},
                    {"type": "btn", "data": {"button": button,
                                             "down": True}}]}})
                time.sleep(0.8)
                qmp_ok({"execute": "input-send-event", "arguments": {"events": [
                    {"type": "abs", "data": {"axis": "x", "value": x}},
                    {"type": "abs", "data": {"axis": "y", "value": y}},
                    {"type": "btn", "data": {"button": button,
                                             "down": False}}]}})
                time.sleep(0.8)
                if menu_open(pix) == expect_open:
                    return
            raise AssertionError("%s: state at %s did not become %s (got %s)" %
                                 (what, pix, "open" if expect_open else "closed",
                                  "open" if menu_open(pix) else "closed"))

        # Absolute left-click on the power button must open the power menu.
        tablet_click(*PWR_BTN, "left", PWR_MENU_PIX, True,
                     "power menu opened by abs click")
        print("  ok: abs left-click opened the power menu")

        # Left-click on clear desktop closes it, then an absolute right-click
        # opens the context menu anchored at the click point.
        tablet_click(*CLOSE_POS, "left", PWR_MENU_PIX, False,
                     "power menu closed by abs click")
        print("  ok: abs left-click closed the power menu")
        tablet_click(*CLOSE_POS, "right", CTX_MENU_PIX, True,
                     "context menu opened by abs right-click")
        print("  ok: abs right-click opened the context menu")

        # The menu must be anchored exactly at the (clamped) click point:
        # top edge at y=500 (no clamp) and left edge at x=848 (clamped from
        # 850 because 850+176 > 1024). Proves absolute coords map 1:1.
        q.screenshot("/tmp/aos-tablet-1-ctx.ppm")
        ACCENT = (91, 147, 216)
        q.assert_pixel(900, 500, ACCENT, "ctx menu top frame at click y")
        q.assert_pixel(848, 508, ACCENT, "ctx menu left frame at clamped x")
        print("  ok: context menu anchored at the abs click point")
    print("PASS: USB tablet absolute pointer")
    return 0


if __name__ == "__main__":
    sys.exit(main())