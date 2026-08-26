#!/usr/bin/env python3
"""GUI terminal scrollback regression.

Boots the AOS ISO headless (GPU path), spawns a terminal through the dock
launcher, plants a *colour marker* at the top of the history and floods the
screen past one screenful, then:

  1. live view: no blue pixels anywhere in the content,
  2. PageUp scrolls into history and the blue marker block appears,
  3. PageDown clamps back to the live view (marker gone, exact pixels),
  4. any other key while scrolled returns to the live view (marker gone).

The marker is `ls /`: directory names render in xterm-33 blue (0,175,255),
a colour nothing else in the terminal ever uses. The flood is a single
`lin/piptest gen 2000 | lin/cat` pipeline (~60 uniform lines) -- repeated
short commands are avoided because the GUI terminal currently stops
rendering after a few busy key/output cycles (a pre-existing issue, also
present without this feature).

Wheel scrolling (WM forwards MSG_WHEEL to the focused window) shares the
same term.c path but cannot be injected headlessly by this QEMU build, so
it is exercised through the keyboard instead.

Keystrokes drop while the guest renders (QEMU's 16-byte PS/2 queue
overflows), so commands are typed only after the previous output drained
and every key press is confirmed by its on-screen result and retried
(same pattern as tablettest). Run:

    python3 scripts/termscrolltest.py
"""
import sys
import time

from qtest import QTest, count_bright, ppm_data

MOUSE_STATE = "/tmp/aos-termscroll.state"
PPM = "/tmp/aos-termscroll.ppm"

# GPU scanout path, same as notepadtest/configtest.
GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]

# The dock term launcher glyph sits at ~(456..487, 718..749). The spawned
# terminal takes the first free window slot: x = 20 + 24*i, y = PANEL_H(26) +
# 8 + 28*i, content origin = (x + 1, y + title 18 + border 1), 80x26 chars of
# 8x16 px. The boot clock window may or may not occupy the other slot (260 px
# wide, up to ~130 px into the content), so all sampled regions stay below
# y = oy+150 -- clear of the clock whichever slot it took.
DOCK_TERM = (471, 733)
SLOT0 = (21, 53)
SLOT1 = (45, 81)


def title_blueish(path, x, y):
    w, _, data = ppm_data(path)
    off = (y * w + x) * 3
    r, g, b = data[off], data[off + 1], data[off + 2]
    return b > 80 and b > r and g > 40


def term_origin(q):
    """Locate the term window: slot 0 at (21,53) or slot 1 at (45,81)."""
    q.screenshot(PPM + ".geom")
    if title_blueish(PPM + ".geom", 500, 44):
        return SLOT0
    if title_blueish(PPM + ".geom", 500, 72):
        return SLOT1
    raise AssertionError("cannot locate the term window title bar")


def band_sig(path, rect):
    """Sample every 4th pixel of *rect* as a comparable signature tuple."""
    x0, y0, x1, y1 = rect
    w, _, data = ppm_data(path)
    sig = []
    for y in range(y0, y1 + 1, 2):
        for x in range(x0, x1 + 1, 8):
            off = (y * w + x) * 3
            sig.append(data[off])
            sig.append(data[off + 2])
    return tuple(sig)


def count_accent(path, rect):
    """Count the ls directory-blue pixels in *rect*.

    The terminal renders xterm-33 as ~(0,135,255) on this setup; the white
    text (255,255,255), the clock digits and the WM cursor accent
    (91,147,216) do not pass the filter.
    """
    x0, y0, x1, y1 = rect
    w, _, data = ppm_data(path)
    n = 0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            off = (y * w + x) * 3
            if (data[off] < 60 and 100 < data[off + 1] < 170
                    and data[off + 2] > 230):
                n += 1
    return n


def wait_settled(q, ox, oy, timeout=90):
    """Poll until the listing columns stop changing (output drained).

    Require 5 consecutive identical polls (~2.5 s of stability): the drain
    renders in slow chunks under TCG, so two equal samples can coincide
    mid-stream. The rect sits below the clock window's reach.
    """
    rect = (ox, oy + 170, ox + 300, oy + 25 * 16 + 15)
    stable = 0
    prev = -1
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(0.5)
        q.screenshot(PPM)
        cur = count_bright(PPM, *rect)
        stable = stable + 1 if cur == prev and cur > 0 else 0
        prev = cur
        if stable >= 5:
            return
    raise AssertionError("term output did not settle in %ds" % timeout)


def spawn_term(q):
    """Click the dock term launcher until the terminal window shows up."""
    for _ in range(6):
        q.mouse_click(*DOCK_TERM)
        for _ in range(10):
            time.sleep(0.4)
            try:
                q.screenshot(q.ppm + ".poll")
                w, _, data = ppm_data(q.ppm + ".poll")
            except RuntimeError:
                continue
            dark = sum(1 for x in range(300, 700)
                       if data[(200 * w + x) * 3] < 20
                       and data[(200 * w + x) * 3 + 1] < 20)
            if dark > 300:
                time.sleep(0.5)
                return
    raise AssertionError("dock click did not spawn the terminal window")


