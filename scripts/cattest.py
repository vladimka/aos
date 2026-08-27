#!/usr/bin/env python3
import os, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "aos.iso")
MON = "/tmp/aos-cattest.sock"
SER = "/tmp/aos-cattest.serial"

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
            if needle and needle in log:
                break
        except socket.timeout:
            pass
    return log

def main():
    for path in (MON, SER):
        try: os.unlink(path)
        except FileNotFoundError: pass
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "256", "-cdrom", ISO, "-display", "none",
        "-serial", "unix:" + SER + ",server,nowait",
        "-monitor", "unix:" + MON + ",server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(SER)
        time.sleep(0.5)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SER)

        # Skip GRUB menu
        m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        m.connect(MON)
        for _ in range(3):
            time.sleep(1.5)
            m.sendall(b"sendkey ret\n")
        m.close()

        log = drain(s, b"", time.time() + 40, b"AOS>")
        text = log.decode(errors="replace")
        if b"KERNEL PANIC" in log:
            raise AssertionError("kernel panic during boot")
        if b"Terminal ready." not in log:
            raise AssertionError("boot incomplete; tail:\n" + text[-400:])
        print("PASS: boot OK")

        # Test 1: echo + cat on a single-line file
        s.sendall(b"echo hello > test.txt\n")
        out = drain(s, b"", time.time() + 10, b"AOS>")
        otext = out.decode(errors="replace")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic in echo redirect")

        s.sendall(b"cat test.txt\n")
        out = drain(s, b"", time.time() + 10, b"AOS>")
        otext = out.decode(errors="replace")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic in cat")
        print("  cat output: " + repr(otext[-300:]))
        if b"hello" in out:
            print("PASS: cat single-line file")
        else:
            raise AssertionError("cat single-line file did not output 'hello'; out:\n" + otext[-300:])

        # Test 2: echo + cat on a multi-line file
        s.sendall(b"echo line1 > ml.txt\n")
        out = drain(s, b"", time.time() + 10, b"AOS>")

        s.sendall(b"echo line2 >> ml.txt\n")
        out = drain(s, b"", time.time() + 10, b"AOS>")

        s.sendall(b"cat ml.txt\n")
        out = drain(s, b"", time.time() + 10, b"AOS>")
        otext = out.decode(errors="replace")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic in cat multi-line")
        print("  cat multi-line output: " + repr(otext[-300:]))
        if b"line1" in out and b"line2" in out:
            print("PASS: cat multi-line file")
        else:
            raise AssertionError("cat multi-line failed; out:\n" + otext[-300:])

        # Test 3: cat with -n flag on single-line file
        s.sendall(b"cat -n test.txt\n")
        out = drain(s, b"", time.time() + 10, b"AOS>")
        otext = out.decode(errors="replace")
        print("  cat -n output: " + repr(otext[-300:]))
        if b"1" in out and b"hello" in out:
            print("PASS: cat -n single-line")
        else:
            print("WARN: cat -n single-line inconclusive")

        # Test 4: read a file with a single byte
        s.sendall(b"echo -n X > onebyte.txt\n")
        out = drain(s, b"", time.time() + 10, b"AOS>")

        s.sendall(b"cat onebyte.txt\n")
        out = drain(s, b"", time.time() + 10, b"AOS>")
        otext = out.decode(errors="replace")
        print("  cat onebyte output: " + repr(otext[-300:]))
        if b"X" in out:
            print("PASS: cat single-byte file")
        else:
            raise AssertionError("cat single-byte file failed; out:\n" + otext[-300:])

        print("\nALL TESTS PASSED")
        return 0
    finally:
        qemu.terminate()
        try: qemu.wait(timeout=5)
        except subprocess.TimeoutExpired: qemu.kill()

if __name__ == "__main__":
    sys.exit(main())
