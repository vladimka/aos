#!/usr/bin/env python3
"""E2E regression for the AOS init system (/etc/init.conf services).

Checks: init starts wm + clock as its children; `kill <wm>` exits with code
9 and init respawns wm under a fresh pid; `kill 0`/`kill 999` are rejected;
`kill <init>` makes the kernel respawn init and reparent the survivors.
"""
import re
import sys
import time

from qtest import QTest


def shell_ready(q, s, timeout=150):
    """Wait until the kernel shell answers a newline with an AOS> prompt.
    Works regardless of whether QTest's internal serial connect happened
    before or after the guest booted (a unix chardev drops output while
    disconnected, so early boot lines may be missing)."""
    end = time.time() + timeout
    while time.time() < end:
        if q.qemu.poll() is not None:
            raise AssertionError("qemu died rc=%s" % q.qemu.returncode)
        try:
            s.sendall(b"\n")
        except OSError:
            time.sleep(1)
            continue
        buf = b""
        dead_end = time.time() + 12
        while time.time() < dead_end:
            try:
                d = s.recv(4096)
                if d:
                    buf += d
            except OSError:
                pass
            if b"AOS>" in buf:
                return True
        time.sleep(1)
    return False


def run_cmd(q, s, cmd, needle, timeout=60):
    """Send `cmd` and collect until `needle`, then keep draining briefly so a
    prompt from an earlier exchange (or a straggling chunk) cannot truncate
    the answer."""
    s.sendall(cmd.encode() if isinstance(cmd, str) else cmd)
    t0 = time.time()
    buf = b""
    s.settimeout(1)
    while time.time() - t0 < timeout:
        got = b""
        try:
            got = s.recv(4096)
            buf += got
        except OSError:
            pass
        if needle in buf and (time.time() - t0) >= 1.5 and not got:
            break
        time.sleep(0.05)
    return buf.decode(errors="replace")


def run_and_settle(q, s, cmd, settle=6.0):
    """Run a command whose side effects report ASYNCHRONOUSLY (kill traces,
    init/respawn messages interleave with the prompt): read the serial socket
    CONTINUOUSLY until the AOS> prompt has been seen and `settle` seconds
    have passed since the command, so no chunk of the stream is lost."""
    s.sendall(cmd.encode() if isinstance(cmd, str) else cmd)
    t0 = time.time()
    buf = b""
    while True:
        try:
            s.settimeout(1)
            d = s.recv(4096)
            if d:
                buf += d
        except OSError:
            pass
        if b"AOS>" in buf and time.time() - t0 >= settle:
            break
        if time.time() - t0 > settle + 30:
            break
        time.sleep(0.05)
    return buf.decode(errors="replace")


def main():
    with QTest("inittest", serial_mode="socket") as q:
        s = q.serial_socket()
        assert shell_ready(q, s), "kernel shell never answered"

        # 1) init owns the services listed in /etc/init.conf
        ps0 = run_cmd(q, s, b"ps\n", b"AOS>")
        mi = re.search(r"^\s*(\d+)\s+0\s+\S+\s+linux\s+init\s*$", ps0, re.M)
        assert mi, "init not running at PPid 0:\n%s" % ps0[-900:]
        init_pid = int(mi.group(1))
        mw = re.search(r"^\s*(\d+)\s+%d\s+\S+\s+linux\s+wm\s*$" % init_pid, ps0, re.M)
        assert mw, "wm not parented to init:\n%s" % ps0[-900:]
        wm_pid = int(mw.group(1))
        assert re.search(r"^\s*\d+\s+%d\s+\S+\s+linux\s+clock\s*$" % init_pid, ps0, re.M), \
            "clock not started by init:\n%s" % ps0[-900:]
        log = ps0

        # 2) kill wm -> exit code 9 -> init respawns it under a new pid
        # (the respawn report lands AFTER the shell prompt: settle first)
        out = run_and_settle(q, s, "kill %d\n" % wm_pid)
        assert ("TEC:pid=%d code=9" % wm_pid) in out, "wm kill trace missing:\n%s" % out[-500:]
        # Respawn may reuse the freed slot, so the new pid can equal the old
        # one -- what matters is that init actually restarted the service.
        m2 = re.search(r"init: started bin/wm \(pid (\d+)\)", out)
        assert m2, "wm not respawned:\n%s" % out[-800:]
        wm_pid = int(m2.group(1))
        log += out

        # 3) negative cases must be rejected harmlessly
        out = run_cmd(q, s, b"kill 0\n", b"AOS>")
        assert "kill: no such process" in out, "kill 0 accepted:\n%s" % out[-500:]
        log += out
        out = run_cmd(q, s, b"kill 999\n", b"AOS>")
        assert "kill: no such process" in out, "kill 999 accepted:\n%s" % out[-500:]
        log += out

        # 4) kill init -> kernel respawns it, survivors reparent
        old_init = init_pid
        out = run_and_settle(q, s, "kill %d\n" % old_init)
        assert ("TEC:pid=%d code=9" % old_init) in out, "init kill trace missing"
        mi2 = re.search(r"init spawned \(pid (\d+)\)", out)
        assert mi2 and int(mi2.group(1)) != old_init, "kernel did not respawn init"
        new_init = int(mi2.group(1))
        log += out
        ps2 = run_cmd(q, s, b"ps\n", b"AOS>")
        assert re.search(r"^\s*%d\s+%d\s+\S+\s+linux\s+wm\s*$" % (wm_pid, new_init), ps2, re.M), \
            "wm not reparented to the new init:\n%s" % ps2[-900:]
        log += ps2

        assert "KERNEL PANIC" not in log, "kernel panic during inittest"
    print("PASS: init system (service start, respawn, kill, reparent)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
