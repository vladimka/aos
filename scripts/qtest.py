#!/usr/bin/env python3
"""Shared QEMU test harness for the AOS regression suite.

The ``QTest`` class owns one ``qemu-system-i386`` process: it boots the ISO,
exposes the HMP monitor for input/screendump, tracks absolute mouse position
in a state file, and reads the serial log. Tests run as a context manager::

    with QTest("mytest") as q:
        q.mouse_click(472, 724)
        q.type_text("echo hi\\n")
        q.assert_pixel(300, 100, (16, 16, 16), "content bg")

The harness is deliberately generic -- it knows nothing about AOS screen
layout. Pixel coordinates and test logic live in the individual test files.

A test that only needs to drive an already-running QEMU (e.g. the CLI
``guitester``) can instantiate ``QTest`` and call the input/screenshot methods
directly without ever calling ``start()``::

    q = QTest("gui", mouse_state=MOUSE_STATE, ppm=PPM)
    q.mouse_click(x, y)
"""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ISO = os.path.join(ROOT, "aos.iso")

# The kernel boots the guest cursor to screen centre (see mouse_init).
MOUSE_BOOT = (511, 383)

# HMP prompt marker (no trailing space -- matches "(qemu)" anywhere in the
# banner, and is a substring of the "(qemu) " prompt itself).
_HMP_PROMPT = b"(qemu)"

# QEMU "mouse_button" bitmask values.
_BUTTON = {"left": "1", "right": "2", "middle": "4"}


def wait_for(path, seconds=10):
    """Wait until *path* exists on disk (polls). Raises RuntimeError on timeout."""
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path):
            return True
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)


def ppm_data(path):
    """Return ``(width, height, raw_bytes)`` from a P6 PPM screenshot file."""
    with open(path, "rb") as f:
        if f.readline().strip() != b"P6":
            raise AssertionError("not a P6 PPM: " + path)
        dim = f.readline().split()
        f.readline()  # maxval
        return int(dim[0]), int(dim[1]), f.read()


def ppm_pixel(path, x, y):
    """Return the ``(r, g, b)`` tuple at pixel ``(x, y)`` in a P6 PPM file."""
    w, _, data = ppm_data(path)
    off = (y * w + x) * 3
    return tuple(data[off:off + 3])


def count_bright(path, x0, y0, x1, y1):
    """Count pixels whose every channel is >= 0xC0 in the rectangle.

    Rendered text is COL_FG (0xD8D8D8) on COL_BG (0x101010): bright glyph
    pixels stand out against the dark background, so growth in this count
    tracks newly printed text.
    """
    w, _, data = ppm_data(path)
    n = 0
    for y in range(y0, y1 + 1):
        row = data[(y * w + x0) * 3:(y * w + x1 + 1) * 3]
        for i in range(0, len(row), 3):
            if row[i] >= 0xC0 and row[i + 1] >= 0xC0 and row[i + 2] >= 0xC0:
                n += 1
    return n


