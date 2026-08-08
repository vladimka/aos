#!/usr/bin/env python3
import sys

from qtest import QTest


def main():
    with QTest("strace", serial_mode="socket") as q:
        s = q.serial_socket()
        q.boot_and_ready(socket=s)

        s.sendall(b"strace linrun\n")
        out = q.serial_drain(s, timeout=45, needle=b"TDMP:done")
        tail = out[-1200:]
        try:
            if b"KERNEL PANIC" in out:
                raise AssertionError("kernel panic during strace linrun:\n"
                                     + tail.decode(errors="replace"))
            if b"== pid 0 ==" not in out:
                raise AssertionError("strace session did not dump == pid 0 ==\n"
                                     + tail.decode(errors="replace"))
            if b"== pid 2 ==" not in out:
                raise AssertionError(
                    "strace did not collect the spawned child\n"
                    "(saw %r in the dump)\n%s"
                    % ([l for l in out.split(b"\n") if b"==" in l and b"pid" in l],
                       tail.decode(errors="replace")))
            for probe in ("spawn(", "writev(", "exit_group("):
                if probe.encode() not in out:
                    raise AssertionError("strace output missing %r\n%s"
                                         % (probe, tail.decode(errors="replace")))
        except AssertionError:
            raise
    print("PASS: strace shows AOS_EXT spawn + linux writev/exit_group, child collected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
