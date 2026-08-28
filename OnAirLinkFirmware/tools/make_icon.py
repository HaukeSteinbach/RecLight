#!/usr/bin/env python3
# Copyright (c) 2026 Hauke Steinbach. All rights reserved.
# Published for inspection only, not as open source: no reuse, no derivative
# works, and no use as machine-learning training data. See LICENSE.

"""
Generates the RecLight Flasher app icon (RecLight.icns).

Draws the icon in pure Python -- a red "on air" dot on the dark RecLight
background, matching the plugin's palette -- and encodes the PNGs with zlib
directly, so building the app needs no image library and no design assets
checked into the repo.
"""

import struct
import subprocess
import sys
import zlib
from pathlib import Path

# Palette shared with the plugin UI (see Source/PluginEditor.h, OALook).
BG = (0x0D, 0x0F, 0x11)
RING = (0x2C, 0x33, 0x42)
DOT = (0xE0, 0x30, 0x30)

ICON_SIZES = [16, 32, 64, 128, 256, 512, 1024]


def render(size):
    """One RGBA image: rounded dark tile, dim ring, red dot in the middle."""
    px = bytearray(size * size * 4)
    c = (size - 1) / 2.0
    radius_tile = size * 0.46          # rounded-square corner falloff
    radius_ring = size * 0.34
    radius_dot = size * 0.20
    ring_w = max(1.0, size * 0.035)

    for y in range(size):
        for x in range(size):
            dx, dy = x - c, y - c
            dist = (dx * dx + dy * dy) ** 0.5

            # Squircle-ish mask: |dx|^4 + |dy|^4 <= r^4 gives the rounded
            # square macOS icons use, without needing a real corner radius.
            m = (abs(dx) ** 4 + abs(dy) ** 4) ** 0.25
            if m > radius_tile:
                continue  # stays fully transparent

            if dist <= radius_dot:
                r, g, b = DOT
            elif abs(dist - radius_ring) <= ring_w:
                r, g, b = RING
            else:
                r, g, b = BG

            i = (y * size + x) * 4
            px[i:i + 4] = bytes((r, g, b, 255))
    return bytes(px)


def png_bytes(size, rgba):
    """Minimal PNG encoder (8-bit RGBA, filter type 0 on every row)."""
    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("RecLight.icns")
    iconset = out.with_suffix(".iconset")
    iconset.mkdir(parents=True, exist_ok=True)

    for size in ICON_SIZES:
        data = png_bytes(size, render(size))
        # iconutil wants both the @1x name and the @2x name of half the size.
        if size <= 512:
            (iconset / f"icon_{size}x{size}.png").write_bytes(data)
        if size >= 32:
            (iconset / f"icon_{size // 2}x{size // 2}@2x.png").write_bytes(data)

    subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(out)], check=True)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
