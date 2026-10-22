"""Regenerates the test textures (stdlib only): python make_textures.py"""
import math
import os
import struct
import zlib


def write_png(path, size, pixel):
    rows = b"".join(
        b"\x00" + bytes(c for x in range(size) for c in pixel(x, y)) for y in range(size)
    )

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)))
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


here = os.path.dirname(os.path.abspath(__file__))
write_png(os.path.join(here, "quadrants.png"), 64, quadrants)
write_png(os.path.join(here, "ridges_normal.png"), 64, ridges)
