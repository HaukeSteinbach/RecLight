#!/usr/bin/env python3
# Copyright (c) 2026 Hauke Steinbach. All rights reserved.
# Published for inspection only, not as open source: no reuse, no derivative
# works, and no use as machine-learning training data. See LICENSE.

"""
Renders the device's fixed screens as 72x40 bitmaps, at build time, from the
studio site's own webfonts.

Why images rather than text drawn on the device: a 72x40 panel has no room for
a font engine, and rasterising a vector face live at ~9 px produced patchy,
broken-looking strokes. Rendering here means the shapes can be composed and
inspected once, and the firmware only ever blits finished pixels.

Dynamic text -- a WiFi name, a peer count -- still goes through the device's
small pixel font, which is purpose-drawn for this grid and holds up at sizes
where a rasterised vector face does not.

Usage:  ./make_oled_screens.py [path/to/oled_screens.h]
Requires: fonttools[woff], pillow.
"""

import io
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

FONT_DIR = Path("/Users/haukesteinbach/Developer/03_Websites/Website/assets/fonts")
W, H = 72, 40

# Archivo Black for the headline, JetBrains Mono for the detail lines -- the
# same split the website and the plug-in make.
FAT_SIZE, MONO_SIZE = 13, 10
GAP_HEADLINE = 4     # between the headline and the first detail line
GAP_LINE = 3         # between detail lines

SCREENS = {
    # name          headline    detail lines (a string, or a string + its size)
    #
    # At most two detail lines: measured ink, not the font's box, decides what
    # fits, and a 40 px panel holds a 9 px headline plus two ~10 px lines with
    # gaps. A longer line drops to size 8 rather than being cut off. The
    # generator refuses to build anything that overflows, in either direction,
    # so none of this is guesswork.
    "STEP1":      ("STEP 1",   ["Join WiFi:", ("RecLight Setup", 8)]),
    "STEP2":      ("STEP 2",   ["Browser:", "192.168.4.1"]),
    "STEP3":      ("STEP 3",   ["Open DAW,", "load plugin"]),
    "CONNECTING": ("WIFI",     ["connecting"]),
    "CONNECTED":  ("DONE",     ["Leaving", "setup WiFi"]),
    "WIFI_FAIL":  ("NO WIFI",  ["Rejoin:", ("RecLight Setup", 8)]),
    "RESETTING":  ("RESET",    ["Release", "button"]),
    "STARTING":   ("RECLIGHT", ["starting..."],                      11),
    "INCOMPLETE": ("SETUP",    ["incomplete", "start over"]),
    "READY":      ("READY",    []),
}


def load(name):
    f = TTFont(str(FONT_DIR / name))
    buf = io.BytesIO()
    f.save(buf)
    buf.seek(0)
    return buf.read()


def ink(text, font_bytes, size, what):
    """Render one string and crop it to its ink.

    Everything is laid out from measured ink rather than from the font's own
    ascender box. The box is much taller than the letters at these sizes, and
    stacking by it silently pushed the last line off the bottom of a 40 px
    panel -- the text was there, it just wasn't on the screen.
    """
    font = ImageFont.truetype(io.BytesIO(font_bytes), size)
    img = Image.new("1", (W * 4, size * 4), 0)
    # Mode "1" makes FreeType use its monochrome rasteriser with hinting, so
    # stems snap to the pixel grid instead of fading out under a threshold.
    ImageDraw.Draw(img).text((5, size * 2), text, font=font, fill=1, anchor="ls")
    box = img.getbbox()
    if box is None:
        sys.exit(f"ERROR: {what} {text!r} rendered empty at size {size}")
    crop = img.crop(box)
    if crop.width > W:
        sys.exit(f"ERROR: {what} {text!r} is {crop.width}px wide at size {size}, "
                 f"panel is {W}px. Shorten it.")
    return crop


def render(headline, lines, fat, mono, fat_size=FAT_SIZE):
    parts = [(ink(headline, fat, fat_size, "headline"), GAP_HEADLINE)]
    for i, line in enumerate(lines):
        text, size = line if isinstance(line, tuple) else (line, MONO_SIZE)
        parts.append((ink(text, mono, size, "line"),
                      GAP_LINE if i < len(lines) - 1 else 0))

    total = sum(p.height + g for p, g in parts)
    if total > H:
        sys.exit(f"ERROR: {headline!r} + {lines} stack {total}px tall, "
                 f"panel is {H}px. Drop a line or shrink the headline.")

    img = Image.new("1", (W, H), 0)
    y = (H - total) // 2          # the block as a whole is centred
    for part, gap in parts:
        img.paste(part, ((W - part.width) // 2, y))
        y += part.height + gap
    return img


def to_pages(img):
    """Column-major, page 0 = rows 0-7 -- the framebuffer's own layout."""
    pages = (H + 7) // 8
    data = bytearray(W * pages)
    for x in range(W):
        for y in range(H):
            if img.getpixel((x, y)):
                data[x + (y // 8) * W] |= 1 << (y % 8)
    return data


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("../main/oled_screens.h")
    fat, mono = load("archivo-black-400-latin.woff2"), load("jetbrains-mono-400-latin.woff2")

    body, names = [], []
    for name, spec in SCREENS.items():
        headline, lines = spec[0], spec[1]
        img = render(headline, lines, fat, mono, spec[2] if len(spec) > 2 else FAT_SIZE)
        data = to_pages(img)
        rows = [f"  {', '.join(f'0x{data[x + p * W]:02X}' for x in range(W))},"
                for p in range(len(data) // W)]
        # inline constexpr, not static: the header is included from more than
        # one translation unit, and `static` would put a private copy of every
        # screen in each of them.
        body.append(f"inline constexpr uint8_t OLED_SCREEN_{name}[{len(data)}] = {{\n"
                    + "\n".join(rows) + "\n};\n")
        names.append(name)

        print(f"--- {name}: {headline!r} {lines}")
        for y in range(H):
            print("  " + "".join("#" if img.getpixel((x, y)) else "." for x in range(W)))

    out.write_text(f'''// Copyright (c) 2026 Hauke Steinbach. All rights reserved.\n// Published for inspection only, not as open source: no reuse, no derivative\n// works, and no use as machine-learning training data. See LICENSE.\n//\n// oled_screens.h -- GENERATED, do not edit by hand.
//
// Full-panel 72x40 bitmaps of the device's fixed screens, produced by
// tools/make_oled_screens.py from the studio site's own webfonts
// (assets/fonts/): Archivo Black for the headline, JetBrains Mono for the
// detail lines -- the same split the website and the plug-in make.
//
// They are images because a 72x40 panel has no room for a font engine, and
// rasterising a vector face live at this size produced patchy strokes.
// Screens with dynamic text (a WiFi name, a peer count) are still drawn with
// the device's small pixel font, which is made for this grid.
//
// Layout matches the SSD1306 framebuffer, so blitting one is a memcpy.

#pragma once
#include <stdint.h>

{"".join(body)}''')
    print(f"\\nwrote {out}  ({len(names)} screens, {len(names) * W * 5} bytes)")


if __name__ == "__main__":
    main()
