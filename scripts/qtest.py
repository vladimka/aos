#!/usr/bin/env python3
"""Shared QEMU test harness for the AOS regression suite.

The ``QTest`` class owns one ``qemu-system-i386`` process: it boots the ISO,
exposes the HMP monitor for input/screendump, tracks absolute mouse position
in a state file, and reads the serial log. Two serial backends are supported:

* ``serial_mode="file"`` (default): kernel writes to a plain file. Best for
  tests that panic the kernel (the log is preserved after the guest halts).
* ``serial_mode="socket"``: kernel writes over a Unix socket. The test can
  connect and read live output (needed to queue commands before the WM
  captures serial input).

Typical GUI test::

    with QTest("mytest") as q:
        q.boot_and_ready()
        q.dock_spawn_term()
        q.type_text("echo hi\\n")
        q.assert_pixel(300, 100, (16, 16, 16), "content bg")

Typical serial test::

    with QTest("mytest", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)
        s.sendall(b"cat /proc/klog\\n")
        out = q.serial_drain(socket=s, needle=b"klog: ready")
"""
import json
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
                 boot_wait=5, serial_mode="file", extra_args=None,
                 qmp_socket=None, autoboot_grub=True):
        self.name = name
        # The shipped grub.cfg waits at the boot menu (default GRUB_TIMEOUT).
        # Tests that boot the default entry fire a few fallback Enters early
        # so they skip the menu wait entirely; tests that pick a specific
        # entry pass autoboot_grub=False.
        self.autoboot_grub = autoboot_grub
        self.mon = monitor_socket or f"/tmp/aos-{name}.sock"
        self.serial_mode = serial_mode
        if serial_mode == "socket":
            self.ser = serial_log or f"/tmp/aos-{name}.ser"
        else:
            self.ser = serial_log or f"/tmp/aos-{name}.log"
        self.mouse_state = mouse_state or f"/tmp/aos-{name}-mouse.state"
        self.ppm = ppm or f"/tmp/aos-{name}.ppm"
        self.iso = iso or DEFAULT_ISO
        self.mouse_boot = mouse_boot
        self.boot_wait = boot_wait
        self.extra_args = extra_args or []
        self.qmp_sock = qmp_socket
        self.qemu = None
        self._last_ppm = None
        self._serial_sock = None

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
        if self.qmp_sock:
            try:
                os.unlink(self.qmp_sock)
            except FileNotFoundError:
                pass
        self._write_state(*self.mouse_boot)
        cmd = [
            "qemu-system-i386", "-m", "256", "-cdrom", self.iso,
            "-display", "none",
        ]
        if self.serial_mode == "socket":
            cmd += ["-serial", "unix:" + self.ser + ",server,nowait"]
        else:
            cmd += ["-serial", "file:" + self.ser]
        cmd += ["-monitor", "unix:" + self.mon + ",server,nowait"]
        if self.qmp_sock:
            cmd += ["-qmp", "unix:" + self.qmp_sock + ",server,nowait"]
        if extra_args:
            cmd += extra_args
        if self.extra_args:
            cmd += self.extra_args
        self.qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        wait_for(self.mon)
        if self.serial_mode == "socket":
            # Connect the serial socket BEFORE the autoboot Enters: a unix
            # chardev drops guest COM1 output while no client is connected,
            # so a late connect would lose the early boot lines (and the AOS>
            # prompt itself) that socket tests assert on.
            self.serial_socket()
        if self.autoboot_grub:
            # Fire fallback Enters while the GRUB menu may be showing: the
            # first one(s) land in SeaBIOS (no-op), one hits the menu and
            # boots the default entry immediately. Later presses are eaten
            # by the WM/shell as harmless empty commands.
            for _ in range(3):
                time.sleep(1.5)
                self.hmp("sendkey ret")
        time.sleep(boot_wait)
        return self

    def stop(self):
        """Terminate the QEMU process."""
        if self.qemu is None:
            return
        if self._serial_sock is not None:
            try:
                self._serial_sock.close()
            except OSError:
                pass
            self._serial_sock = None
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

    def qmp(self, obj):
        """Send a QMP command object and return its response dict.

        Requires ``qmp_socket`` (an extra ``-qmp unix:...`` monitor) passed to
        the constructor. Each call opens a fresh connection: reads the QMP
        greeting, runs ``qmp_capabilities``, sends *obj*, and returns the
        first ``{"return": ...}`` / ``{"error": ...}`` response.
        """
        if self.qmp_sock is None:
            raise RuntimeError("qmp() requires qmp_socket=")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(5)
            s.connect(self.qmp_sock)
            data = b""
            while b'"QMP"' not in data:
                data += s.recv(4096)
            s.sendall(b'{"execute": "qmp_capabilities"}\n')
            while b"\n" not in data:
                data += s.recv(4096)
            s.sendall(json.dumps(obj).encode() + b"\n")
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
                for line in data.split(b"\n"):
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        o = json.loads(line)
                    except (json.JSONDecodeError, ValueError):
                        continue
                    if "return" in o or "error" in o:
                        return o
            raise RuntimeError("QMP command timed out: " + json.dumps(obj))

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
        keys = {
            "\n": "ret", " ": "spc", "/": "slash",
            ">": "shift-dot", "<": "shift-comma",
            "|": "shift-backslash", "?": "shift-slash",
            ":": "shift-semicolon", ";": "semicolon",
            "_": "shift-minus", "!": "shift-1", "~": "shift-grave",
        }
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

    def count_text_pixels(self, path, x0, y0, x1, y1):
        """Count bright-text pixels in *path* rectangle (see ``count_bright``)."""
        return count_bright(path, x0, y0, x1, y1)

    # -----------------------------------------------------------------
    # Serial log
    # -----------------------------------------------------------------
    def serial_read(self):
        """Read the entire serial log file (empty string if absent).

        Only valid when ``serial_mode="file"``; socket-mode tests use
        :meth:`serial_socket` + :meth:`serial_drain` instead.
        """
        try:
            with open(self.ser, "r", errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def serial_wait(self, needle, timeout=10):
        """Poll the serial log until *needle* appears or *timeout* elapses.

        Only valid when ``serial_mode="file"``.
        """
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.serial_read():
                return True
            time.sleep(0.25)
        return False

    def serial_socket(self):
        """Connect to the serial Unix socket (``serial_mode="socket"`` only).

        Returns a connected ``socket.socket`` with a 1-second timeout.
        """
        if self.serial_mode != "socket":
            raise RuntimeError("serial_socket() requires serial_mode='socket'")
        if self._serial_sock is not None:
            return self._serial_sock
        wait_for(self.ser)
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(self.ser)
        self._serial_sock = s
        return s

    def serial_drain(self, sock, timeout=30, needle=b""):
        """Read from *sock* until *needle* or *timeout`` elagues.

        Returns the accumulated bytes. Re-raises nothing on timeout -- the
        caller checks the result.
        """
        out = b""
        end = time.time() + timeout
        while time.time() < end:
            try:
                d = sock.recv(4096)
                if not d:
                    break
                out += d
                if needle and needle in out:
                    break
            except socket.timeout:
                pass
        return out

    def boot_and_ready(self, socket=None, panic_ok=False):
        """Wait for the ``AOS>`` prompt on *socket* (or serial log).

        After the prompt appears, sleeps ``boot_wait`` seconds for the WM to
        settle. Raises if ``KERNEL PANIC`` is seen (unless *panic_ok*).
        Returns the bytes drained from *socket* in socket mode, else ``None``.
        """
        if socket is not None:
            out = self.serial_drain(socket, timeout=40, needle=b"AOS>")
            if b"KERNEL PANIC" in out and not panic_ok:
                raise AssertionError("kernel panic during boot:\n"
                                     + out[-400:].decode(errors="replace"))
            time.sleep(self.boot_wait)
            return out
        if not self.serial_wait("AOS>", timeout=40):
            raise AssertionError("AOS> prompt missing after boot")
        if "KERNEL PANIC" in self.serial_read() and not panic_ok:
            raise AssertionError("kernel panic during boot")
        time.sleep(self.boot_wait)
        return None

    # -----------------------------------------------------------------
    # Common GUI helpers
    # -----------------------------------------------------------------
    def dock_spawn_term(self):
        """Click the dock launcher to spawn a terminal window and focus it.

        The dock term launcher is at screen (472, 724); a fresh terminal
        opens at ~(20, 20). The terminal process takes a variable time to
        initialize its event loop and register for keyboard events with the
        WM, so we poll the screen until the terminal window is visible
        (its content-bg pixel at (30,100) goes dark) before returning.
        """
        self.mouse_click(472, 724)
        # Poll until the terminal window appears (content bg = 0x101010).
        # The desktop gradient at (30,100) is brighter, so a dark pixel means
        # the terminal is up. Fall back to a fixed wait if polling fails.
        poll = self.ppm + ".poll"
        for _ in range(40):
            time.sleep(0.25)
            try:
                self.screenshot(poll)
                if self.pixel(30, 100, path=poll) == (16, 16, 16):
                    time.sleep(0.5)
                    return
            except RuntimeError:
                pass
        time.sleep(2)

    def term_text_band(self):
        """Return the (x0, y0, x1, y1) rectangle covering terminal text.

        Terminal content starts at x=21, y=39; 26 rows of 16 px each span to
        y=39+26*16. Content width is 640 px (x=21..660).
        """
        return (21, 39, 660, 39 + 26 * 16)

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
