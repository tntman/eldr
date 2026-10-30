"""Writes the showcase's test images (plain Python, no imaging library):
detail.png, 512x512 sRGB colour with smooth gradients, hard-edged discs, a
checker and one-texel lines (what block compression finds hardest), and
rings.png, 512x512 gray (BC4).
Run: python make_textures.py"""
import math
import os
import struct
import zlib

N = 512


def png(path, rows, channels):
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    chunk = lambda tag, data: struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    kind = {1: 0, 3: 2}[channels]
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, kind, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def detail(x, y):
    u, v = x / N, y / N
    r = 0.5 + 0.5 * math.sin(6.0 * u + 2.0 * math.sin(5.0 * v))
    g = 0.5 + 0.5 * math.sin(7.0 * v + 3.0 * u)
    b = 0.5 + 0.5 * math.sin(9.0 * (u + v))
    for cx, cy, rad, col in ((0.3, 0.3, 0.12, (1, 0.1, 0.1)), (0.7, 0.35, 0.1, (0.1, 0.9, 0.2)),
                             (0.5, 0.72, 0.15, (0.15, 0.2, 1))):
        if (u - cx) ** 2 + (v - cy) ** 2 < rad * rad:
            r, g, b = col
    if u > 0.75 and v > 0.75:
        r = g = b = 1.0 if (x // 4 + y // 4) % 2 else 0.0
    if x % 32 == 0 or y % 32 == 0:
        r, g, b = 1 - r, 1 - g, 1 - b
    return [int(round(255 * c)) for c in (r, g, b)]


def rings(x, y):
    d = math.hypot(x - N / 2, y - N / 2)
    return [int(127.5 + 127.5 * math.sin(d * 0.25 + 0.3 * math.sin(x * 0.05)))]


here = os.path.dirname(os.path.abspath(__file__))
png(os.path.join(here, "detail.png"), [sum((detail(x, y) for x in range(N)), []) for y in range(N)], 3)
png(os.path.join(here, "rings.png"), [sum((rings(x, y) for x in range(N)), []) for y in range(N)], 1)