class QTest:
    """Boot the AOS ISO under QEMU and drive it through the HMP monitor."""

    def __init__(self, name, *, monitor_socket=None, serial_log=None,
                 mouse_state=None, ppm=None, iso=None, mouse_boot=MOUSE_BOOT,
                 boot_wait=5):
        self.name = name
        self.mon = monitor_socket or f"/tmp/aos-{name}.sock"
        self.ser = serial_log or f"/tmp/aos-{name}.log"
        self.mouse_state = mouse_state or f"/tmp/aos-{name}-mouse.state"
        self.ppm = ppm or f"/tmp/aos-{name}.ppm"
        self.iso = iso or DEFAULT_ISO
        self.mouse_boot = mouse_boot
        self.boot_wait = boot_wait
        self.qemu = None
        self._last_ppm = None

    # -----------------------------------------------------------------
    # Lifecycle
    # -----------------------------------------------------------------
    def start(self, extra_args=None, boot_wait=None):
        """Launch QEMU, wait for the monitor socket, pause for the guest to boot.

        ``extra_args`` are appended to the QEMU command line (e.g. virtio
        devices). ``boot_wait`` seconds are slept after the monitor appears
        (defaults to the constructor ``boot_wait``).
        """
        if boot_wait is None:
            boot_wait = self.boot_wait
        for p in (self.mon, self.ser, self.mouse_state):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        self._write_state(*self.mouse_boot)
        cmd = [
            "qemu-system-i386", "-m", "256", "-cdrom", self.iso,
            "-display", "none",
            "-serial", "file:" + self.ser,
            "-monitor", "unix:" + self.mon + ",server,nowait",
        ]
        if extra_args:
            cmd += extra_args
        self.qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        wait_for(self.mon)
        time.sleep(boot_wait)
        return self

    def stop(self):
        """Terminate the QEMU process."""
        if self.qemu is None:
            return
        self.qemu.terminate()
        try:
            self.qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.qemu.kill()
            self.qemu.wait()
        self.qemu = None

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
        return False

    # -----------------------------------------------------------------
    # Raw monitor command
    # -----------------------------------------------------------------
    def hmp(self, command):
        """Send an HMP command and return the decoded response."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(5)
            s.connect(self.mon)
            data = b""
            while _HMP_PROMPT not in data:
                data += s.recv(4096)
            s.sendall(command.encode() + b"\n")
            data = b""
            while _HMP_PROMPT not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            return data.decode(errors="replace")

    # -----------------------------------------------------------------
    # Mouse (absolute, stateful)
    # -----------------------------------------------------------------
    def _read_state(self):
        try:
            with open(self.mouse_state) as f:
                x, y = f.read().split()
                return int(x), int(y)
        except Exception:
            return self.mouse_boot

    def _write_state(self, x, y):
        with open(self.mouse_state, "w") as f:
            f.write("%d %d\n" % (x, y))

    def reset_mouse(self, x, y):
        """Set the tracked cursor position without moving (drop-in for CLI)."""
        self._write_state(x, y)

    def mouse_move(self, x, y):
        """Move the cursor to absolute ``(x, y)`` in steps, tracking position."""
        cx, cy = self._read_state()
        dx, dy = x - cx, y - cy
        step = 100
        while dx or dy:
            sx = max(-step, min(step, dx))
            sy = max(-step, min(step, dy))
            self.hmp("mouse_move %d %d" % (sx, sy))
            time.sleep(0.02)
            dx -= sx
            dy -= sy
        self._write_state(x, y)

    def mouse_click(self, x, y, button="left"):
        """Absolute move to ``(x, y)`` then click *button*."""
        self.mouse_move(x, y)
        time.sleep(0.3)
        self.hmp("mouse_button " + _BUTTON[button])
        time.sleep(0.4)
        self.hmp("mouse_button 0")

    def mouse_scroll(self, clicks):
        """Scroll the wheel. Positive = down (matches QEMU's inverted dz).

        Emits one wheel unit per click (each is a discrete ``mouse_move``),
        which matches real hardware and avoids QEMU's dz clamping.
        """
        if clicks == 0:
            return
        dz = -1 if clicks > 0 else 1
        for _ in range(abs(clicks)):
            self.hmp("mouse_move 0 0 %d" % dz)
            time.sleep(0.05)

    # -----------------------------------------------------------------
    # Keyboard
    # -----------------------------------------------------------------
    def key(self, keyname):
        """Press a single key by HMP name (e.g. ``"ret"``, ``"ctrl-s"``)."""
        self.hmp("sendkey " + keyname)
        time.sleep(0.04)

    def keys(self, *keynames):
        """Press a key combination, e.g. ``keys("ctrl", "s")`` -> ``ctrl-s``."""
        self.hmp("sendkey " + "-".join(keynames))
        time.sleep(0.04)

    def type_text(self, text):
        """Type a string via sendkey (``\\n`` -> ``ret``, space -> ``spc``)."""
        keys = {"\n": "ret", " ": "spc"}
        for ch in text:
            self.key(keys.get(ch, ch))

    # -----------------------------------------------------------------
    # Screenshots
    # -----------------------------------------------------------------
    def screenshot(self, path):
        """Ask QEMU to screendump to *path*; wait for the file and remember it."""
        self.hmp("screendump " + path)
        wait_for(path)
        self._last_ppm = path
        return path

    def pixel(self, x, y, path=None):
        """Return ``(r, g, b)`` at ``(x, y)`` in *path* (default: last screenshot)."""
        p = path or self._last_ppm
        if p is None:
            raise RuntimeError("no screenshot taken yet -- call screenshot() first")
        return ppm_pixel(p, x, y)

    def assert_pixel(self, x, y, expected_rgb, msg="", path=None):
        """Assert pixel ``(x, y)`` equals *expected_rgb*; raise on mismatch."""
        got = self.pixel(x, y, path=path)
        if got != tuple(expected_rgb):
            raise AssertionError(
                "%s: pixel(%d,%d)=%s want %s" % (msg, x, y, got, expected_rgb))
        print("  ok: %s at (%d,%d)=%s" % (msg, x, y, got))

    # -----------------------------------------------------------------
    # Serial log
    # -----------------------------------------------------------------
    def serial_read(self):
        """Read the entire serial log file (empty string if absent)."""
        try:
            with open(self.ser, "r", errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def serial_wait(self, needle, timeout=10):
        """Poll the serial log until *needle* appears or *timeout* elapses."""
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.serial_read():
                return True
            time.sleep(0.25)
        return False

    # -----------------------------------------------------------------
    # Screen-change detection
    # -----------------------------------------------------------------
    def wait_for_change(self, timeout=10, poll=0.5):
        """Block until the framebuffer changes (compares successive screendumps)."""
        prev = self.screenshot(self.ppm + ".a")
        end = time.time() + timeout
        while time.time() < end:
            time.sleep(poll)
            cur = self.screenshot(self.ppm + ".b")
            if ppm_data(prev)[2] != ppm_data(cur)[2]:
                return True
            prev = cur
        return False
