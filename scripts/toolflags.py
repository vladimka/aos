#!/usr/bin/env python3
"""E2E test for the new tool flags: head -n, wc -l/-w/-c, cat multi-file,
mkdir -p, cp -r, rm -r, cd -.
"""
import socket
import sys
import time

from qtest import QTest

CMDS = [
    "echo a > /f1",
    "echo b > /f2",
    "echo CAT2-MARK",
    "cat /f1 /f2",
    "echo CAT2-END",
    "echo WC-MARK",
    "wc -l /f1",
    "echo WC-END",
    "echo HEAD1-MARK",
    "head -n 1 /f1",
    "echo HEAD1-END",
    "mkdir -p /a/b/c",
    "echo LSAB-MARK",
    "ls /a/b",
    "echo LSAB-END",
    "cp -r /a /b2",
    "echo LSB2-MARK",
    "ls /b2",
    "echo LSB2-END",
    "rm -r /b2",
    "echo RM2-MARK",
    "ls /b2",
    "echo RM2-END",
    "mkdir -p /m1/sub",
    "echo tree-file > /m1/f",
    "mv /m1 /m2",
    "echo MVDST-MARK",
    "ls /m2",
    "echo MVDST-END",
    "echo MVSRC-MARK",
    "ls /m1",
    "echo MVSRC-END",
    "export KV1=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
    "export KV2=yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",
    "echo ENVP-MARK",
    "bin/envp",
    "echo ENVP-END",
    "cd /a/b/c",
    "pwd",
    "cd -",
    "echo flags-done",
]


def main():
    with QTest("toolflags", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)
        out = b""
        for line in CMDS:
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
        out += q.serial_drain(s, timeout=15, needle=b"flags-done")
        if b"KERNEL PANIC" in out:
            raise AssertionError("kernel panic:\n" + out[-400:].decode(errors="replace"))
        otext = out.decode(errors="replace")

        def between(a, b):
            tail = otext.split(a, 1)[1] if a in otext else ""
            return tail.split(b, 1)[0] if b in tail else ""

        failures = []
        cat2 = between("CAT2-MARK", "CAT2-END")
        if "a" not in cat2 or "b" not in cat2:
            failures.append("cat /f1 /f2 did not print both files")
        wc = between("WC-MARK", "WC-END")
        if "1 /f1" not in wc:
            failures.append("wc -l /f1 did not report 1 line")
        h1 = between("HEAD1-MARK", "HEAD1-END")
        if "a" not in h1 or "b\n" in h1:
            failures.append("head -n 1 /f1 did not print just the first line")
        lsab = between("LSAB-MARK", "LSAB-END")
        if "c" not in lsab:
            failures.append("mkdir -p /a/b/c -> ls /a/b did not show c")
        lsb2 = between("LSB2-MARK", "LSB2-END")
        if "b" not in lsb2:
            failures.append("cp -r /a /b2 -> ls /b2 did not show the tree")
        rm2 = between("RM2-MARK", "RM2-END")
        if "No such file or directory" not in rm2:
            failures.append("rm -r /b2 did not remove the tree (ls should error)")
        mvdst = between("MVDST-MARK", "MVDST-END")
        if "sub" not in mvdst or "f" not in mvdst:
            failures.append("mv /m1 /m2 did not copy the tree (ls /m2 missing sub/f)")
        mvsrc = between("MVSRC-MARK", "MVSRC-END")
        if "No such file or directory" not in mvsrc:
            failures.append("mv /m1 /m2 did not remove the source (ls /m1 should error)")
        envp = between("ENVP-MARK", "ENVP-END")
        if "TERM=aos" not in envp or "KV1=xxx" not in envp or "KV2=yyy" not in envp:
            failures.append("bin/envp did not see TERM=aos + KV1/KV2 from the kernel shell")
        if "CMD:[cd] ARG:[-]" not in otext or "/a/b/c" not in otext:
            failures.append("cd - did not echo the previous directory")

        if failures:
            raise AssertionError("; ".join(failures) + ";\nout:\n" + otext[-800:])
    print("PASS: tool flags")
    return 0


if __name__ == "__main__":
    sys.exit(main())