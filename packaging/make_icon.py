#!/usr/bin/env python3
"""Render the mediaplayer icon to a square RGBA PNG.

Stdlib-only (zlib, struct, math) so it works on machines with no
rsvg-convert/inkscape/imagemagick. Mirrors packaging/mediaplayer.svg: a
rounded-square background with a play triangle, drawn with 4x4
supersampling for antialiased edges.

Usage: python3 packaging/make_icon.py build/icon-1024.png
"""

import math
import os
import struct
import sys
import zlib

DESIGN = 128.0
SS = 4
OUT_SIZE = 1024

BG = (24, 24, 28, 255)
ACCENT = (110, 150, 230, 255)


def fill_span(buf, row, x0, x1, width, pixel):
    x0 = max(0, x0)
    x1 = min(width, x1)
    if x1 <= x0:
        return
    start = (row * width + x0) * 4
    buf[start:start + (x1 - x0) * 4] = pixel * (x1 - x0)


def draw_rounded_rect(buf, width, x, y, w, h, r, pixel):
    for row in range(int(y), int(y + h)):
        centre = row + 0.5
        if centre < y + r:
            dy = (y + r) - centre
            inset = r - math.sqrt(max(0.0, r * r - dy * dy))
        elif centre > y + h - r:
            dy = centre - (y + h - r)
            inset = r - math.sqrt(max(0.0, r * r - dy * dy))
        else:
            inset = 0.0
        fill_span(buf, row, round(x + inset), round(x + w - inset), width, pixel)


def draw_triangle(buf, width, top, right, bottom, pixel):
    left_x = top[0]
    for row in range(int(math.ceil(top[1])), int(bottom[1])):
        centre = row + 0.5
        if centre <= right[1]:
            t = (centre - top[1]) / (right[1] - top[1])
            right_x = top[0] + t * (right[0] - top[0])
        else:
            t = (centre - right[1]) / (bottom[1] - right[1])
            right_x = right[0] + t * (bottom[0] - right[0])
        fill_span(buf, row, round(left_x), round(right_x), width, pixel)


def downsample(buf, sw, size, factor):
    total = factor * factor
    out = bytearray(size * size * 4)
    for oy in range(size):
        src_row = oy * factor
        for ox in range(size):
            sr = sg = sb = sa = 0
            for sy in range(factor):
                base = ((src_row + sy) * sw + ox * factor) * 4
                for sx in range(factor):
                    p = base + sx * 4
                    a = buf[p + 3]
                    if a:
                        sr += buf[p] * a
                        sg += buf[p + 1] * a
                        sb += buf[p + 2] * a
                        sa += a
            o = (oy * size + ox) * 4
            if sa:
                out[o] = sr // sa
                out[o + 1] = sg // sa
                out[o + 2] = sb // sa
                out[o + 3] = sa // total
    return out


def png_chunk(tag, data):
    crc = zlib.crc32(tag + data) & 0xffffffff
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)


def write_png(path, size, rgba):
    stride = size * 4
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n"
           + png_chunk(b"IHDR", header)
           + png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + png_chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def render(size):
    factor = size / DESIGN
    sw = size * SS
    buf = bytearray(sw * sw * 4)

    def s(value):
        return value * factor * SS

    draw_rounded_rect(buf, sw, s(4), s(4), s(120), s(120), s(26), bytes(BG))
    draw_triangle(buf, sw,
                  (s(50), s(36)), (s(96), s(64)), (s(50), s(92)),
                  bytes(ACCENT))
    return downsample(buf, sw, size, SS)


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: make_icon.py OUT.png\n")
        return 2
    out = argv[1]
    parent = os.path.dirname(out)
    if parent:
        os.makedirs(parent, exist_ok=True)
    write_png(out, OUT_SIZE, render(OUT_SIZE))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
