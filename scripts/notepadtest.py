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
import sys
import time

from qtest import QTest, count_bright

MOUSE_STATE = "/tmp/aos-notepad.state"
PPM = "/tmp/aos-notepad.ppm"

# GPU scanout path: replace the default std VGA with virtio-vga so the WM
# renders through the double-buffered virtio-gpu flip (see Task 6).
GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

DESKTOP = (26, 32, 48)            # COL_DESKTOP 0x1A2030
MENU_BG = (32, 40, 58)            # COL_MENU_BG  0x20283A
CONTENT_BG = (16, 16, 16)         # COL_BG      0x101010
GREEN = (0, 255, 0)

NOTEPAD_X, NOTEPAD_Y = 20, 20     # first window slot
STATUS_Y = NOTEPAD_Y + 1 + 18 + 25 * 16   # status bar row (25) screen y


def assert_pixel(q, path, x, y, want, what):
    q.assert_pixel(x, y, want, what, path=path)


def main():
    with QTest("notepad", mouse_state=MOUSE_STATE, ppm=PPM, boot_wait=6,
               extra_args=GPU_ARGS) as q:
        # 1. demo.ico green disc on the desktop (icon 0 at grid (16,24)).
        q.screenshot("/tmp/notepad-0-ico.ppm")
        assert_pixel(q, "/tmp/notepad-0-ico.ppm", 31, 39, GREEN,
                     "demo.ico green disc")

        # 2. Right-click desktop opens the context menu. The cursor sits at
        #    the menu's top-left corner, so item 0 is hover-highlighted (in
        #    accent); probe item 1's row for the interior bg color.
        q.mouse_click(500, 400, button="right")
        time.sleep(0.5)
        q.screenshot("/tmp/notepad-1-menu.ppm")
        assert_pixel(q, "/tmp/notepad-1-menu.ppm", 520, 430, MENU_BG,
                     "context menu background")

        # 3. Pick "Новый файл", type a name, press Enter.
        q.mouse_click(590, 411)
        time.sleep(0.5)
        q.screenshot("/tmp/notepad-2-dialog.ppm")
        assert_pixel(q, "/tmp/notepad-2-dialog.ppm", 400, 285, MENU_BG,
                     "create-file dialog background")
        q.type_text("note.txt")
        q.key("ret")
        time.sleep(1)
        q.screenshot("/tmp/notepad-3-icon.ppm")
        if count_bright("/tmp/notepad-2-dialog.ppm", 68, 24, 99, 55) != 0:
            raise AssertionError("note.txt icon area was bright before creation")
        if count_bright("/tmp/notepad-3-icon.ppm", 68, 24, 99, 55) == 0:
            raise AssertionError("note.txt icon did not appear on the desktop")
        print("  ok: note.txt icon appeared")

        # 4. Open it in notepad, type text, save with Ctrl+S.
        q.mouse_click(84, 40)
        time.sleep(1)
        q.screenshot("/tmp/notepad-4-window.ppm")
        assert_pixel(q, "/tmp/notepad-4-window.ppm", 300, 100, CONTENT_BG,
                     "notepad content background")
        q.type_text("hello world")
        q.key("ret")
        q.type_text("second line")
        time.sleep(0.3)
        before = count_bright("/tmp/notepad-4-window.ppm", 21, STATUS_Y,
                              659, STATUS_Y + 15)
        q.key("ctrl-s")
        time.sleep(0.5)
        q.screenshot("/tmp/notepad-5-saved.ppm")
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
        q.mouse_click(650, 30)                       # close button of notepad
        time.sleep(0.5)
        q.dock_spawn_term()
        q.screenshot("/tmp/notepad-6-term.ppm")
        before = count_bright("/tmp/notepad-6-term.ppm", 21, 39, 660, 70)
        q.type_text("cat note.txt\n")
        after = before
        for _ in range(40):
            time.sleep(0.25)
            q.screenshot("/tmp/notepad-7-cat.ppm")
            after = count_bright("/tmp/notepad-7-cat.ppm", 21, 39, 660, 70)
            if after - before >= 200:
                break
        if after - before < 200:
            raise AssertionError(
                "term did not print cat note.txt (band bright %d -> %d)"
                % (before, after))
        print("  ok: `cat note.txt` rendered in the terminal")

        print("PASS: desktop icons, context menu, notepad save regression")
        return 0


if __name__ == "__main__":
    sys.exit(main())
