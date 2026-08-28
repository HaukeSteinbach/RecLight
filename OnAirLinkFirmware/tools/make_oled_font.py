#!/usr/bin/env python3
# Copyright (c) 2026 Hauke Steinbach. All rights reserved.
# Published for inspection only, not as open source: no reuse, no derivative
# works, and no use as machine-learning training data. See LICENSE.

"""
Rasterises the studio site's own Archivo Black into the firmware.

The 72x40 OLED has no font engine, so the headline is baked in as a bitmap at
build time from the very same .woff2 the website serves (assets/fonts/). That
makes the device's REC and the site's headlines the same typeface, not a
lookalike.

Only the headline is done this way. The small status lines keep the compact
5x7 pixel font: Archivo Black is a heavy, wide grotesque, and at the ~8 px
those lines need, a line like "Open browser:" would be far wider than the 72 px
panel -- and the rasteriser would turn its thick strokes into mush at that
size. The website makes the same split, Archivo Black for headlines and a
lighter face for body text.

Usage:  ./make_oled_font.py [path/to/oled_font.h]
Requires: fonttools[woff], pillow.
"""

import io
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[2]
FONT_DIR = (REPO.parent.parent / "03_Websites" / "Website" / "assets" / "fonts")
WOFF2 = FONT_DIR / "archivo-black-400-latin.woff2"
# The site sets its eyebrows and labels in JetBrains Mono, and so does the
# plug-in. At 9 px it advances exactly 5 px per character, which is narrower
# than the pixel font it replaces -- so lines like "Open browser:" finally fit
# the 72 px panel instead of running off the edge.
MONO_WOFF2 = FONT_DIR / "jetbrains-mono-400-latin.woff2"
MONO_SIZE  = 9
CELL_W, CELL_H = 5, 9      # 9 rows so descenders (p, y, g) are not clipped
BASELINE = 7               # caps occupy rows 1..6, descenders 7..8
FIRST_CH, LAST_CH = 0x20, 0x7E

PANEL_W, PANEL_H = 72, 40
WORD = "REC"
# The word sits inside a filled block that runs to the panel edge, so it is
# fitted to a box with a margin rather than to the panel itself.
BOX_W, BOX_H = 62, 26


def load_font_bytes(path=None):
    path = path or WOFF2
    if not path.exists():
        sys.exit(f"ERROR: font not found: {path}\n"
                 "It lives in the website repo; clone it next to this one.")
    # FreeType can't read woff2 directly, so unpack it to plain TTF in memory.
    f = TTFont(str(path))
    buf = io.BytesIO()
    f.flatten = False
    f.save(buf)
    buf.seek(0)
    return buf.read()


def render_word(ttf_bytes, word, box_w, box_h):
    """Render `word` at the largest size that fits the box, in pure black and
    white.

    Rendered directly at its final size onto a bilevel image: the earlier
    version rendered large and then resampled down, and the resampling put
    grey back into a picture that has to end up as single pixels -- exactly
    the blur this is meant to avoid.
    """
    best = None
    for size in range(box_h, box_h * 4):
        font = ImageFont.truetype(io.BytesIO(ttf_bytes), size)
        img = Image.new("1", (box_w * 3, size * 3), 0)
        ImageDraw.Draw(img).text((5, size * 2), word, font=font, fill=1, anchor="ls")
        bbox = img.getbbox()
        if not bbox:
            continue
        w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
        if w > box_w or h > box_h:
            break
        best = img.crop(bbox)
    if best is None:
        sys.exit(f"ERROR: {word!r} does not fit {box_w}x{box_h}")
    return best


def to_columns(img):
    """1-bit column-major bytes, the layout the SSD1306 framebuffer uses."""
    w, h = img.size
    pages = (h + 7) // 8
    data = bytearray(w * pages)
    for x in range(w):
        for y in range(h):
            if img.getpixel((x, y)):
                data[x + (y // 8) * w] |= 1 << (y % 8)
    return w, h, pages, data


def render_small_font(ttf_bytes):
    """One 5x9 cell per printable ASCII character, column-major like the panel."""
    font = ImageFont.truetype(io.BytesIO(ttf_bytes), MONO_SIZE)
    rows = []
    for code in range(FIRST_CH, LAST_CH + 1):
        img = Image.new("1", (CELL_W + 4, CELL_H + 6), 0)
        # Anchored on the BASELINE, not on the ascender top: PIL's default
        # anchor measures from the top of the font's ascent box, which at this
        # size sits several rows above the caps and pushed every glyph off the
        # bottom of the cell.
        ImageDraw.Draw(img).text((0, BASELINE), chr(code), font=font,
                                 fill=1, anchor="ls")
        cols = []
        for x in range(CELL_W):
            bits = 0
            for y in range(CELL_H):
                if img.getpixel((x, y)):
                    bits |= 1 << y
            cols.append(bits)
        rows.append((code, cols))
    return rows


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("../main/oled_font.h")
    ttf = load_font_bytes()

    img = render_word(ttf, WORD, BOX_W, BOX_H)
    w, h, pages, data = to_columns(img)

    small = render_small_font(load_font_bytes(MONO_WOFF2))  # kept for reference
    small_rows = "\n".join(
        "  { " + ", ".join(f"0x{c:04X}" for c in cols) + f" }},  // {chr(code)!r}"
        for code, cols in small)

    rows = []
    for p in range(pages):
        row = ", ".join(f"0x{data[x + p * w]:02X}" for x in range(w))
        rows.append(f"  /* page {p} */ {row},")

    out.write_text(f'''// Copyright (c) 2026 Hauke Steinbach. All rights reserved.\n// Published for inspection only, not as open source: no reuse, no derivative\n// works, and no use as machine-learning training data. See LICENSE.\n//\n// oled_font.h -- GENERATED, do not edit by hand.
//
// Produced by tools/make_oled_font.py from the studio site's own
// assets/fonts/archivo-black-400-latin.woff2, so the word on the device is
// set in the same typeface as the website's headlines rather than an
// approximation of it. Re-run the generator if the brand font ever changes.

#pragma once
#include <stdint.h>

#define OLED_REC_W {w}
#define OLED_REC_H {h}
#define OLED_REC_PAGES {pages}

// Column-major, one bit per pixel, page 0 = rows 0-7 -- the same layout as
// the SSD1306 framebuffer, so blitting is a straight copy.
static const uint8_t OLED_REC_BITMAP[{w * pages}] = {{
{chr(10).join(rows)}
}};

// --- Small text: JetBrains Mono, the site's and the plug-in's label face ---
#define OLED_SMALL_FIRST 0x{FIRST_CH:02X}
#define OLED_SMALL_LAST  0x{LAST_CH:02X}
#define OLED_SMALL_W {CELL_W}
#define OLED_SMALL_H {CELL_H}

// One entry per character, {CELL_W} columns of {CELL_H} bits (bit 0 = top row).
static const uint16_t OLED_SMALL_FONT[][{CELL_W}] = {{
{small_rows}
}};
''')
    print(f"wrote {out}  (REC {w}x{h}px {w * pages} B, "
          f"small font {len(small)} glyphs {len(small) * CELL_W * 2} B)")

    # ASCII proof, so the shapes can be checked without flashing a board.
    for y in range(h):
        print("".join("#" if img.getpixel((x, y)) else "." for x in range(w)))


if __name__ == "__main__":
    main()
