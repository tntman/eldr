"""Regenerates the test textures (stdlib only): python make_textures.py"""
import math
import os
import struct
import zlib


def write_png(path, size, pixel):
    # RGB, or RGBA when pixel() returns four values.
    channels = len(pixel(0, 0))
    rows = b"".join(
        b"\x00" + bytes(c for x in range(size) for c in pixel(x, y)) for y in range(size)
    )

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6 if channels == 4 else 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows)))
        f.write(chunk(b"IEND", b""))


def quadrants(x, y):
    # PNG rows run top-down, so y < 32 is the top half (st v > 0.5).
    top, right = y < 32, x >= 32
    if top:
        return (255, 255, 255) if right else (40, 60, 255)
    return (40, 200, 40) if right else (230, 40, 40)


def ridges(x, y):
    # Tangent-space normals tilted along u: vertical ridges.
    nx = 0.6 * math.sin(x / 64 * 8 * 2 * math.pi)
    nz = math.sqrt(1 - nx * nx)
    return (round((nx * 0.5 + 0.5) * 255), 128, round((nz * 0.5 + 0.5) * 255))


def udim_tile(color):
    # A checker in the tile's colour inside a white border, to see the seams.
    def pixel(x, y):
        if min(x, y) < 3 or max(x, y) > 60:
            return (255, 255, 255)
        return color if (x // 8 + y // 8) % 2 else tuple(c // 2 for c in color)
    return pixel


def cutout(x, y):
    # A green ring: opaque between radius 12 and 28 of 32, clear elsewhere.
    r = math.hypot(x - 31.5, y - 31.5)
    return (40, 160, 50, 255 if 12 <= r <= 28 else 0)


here = os.path.dirname(os.path.abspath(__file__))
write_png(os.path.join(here, "quadrants.png"), 64, quadrants)
write_png(os.path.join(here, "ridges_normal.png"), 64, ridges)
write_png(os.path.join(here, "cutout.png"), 64, cutout)
# UDIM tiles 1001 (u 0..1, v 0..1), 1002 (u 1..2) and 1011 (v 1..2); 1012
# is left out on purpose.
for tile, color in ((1001, (230, 120, 20)), (1002, (20, 170, 170)), (1011, (150, 60, 200))):
    write_png(os.path.join(here, f"udim.{tile}.png"), 64, udim_tile(color))
