#!/usr/bin/env python3
"""Generate a 32x32 32bpp Windows .ico file with a green circle.

Emits a single BMP-encoded icon: a solid green disc on a transparent
background. The AND mask is all zeros (every pixel is opaque-by-alpha or
transparent-by-zero-alpha), so the decoder's palette path is exercised only
for its 32bpp branch. Written to stdout; run as:

    python3 scripts/gen_ico.py > scripts/demo.ico
"""

import struct
import sys

SIZE = 32
CX = 15.5
CY = 15.5
RADIUS = 13.0
GREEN = (0, 255, 0, 255)   # B, G, R, A


def build():
    stride = ((SIZE * 32 + 31) // 32) * 4       # 128 bytes/row for 32bpp
    and_stride = ((SIZE + 31) // 32) * 4        # 4 bytes/row for 1bpp mask
    xor = bytearray()
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            dx, dy = x + 0.5 - CX, y + 0.5 - CY
            inside = dx * dx + dy * dy <= RADIUS * RADIUS
            row += bytes(GREEN if inside else (0, 0, 0, 0))
        xor += bytes(row) + b"\x00" * (stride - len(row))
    and_mask = bytearray(b"\x00" * and_stride * SIZE)

    image = xor + and_mask
    bitinfo = struct.pack(
        "<IiiHHIIiiII",
        40,                        # BITMAPINFOHEADER size
        SIZE,                      # width
        SIZE * 2,                  # height (XOR + AND)
        1,                         # planes
        32,                        # bpp
        0,                         # no compression
        len(image),
        0, 0, 0, 0,               # resolution + colors unused
    )
    dir_entry = struct.pack(
        "<BBBBHHII",
        SIZE, SIZE, 0, 0,          # width, height, colors, reserved
        1, 32,                     # planes, bpp
        len(bitinfo) + len(image),
        6 + 16,                    # offset to image data
    )
    return struct.pack("<HHH", 0, 1, 1) + dir_entry + bitinfo + image


if __name__ == "__main__":
    sys.stdout.buffer.write(build())