def key_until_accent(q, key, path, content, want_visible, tries=4):
    """Send *key* up to *tries* times until the marker matches the goal.

    Each press is polled for up to 3 s before another one is sent -- a full
    scrollback render + WM composite takes ~1 s under TCG.
    """
    for _ in range(tries):
        q.key(key)
        end = time.time() + 3.0
        while time.time() < end:
            time.sleep(0.4)
            q.screenshot(path)
            if (count_accent(path, content) > 0) == want_visible:
                return
    raise AssertionError("key %r: marker did not %s after %d tries"
                         % (key, "appear" if want_visible else "disappear",
                            tries))


def main():
    with QTest("termscroll", mouse_state=MOUSE_STATE, ppm=PPM, boot_wait=6,
               extra_args=GPU_ARGS) as q:
        # 1. Spawn the term via the dock launcher (init starts only wm and
        #    clock), give its sh time to start, then click inside the content
        #    to make sure the window is focused.
        spawn_term(q)
        ox, oy = term_origin(q)
        time.sleep(2.0)
        q.type_text("\n")              # harmless: spawns sh if not yet alive
        time.sleep(1.0)
        q.mouse_click(ox + 200, oy + 250)
        time.sleep(0.5)

        # 2. Plant the colour marker at the top of the history, then flood
        #    past one screen with a single pipeline (~60 uniform lines).
        #    Commands are typed only after the previous output drained:
        #    keystrokes sent while the guest renders overflow QEMU's 16-byte
        #    PS/2 queue and get dropped. The 4-row `ls /` output never
        #    reaches the settle-sampling strip (kept below the clock
        #    window's reach), so it gets a fixed drain pause instead.
        q.type_text("ls /\n")
        time.sleep(8)
        q.type_text("lin/piptest gen 2000 | lin/cat\n")
        wait_settled(q, ox, oy, timeout=120)

        # The accent filter only matches xterm-33 blue -- the clock window's
        # white digits and the WM's accent-coloured cursor do not pass it, so
        # the full content area is safe to sample.
        content = (ox, oy, ox + 639, oy + 25 * 16 + 15)
        mid = (ox, oy + 170, ox + 300, oy + 186)

        # 3. Live view: the marker block is buried in history -- no blue.
        q.screenshot("/tmp/termscroll-live.ppm")
        if count_accent("/tmp/termscroll-live.ppm", content) != 0:
            raise AssertionError("blue marker visible in the live view")
        print("  ok: live view shows no history marker")

        # 4. PageUp until the marker block scrolls into view: the view moved
        #    into history (the marker exists only there).
        key_until_accent(q, "pgup", "/tmp/termscroll-up.ppm", content, True)
        print("  ok: PageUp scrolled into history (marker visible)")

        # 5. PageDown until the view clamps back at the live pixels: marker
        #    gone and the sampled band byte-equal to the live view again.
        key_until_accent(q, "pgdn", "/tmp/termscroll-clamp.ppm", content,
                         False)
        for _ in range(12):
            q.key("pgdn")
            time.sleep(0.3)
        time.sleep(1.0)
        q.screenshot("/tmp/termscroll-clamp2.ppm")
        if count_accent("/tmp/termscroll-clamp2.ppm", content) != 0:
            raise AssertionError("PageDown did not clamp back to live view")
        q.screenshot("/tmp/termscroll-live2.ppm")
        if band_sig("/tmp/termscroll-clamp2.ppm", mid) != \
                band_sig("/tmp/termscroll-live2.ppm", mid):
            raise AssertionError("clamped view differs from the live view")
        print("  ok: PageDown clamps at the live view")

        # 6. Any key returns to live from mid-history.
        key_until_accent(q, "pgup", "/tmp/termscroll-up2.ppm", content, True)
        q.key("x")                     # resets voff, echoes 'x' at the prompt
        time.sleep(1.5)
        q.screenshot("/tmp/termscroll-key.ppm")
        if count_accent("/tmp/termscroll-key.ppm", content) != 0:
            raise AssertionError("key press while scrolled did not return "
                                 "to the live view")
        q.screenshot("/tmp/termscroll-live3.ppm")
        if band_sig("/tmp/termscroll-key.ppm", mid) != \
                band_sig("/tmp/termscroll-live3.ppm", mid):
            raise AssertionError("view after key press differs from live")
        print("  ok: key press returned to the live view")

        print("PASS: GUI terminal scrollback regression")
        return 0


if __name__ == "__main__":
    sys.exit(main())
