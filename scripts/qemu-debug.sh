#!/usr/bin/env bash
# Launch AOS under QEMU for interactive debugging via VNC + QMP + serial
# sockets (connect with the qemu-vnc MCP tools: vm_connect vnc_port=5907,
# qmp_socket=/tmp/aos-debug.qmp, serial_socket=/tmp/aos-debug.serial).
#
# Usage: scripts/qemu-debug.sh [extra qemu args...]
set -euo pipefail
cd "$(dirname "$0")/.."

ISO=${ISO:-aos.iso}
DISPLAY_NO=${DISPLAY_NO:-7}
QMP=${QMP:-/tmp/aos-debug.qmp}
SERIAL=${SERIAL:-/tmp/aos-debug.serial}

rm -f "$QMP" "$SERIAL"

echo "VNC:    :$DISPLAY_NO  (127.0.0.1:$((5900 + DISPLAY_NO)))"
echo "QMP:    $QMP"
echo "Serial: $SERIAL"
echo "Ctrl+C stops QEMU."

exec qemu-system-i386 -m 256 -rtc base=localtime -cdrom "$ISO" \
  -vga none -device virtio-vga,disable-modern=on \
  -display none -vnc ":$DISPLAY_NO" \
  -monitor none \
  -qmp "unix:$QMP,server,nowait" \
  -serial "unix:$SERIAL,server,nowait" \
  -device piix3-usb-uhci -device usb-tablet \
  "$@"
