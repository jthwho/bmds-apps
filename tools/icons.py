#!/usr/bin/env python3
# @file      icons.py
# @copyright Jason Howard. All rights reserved.
#
# SPDX-License-Identifier: MIT
# See LICENSE file in the project root folder for license information.
#
# Draws Minesweeper's icons, minesweeper/icons/16.png and 32.png: a raised tile with a mine on it,
# the game's own look (draw.c's colours); and the I/O Manager's, ioman/icons/16.png and 32.png: a
# chip with its pins, one lit green. Python's standard library only; the PNGs are committed, so
# this runs only when an icon changes:
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


CHIP = (0x2a, 0x2e, 0x36)
CHIP_EDGE = (0x4a, 0x50, 0x5c)
PIN = (0xb8, 0xbc, 0xc4)
LIT = (0x30, 0xc0, 0x48)


def chip(side):
    """The I/O Manager's icon: a chip, its pins on all four sides, one of them lit."""
    px = [[(0, 0, 0, 0) for _ in range(side)] for _ in range(side)]
    body0, body1 = side * 0.22, side * 0.78  # the package
    pins = 4 if side < 32 else 5
    pitch = (body1 - body0) / pins
    width = max(1.0, pitch * 0.45)
    for y in range(side):
        for x in range(side):
            inside = body0 <= x < body1 and body0 <= y < body1
            if inside:
                edge = x < body0 + 1 or y < body0 + 1 or x >= body1 - 1 or y >= body1 - 1
                px[y][x] = (CHIP_EDGE if edge else CHIP) + (255,)
                continue
            for i in range(pins):
                c = body0 + pitch * (i + 0.5)
                along = abs(x - c + 0.5) < width / 2 + 0.25, abs(y - c + 0.5) < width / 2 + 0.25
                lit = i == 1
                if along[0] and (y < body0 or y >= body1) and 1 <= y < side - 1:
                    px[y][x] = ((LIT if lit and y < body0 else PIN) + (255,))
                if along[1] and (x < body0 or x >= body1) and 1 <= x < side - 1:
                    px[y][x] = PIN + (255,)
    # Pin 1's mark.
    m = max(1, side // 10)
    for y in range(int(body0) + 2, int(body0) + 2 + m):
        for x in range(int(body0) + 2, int(body0) + 2 + m):
            px[y][x] = PIN + (255,)
    return px


def main():
    for app, draw in (("minesweeper", icon), ("ioman", chip)):
        out = os.path.join(ROOT, app, "icons")
        os.makedirs(out, exist_ok=True)
        for side in (16, 32):
            png(os.path.join(out, "%d.png" % side), side, draw(side))
            print(os.path.join(out, "%d.png" % side))


if __name__ == "__main__":
    main()
