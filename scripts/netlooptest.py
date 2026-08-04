#!/usr/bin/env python3
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-netloop.sock"
SER = "/tmp/aos-netloop.log"
PORT = 9400


def wait_for(path, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        if os.path.exists(path):
            return
        time.sleep(0.05)
    raise RuntimeError("timeout waiting for " + path)


def wait_for_serial(text, seconds=25):
    end = time.time() + seconds
    while time.time() < end:
        try:
            with open(SER, "r", errors="replace") as f:
                if text in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    return False


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def main():
    for path in (MON, SER):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "file:" + SER, "-monitor", "unix:" + MON + ",server,nowait",
        "-netdev", "socket,id=n0,listen=127.0.0.1:%d" % PORT,
        "-device", "virtio-net-pci,disable-modern=on,mac=52:54:00:12:34:56,netdev=n0",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(MON)
        if not wait_for_serial("vnet: ready"):
            raise AssertionError("virtio-net did not initialize")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(20)
        s.connect(("127.0.0.1", PORT))
        # QEMU's stream socket netdev frames every packet with a 4-byte
        # big-endian length prefix (net_socket_send / net_fill_rstate), so
        # we must send len+frame and strip the prefix on receive.
        frame = b"\xff" * 6 + b"\x52\x54\x00\x12\x34\x56" + b"\x08\x00" + b"A" * 46
        s.sendall(struct.pack(">I", len(frame)) + frame)
        hdr = recv_exact(s, 4)
        echoed = None
        if hdr is not None:
            n = struct.unpack(">I", hdr)[0]
            echoed = recv_exact(s, n)
        s.close()
        if echoed != frame:
            raise AssertionError("echo mismatch: got %r" % (echoed,))
        log = open(SER, "r", errors="replace").read()
        if "net: RX frame len=60" not in log:
            raise AssertionError("guest did not log RX frame")
        if "KERNEL PANIC" in log:
            raise AssertionError("kernel panic during net loopback")
        print("PASS: virtio-net RX + TX loopback")
        return 0
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
