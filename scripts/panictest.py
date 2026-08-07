#!/usr/bin/env python3
"""E2E backtrace-symbol test: run `panic`, assert the serial log shows a
kernel panic and at least one resolved frame ("name+0x.."). The kernel halts
after the panic; the test reads the log and terminates QEMU.
"""
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
SER = "/tmp/aos-panictest.ser"

def wait_for(path, seconds=10):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path): return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)

def drain(s, log, end, needle=b""):
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: break
            log += d
            if needle and needle in log: break
        except socket.timeout:
            pass
    return log

def main():
    try: os.unlink(SER)
    except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-no-reboot",
        "-serial", "unix:" + SER + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = b""
    try:
        wait_for(SER)
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        log = drain(s, b"", time.time() + 40, b"AOS>")
        s.sendall(b"panic\n")
        # The panic is triggered by a kernel-mode `int $0x0` (AOS_PANIC), so the
        # frame chain from r->ebp walks kernel frames. The exact symbols depend
        # on codegen (aos_gui_handler is inlined into syscall_handler, so the
        # top frame is usually the asm caller syscall_common): only assert that
        # frames resolve to a name+offset in the kernel text range.
        out = drain(s, b"", time.time() + 20, b"--- end ---")

        failures = []
        if b"KERNEL PANIC" not in out:
            failures.append("KERNEL PANIC banner missing")
        if b"--- backtrace ---" not in out or b"--- end ---" not in out:
            failures.append("backtrace block missing")
        else:
            frame_re = re.compile(r"eip=0x([0-9a-f]+)\s+(\w+)\+0x([0-9a-f]+)")
            frames = []
            for l in out.decode(errors="replace").splitlines():
                m = frame_re.search(l)
                if m:
                    addr = int(m.group(1), 16)
                    if 0x100000 <= addr < 0x400000:
                        frames.append(l)
            if not frames:
                failures.append("no resolved (name+offset) kernel-text backtrace frame")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + out[-800:].decode(errors="replace"))
        print("PASS: panic backtrace resolves eip to symbol names")
        return 0
    finally:
        try:
            with open("/tmp/aos-panictest.log", "wb") as f:
                f.write(out)
        except (NameError, OSError):
            pass
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
