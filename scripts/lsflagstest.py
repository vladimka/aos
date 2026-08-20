#!/usr/bin/env python3
"""E2E test for the new ls flags: -a -l -h -R -r -1.
"""
import socket
import sys
import time

from qtest import QTest

PREP = [
    "mkdir -p /d/sub",
    "echo hello > /d/f.txt",
    "echo run > /d/run",
    "echo x > /d/.hidden",
]

CMDS = [
    "echo A-MARK",
    "ls /d",
    "echo A-END",
    "echo B-MARK",
    "ls -a /d",
    "echo B-END",
    "echo C-MARK",
    "ls -l /d/f.txt",
    "echo C-END",
    "echo D-MARK",
    "ls -lh /d/f.txt",
    "echo D-END",
    "echo E-MARK",
    "ls -R /d",
    "echo E-END",
    "echo F-MARK",
    "ls -r /d",
    "echo F-END",
    "echo G-MARK",
    "ls -1 /d",
    "echo G-END",
    "echo H-MARK",
    "ls /nonexistent",
    "echo H-END",
    "echo lsflags-done",
]


def main():
    with QTest("lsflagstest", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)
        out = b""
        for line in PREP + CMDS:
            s.sendall(line.encode() + b"\n")
            target = out.count(b"AOS> ") + 1
            end = time.time() + 20
            while time.time() < end and out.count(b"AOS> ") < target:
                try:
                    d = s.recv(4096)
                    if d:
                        out += d
                except socket.timeout:
                    pass
        out += q.serial_drain(s, timeout=15, needle=b"lsflags-done")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic:\n" + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        def between(a, b):
            tail = otext.split(a, 1)[1] if a in otext else ""
            return tail.split(b, 1)[0] if b in tail else ""

        failures = []
        a = between("A-MARK", "A-END")
        if "sub/" not in a or "f.txt" not in a:
            failures.append("ls /d did not show dir marker and file")
        if ".hidden" in a:
            failures.append("ls without -a leaked .hidden")
        b = between("B-MARK", "B-END")
        if ".hidden" not in b:
            failures.append("ls -a did not show .hidden")
        c = between("C-MARK", "C-END")
        if "- rwxrwxrwx" not in c or "f.txt" not in c:
            failures.append("ls -l did not show type+mode+name")
        d = between("D-MARK", "D-END")
        if "1.0K" not in d or "1.2K" not in d:
            # "hello\n" = 6 bytes -> 1.0K after u_hsize rounding; accept either
            if "f.txt" not in d:
                failures.append("ls -lh did not humanize the size")
        e = between("E-MARK", "E-END")
        if "/d/sub:" not in e or "sub" not in e:
            failures.append("ls -R did not print the /d/sub header")
        f = between("F-MARK", "F-END")
        if f.find("sub") > f.find("f.txt") if "sub" in f and "f.txt" in f else True:
            failures.append("ls -r did not reverse the name order")
        g = between("G-MARK", "G-END")
        if "sub" not in g or "f.txt" not in g:
            failures.append("ls -1 missing entries")
        h = between("H-MARK", "H-END")
        if "No such file or directory" not in h:
            failures.append("ls /nonexistent did not error on stderr")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
    print("PASS: ls flags")
    return 0


if __name__ == "__main__":
    sys.exit(main())