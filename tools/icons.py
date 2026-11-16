#!/usr/bin/env python3
# @file      icons.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Draws Minesweeper's icons, minesweeper/icons/16.png and 32.png: a raised tile with a mine on it,
# the game's own look (draw.c's colours). Python's standard library only; the PNGs are committed,
# so this runs only when the icon changes:
#
#   tools/icons.py

import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HIDDEN = (0x8f, 0xa3, 0xb8)
LIGHT = (0xc4, 0xd2, 0xe0)
DARK = (0x56, 0x66, 0x78)
MINE = (0x20, 0x22, 0x28)
SHINE = (0xf0, 0xf0, 0xf0)


def png(path, side, pixels):
    """Writes an RGBA PNG of side x side from pixels[y][x] = (r, g, b, a)."""
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in pixels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def icon(side):
    px = [[HIDDEN + (255,) for _ in range(side)] for _ in range(side)]
    bevel = 3 if side >= 32 else 2
    for y in range(side):
        for x in range(side):
            if x < bevel or y < bevel:
                px[y][x] = LIGHT + (255,)
            if x >= side - bevel or y >= side - bevel:
                px[y][x] = DARK + (255,)
    # The mine: spikes, a body, a shine (draw.c's mine()).
    c = (side - 1) / 2
    r = side * 0.3
    for y in range(side):
        for x in range(side):
            dx, dy = x - c, y - c
            d2 = dx * dx + dy * dy
            spike = (abs(dx) < side / 24 + 0.5 or abs(dy) < side / 24 + 0.5 or abs(abs(dx) - abs(dy)) < side / 20 + 0.5)
            if d2 <= r * r or (spike and d2 <= (r * 1.45) ** 2):
                px[y][x] = MINE + (255,)
            sx, sy = x - (c - r / 3), y - (c - r / 3)
            if sx * sx + sy * sy <= max(1.0, (r / 4)) ** 2:
                px[y][x] = SHINE + (255,)
    return px


def main():
    out = os.path.join(ROOT, "minesweeper", "icons")
    os.makedirs(out, exist_ok=True)
    for side in (16, 32):
        png(os.path.join(out, "%d.png" % side), side, icon(side))
        print(os.path.join(out, "%d.png" % side))


if __name__ == "__main__":
    main()
