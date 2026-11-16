#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 Leo Kuroshita (@kurogedelic)
"""Draws the nestopia-cz launcher icons (a dark tile with a plain two-button pad) as PNG files, no libraries needed."""
import math, struct, sys, zlib

def png(path, S):
    rows = []
    for y in range(S):
        row = bytearray([0])
        for x in range(S):
            u, v = x / (S - 1), y / (S - 1)
            r, g, b, a = 28, 28, 36, 255
            cx, cy = abs(u - 0.5), abs(v - 0.5)
            if cx > 0.36 and cy > 0.36 and math.hypot(cx - 0.36, cy - 0.36) > 0.14: a = 0
            if 0.12 < u < 0.88 and 0.32 < v < 0.72: r, g, b = 150, 150, 156                        # the pad body
            if (0.20 < u < 0.38 and 0.47 < v < 0.57) or (0.25 < u < 0.33 and 0.40 < v < 0.64): r, g, b = 40, 40, 48   # d-pad cross
            for bx in (0.62, 0.76):                                                                  # two round buttons
                if math.hypot(u - bx, v - 0.52) < 0.065: r, g, b = 200, 50, 60
            if 0.44 < u < 0.54 and 0.58 < v < 0.62: r, g, b = 60, 60, 70                              # select
            row += bytes([r, g, b, a])
        rows.append(bytes(row))
    raw = b"".join(rows)
    def chunk(t, d): c = struct.pack(">I", len(d)) + t + d; return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

for name, size in (("nestopia-cz.png", 80), ("nestopia-cz_80.png", 80), ("nestopia-cz_100.png", 100)):
    png(sys.argv[1] + "/" + name, size)
