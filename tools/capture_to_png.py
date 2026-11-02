#!/usr/bin/env python3
"""The renderer's capture_*.bin (w x h of raw texels, the Vulkan format in the name) to PNGs, for looking at.
   tools/capture_to_png.py <folder>"""
import pathlib
import re
import struct
import sys
import zlib


def half(u):
    s, e, m = u >> 15, (u >> 10) & 31, u & 1023
    v = (0 if e == 0 else (2 ** (e - 15)) * (1 + m / 1024)) if e != 31 else 1e9
    return -v if s else v


def texel(fmt, d, i):
    if fmt in (37,):  # R8G8B8A8_UNORM
        return d[i * 4], d[i * 4 + 1], d[i * 4 + 2], d[i * 4 + 3]
    if fmt == 9:  # R8_UNORM
        return d[i], d[i], d[i], 255
    if fmt == 16:  # R8G8_UNORM
        return d[i * 2], d[i * 2 + 1], 0, 255
    if fmt == 64:  # A2B10G10R10
        v = struct.unpack_from('<I', d, i * 4)[0]
        return (v & 1023) >> 2, ((v >> 10) & 1023) >> 2, ((v >> 20) & 1023) >> 2, (v >> 30) * 85
    if fmt == 97:  # RGBA16F
        r, g, b, a = struct.unpack_from('<4H', d, i * 8)
        return tuple(max(0, min(255, int(half(x) * 255))) for x in (r, g, b, a))
    if fmt == 100:  # R32F
        v = struct.unpack_from('<f', d, i * 4)[0]
        c = max(0, min(255, int(v * 255)))
        return c, c, c, 255
    return 255, 0, 255, 255


def png(path, w, h, rows):
    def chunk(t, b):
        c = struct.pack('>I', len(b)) + t + b
        return c + struct.pack('>I', zlib.crc32(t + b))
    raw = b''.join(b'\0' + r for r in rows)
    data = chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 1)) + chunk(b'IEND', b'')
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + data)


for p in sorted(pathlib.Path(sys.argv[1]).glob('capture_*.bin')):
    m = re.search(r'_(\d+)x(\d+)_f(\d+)\.bin$', p.name)
    w, h, fmt = int(m[1]), int(m[2]), int(m[3])
    d = p.read_bytes()
    rows, total, nonzero, amax = [], [0, 0, 0, 0], 0, 0
    for y in range(h):
        row = bytearray()
        for x in range(w):
            t = texel(fmt, d, y * w + x)
            row += bytes(t[:3])
            nonzero += any(t[:3])
            for k in range(4):
                total[k] += t[k]
        rows.append(bytes(row))
    n = w * h
    print(f'{p.name}: nonzero {100 * nonzero / n:.1f}% mean {[round(c / n) for c in total]}')
    png(p.with_suffix('.png'), w, h, rows)
