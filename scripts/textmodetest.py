#!/usr/bin/env python3
"""E2E text-mode boot test: the "AOS (text)" GRUB entry boots the kernel
without a linear framebuffer (classic VGA text console on 0xB8000).

Text mode must stay console-only: the WM is never spawned and the
virtio-gpu driver must not take over the scanout — even when a
virtio-vga device is present (the make run / qemu-debug configuration).
The serial console must stay interactive.
"""
import sys
import time

from qtest import QTest

GPU_ARGS = ["-vga", "none", "-device", "virtio-vga,disable-modern=on"]


def boot_text_entry(q):
    """Select the second GRUB entry ("AOS (text)") and wait for AOS>.

    Down highlights it, Enter boots it (digit shortcuts proved unreliable
    in this GRUB build). Presses landing before the menu exists are
    swallowed by SeaBIOS and are harmless. Stop once kernel output reaches
    serial.
    """
    s = q.serial_socket()
    out = b""
    for _ in range(6):
        time.sleep(1.5)
        q.key("down")
        q.key("ret")
        out += q.serial_drain(s, timeout=3)
        if len(out.strip()) > 20:
            break

    # Kernel is running; wait for the shell prompt.
    out += q.serial_drain(s, timeout=60, needle=b"AOS>")
    if b"AOS>" not in out:
        raise AssertionError("AOS> prompt missing after text-mode boot:\n"
                             + out[-400:].decode(errors="replace"))
    return out.decode(errors="replace")


def check_text_boot(otext):
    failures = []
    if "KERNEL PANIC" in otext:
        failures.append("kernel panic during text-mode boot")

    # vga_init() must have taken the classic VGA text path.
    if "Framebuffer: not available, using text mode" not in otext:
        failures.append("kernel did not boot without a framebuffer "
                        "(text mode not selected)")

    # Text mode must stay console-only: no WM process at all, and the
    # virtio-gpu driver must not grab the scanout away from VGA text.
    if "Window manager spawned" in otext or "\nwm:" in otext:
        failures.append("window manager started in text mode")
    if "Text mode: window manager not started." not in otext:
        failures.append("kernel did not report skipping the WM")
    if "virtio-gpu: framebuffer flip enabled" in otext:
        failures.append("virtio-gpu took over the scanout in text mode")
    return failures


def main():
    # Scenario 1: plain QEMU, no virtio devices at all.
    with QTest("textmodetest", serial_mode="socket", autoboot_grub=False) as q:
        s = q.serial_socket()
        otext = boot_text_entry(q)
        failures = check_text_boot(otext)
        if failures:
            raise AssertionError("; ".join(failures) + ";\nserial:\n"
                                 + otext[-800:])

        # The text console must stay interactive over serial.
        s.sendall(b"echo hello_text\n")
        resp = q.serial_drain(s, timeout=15, needle=b"hello_text")
        if b"hello_text" not in resp:
            raise AssertionError("serial console not interactive after "
                                 "text-mode boot:\n"
                                 + resp[-400:].decode(errors="replace"))

    # Scenario 2: same text entry but WITH a virtio-vga device attached
    # (the make run / scripts/qemu-debug.sh configuration). Choosing text
    # mode must win over GPU presence: no WM GUI on the virtio scanout.
    with QTest("textmodetest-gpu", serial_mode="socket", autoboot_grub=False,
               extra_args=GPU_ARGS) as q:
        otext = boot_text_entry(q)
        failures = check_text_boot(otext)
        if failures:
            raise AssertionError("[gpu] " + "; ".join(failures) +
                                 ";\nserial:\n" + otext[-800:])
        if "virtio-gpu: skipped (text mode)" not in otext:
            failures.append("virtio-gpu driver did not report being skipped")

    print("PASS: AOS (text) GRUB entry boots into VGA text mode "
          "(WM skipped, virtio-gpu skipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
