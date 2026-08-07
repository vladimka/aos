#!/usr/bin/env python3
import os
import socket
import struct
import subprocess
import sys
import time

from qtest import wait_for

PORT = 9400
SER = "/tmp/aos-netloop.log"


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def main():
    try:
        os.unlink(SER)
    except FileNotFoundError:
        pass
    cmd = [
        "qemu-system-i386", "-m", "256", "-cdrom", "aos.iso", "-display", "none",
        "-serial", "file:" + SER,
        "-monitor", "unix:/tmp/aos-netloop.sock,server,nowait",
        "-netdev", "socket,id=n0,listen=127.0.0.1:%d" % PORT,
        "-device", "virtio-net-pci,disable-modern=on,mac=52:54:00:12:34:56,netdev=n0",
    ]
    qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for("/tmp/aos-netloop.sock")
        end = time.time() + 25
        ready = False
        while time.time() < end:
            try:
                with open(SER, "r", errors="replace") as f:
                    if "vnet: ready" in f.read():
                        ready = True
                        break
            except FileNotFoundError:
                pass
            time.sleep(0.2)
        if not ready:
            raise AssertionError("virtio-net did not initialize")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(20)
        s.connect(("127.0.0.1", PORT))
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
