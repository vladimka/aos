#!/usr/bin/env python3
"""E2E ps test: process table via /proc/<pid>/cmdline and /proc/<pid>/status."""
import sys

from qtest import QTest


def main():
    with QTest("pstest", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)

        s.sendall(b"ps\n"
                  b"cat /proc/1/cmdline\n"
                  b"cat /proc/1/status\n")
        out = q.serial_drain(s, timeout=30, needle=b"Abi:")

        # Background notepad with a filename argument stays alive waiting
        # for GUI events, so the second ps must show its args in CMD.
        s.sendall(b"bin/notepad note456.txt &\n"
                  b"ps\n")
        out += q.serial_drain(s, timeout=30, needle=b"note456.txt")

        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic during ps commands:\n"
                                 + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        failures = []
        lines = otext.splitlines()

        # ps prints a header with PID and CMD columns.
        if not any("PID" in ln and "CMD" in ln for ln in lines):
            failures.append("ps output has no PID/CMD header")

        # pid 1 is the window manager, spawned by the kernel at boot.
        # (Serial-console commands run in-place as pid 0, which procfs
        # deliberately does not list, so ps cannot show itself here.)
        wm_rows = [ln.split() for ln in lines if ln.split()[-1:] == ["wm"]]
        if not wm_rows:
            failures.append("ps did not list the wm process")
        else:
            row = wm_rows[0]
            if row[0] != "1":
                failures.append("wm row has pid %r, want 1" % row[0])
            if row[2] not in ("ready", "run", "sleep", "wait"):
                failures.append("wm row state %r unexpected" % row[2])
            if row[3] != "linux":
                failures.append("wm row abi %r, want linux" % row[3])

        # cmdline/status of a live pid: the window manager.
        if "\nwm" not in otext and not otext.rstrip().endswith("\nwm"):
            failures.append("cat /proc/1/cmdline did not print 'wm'")
        if "State:" not in otext or "PPid:" not in otext or "Abi:" not in otext:
            failures.append("cat /proc/1/status missing State/PPid/Abi fields")
        elif "PPid:\t0" not in otext:
            failures.append("wm PPid should be 0 (kernel-spawned)")

        # cmdline includes the spawn arguments (background notepad with a
        # filename argument stays alive waiting for GUI events).
        if "notepad note456.txt" not in otext:
            failures.append("ps did not show 'notepad note456.txt' with args")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
    print("PASS: ps lists processes via /proc/<pid>/cmdline+status")
    return 0


if __name__ == "__main__":
    sys.exit(main())
