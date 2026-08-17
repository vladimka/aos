#!/usr/bin/env python3
"""Top panel + power menu regression for the AOS WM.

Boots the AOS ISO headless (GPU path) and asserts:
  1. the top panel renders (panel bg at (700,0) == 0x232C40) with a centered
     clock (bright time glyphs in the panel strip),
  2. the power button sits at the top-right and shows the power glyph,
  3. clicking it opens the power menu below it (menu bg pixels),
  4. clicking «Выключить» triggers the shutdown (serial: "wm: shutdown").
"""
import os
import sys
import time

from qtest import QTest, count_bright

GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

PANEL_BG = (35, 44, 64)            # col_dock_bg 0x232C40
MENU_BG = (32, 40, 58)             # col_menu_bg 0x20283A
PWR_BTN = (1004, 13)               # top-right 24x24 button centre
MENU_ITEM0 = (950, 40)             # «Выключить» row
MENU_ITEM1 = (880, 60)             # «Перезагрузить» row (interior bg)


def main():
    with QTest("power", boot_wait=6, extra_args=GPU_ARGS) as q:
        q.boot_and_ready()
        # 1. Panel bg + centered clock.
        q.screenshot("/tmp/aos-power-0-panel.ppm")
        q.assert_pixel(700, 0, PANEL_BG, "top panel background")
        if count_bright("/tmp/aos-power-0-panel.ppm", 400, 2, 640, 24) < 100:
            raise AssertionError("centered clock not rendered in the panel")
        print("  ok: panel + clock")
        # 2. Power button glyph (bright ring pixels inside the button).
        if count_bright("/tmp/aos-power-0-panel.ppm", 992, 1, 1016, 25) < 20:
            raise AssertionError("power button glyph missing")
        print("  ok: power button")
        # 3. Click the button -> power menu opens.
        q.mouse_click(*PWR_BTN)
        time.sleep(0.5)
        q.screenshot("/tmp/aos-power-1-menu.ppm")
        q.assert_pixel(*MENU_ITEM1, MENU_BG, "power menu item row")
        print("  ok: power menu opened")
        # 4. Click «Выключить» -> QEMU powers off (the monitor socket
        #    disappears), so the click's button-release cannot be delivered.
        q.mouse_move(*MENU_ITEM0)
        time.sleep(0.3)
        try:
            q.hmp("mouse_button 1")     # left-button press (HMP bitmask)
        except Exception:
            pass
        if not q.serial_wait("wm: shutdown", timeout=10):
            raise AssertionError("shutdown not triggered; log:\n"
                                 + q.serial_read()[-400:])
        end = time.time() + 5
        while os.path.exists(q.mon) and time.time() < end:
            time.sleep(0.25)
        if os.path.exists(q.mon):
            raise AssertionError("VM still running after shutdown")
        print("  ok: shutdown triggered")

    with QTest("power", boot_wait=6, extra_args=GPU_ARGS) as q:
        q.boot_and_ready()
        # 5. Reboot path: button -> menu -> «Перезагрузить».
        q.mouse_click(*PWR_BTN)
        time.sleep(0.5)
        q.screenshot("/tmp/aos-power-2-rebootmenu.ppm")
        q.assert_pixel(*MENU_ITEM1, MENU_BG, "power menu item row (reboot)")
        q.mouse_click(*MENU_ITEM1)
        if not q.serial_wait("wm: reboot", timeout=10):
            raise AssertionError("reboot not triggered; log:\n"
                                 + q.serial_read()[-400:])
        if not q.serial_wait("AAA    OOO    SSS", timeout=30):
            raise AssertionError("system did not boot after reboot; log:\n"
                                 + q.serial_read()[-400:])
        print("  ok: reboot triggered")
    print("PASS: top panel + power menu")
    return 0


if __name__ == "__main__":
    sys.exit(main())