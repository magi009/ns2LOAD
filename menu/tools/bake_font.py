#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bakes the menu's font into source/font_baked.h.

The menu draws its text from glyph pictures made here, so that it builds with nothing but the
console's own library: no FreeType on the console, none on the PC that renders the previews.
The face is Aileron Regular (Sora Sagano, CC0 1.0, public domain), the one Pillow carries as its
default; each glyph is rendered once at BASE pixels and scaled to the size asked for when drawn.

    python tools/bake_font.py            (needs Pillow 10.1 or newer)
"""
import os

from PIL import Image, ImageDraw, ImageFont

BASE = 48
RANGES = [(0x20, 0x7E), (0xA1, 0xFF)]
EXTRA = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2122, 0x20AC, 0x0152, 0x0153, 0x0160, 0x0161, 0x0178, 0x017D, 0x017E]


def main():
    font = ImageFont.load_default(BASE)
    missing = font.getmask(chr(0xFFFF)).getbbox(), bytes(font.getmask(chr(0xFFFF)))
    glyphs, blob = [], bytearray()
    for cp in [c for a, b in RANGES for c in range(a, b + 1)] + EXTRA:
        ch = chr(cp)
        advance = round(font.getlength(ch))
        box = font.getbbox(ch, anchor='ls')  # (left, top, right, bottom) from the pen on the baseline
        mask = font.getmask(ch)
        if cp != 0x20 and (mask.getbbox(), bytes(mask)) == missing:
            continue  # (the face has no such glyph: it would be the empty box)
        w, h = mask.size
        if cp == 0x20 or not w or not h:
            glyphs.append((cp, 0, 0, 0, 0, advance, len(blob)))
            continue
        glyphs.append((cp, w, h, box[0], -box[1], advance, len(blob)))
        blob += bytes(mask)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'source', 'font_baked.h')
    with open(out, 'w', newline='\n') as f:
        f.write('// SPDX-License-Identifier: CC0-1.0\n')
        f.write('// Made by tools/bake_font.py: not to be edited. The glyphs of Aileron Regular (Sora Sagano, CC0 1.0)\n')
        f.write(f'// at {BASE} pixels: for each, its size, where it sits from the pen, how far the pen moves, and its coverage.\n')
        f.write('#pragma once\n#include <cstdint>\nnamespace BakedFont {\n')
        f.write(f'constexpr int kBase = {BASE};\n')
        f.write('struct Glyph { std::uint32_t cp; std::uint8_t w, h; std::int8_t left, top; std::uint8_t advance; std::uint32_t at; };\n')
        f.write(f'constexpr int kCount = {len(glyphs)};\n')
        f.write('inline constexpr Glyph kGlyphs[kCount] = {\n')
        for g in glyphs:
            f.write('    {%d, %d, %d, %d, %d, %d, %d},\n' % g)
        f.write('};\n')
        f.write(f'inline constexpr std::uint8_t kCoverage[{len(blob)}] = {{\n')
        for k in range(0, len(blob), 32):
            f.write('    ' + ','.join(str(b) for b in blob[k:k + 32]) + ',\n')
        f.write('};\n}  // namespace BakedFont\n')
    print(f'{len(glyphs)} glyphs, {len(blob)} bytes of coverage -> {os.path.normpath(out)}')


if __name__ == '__main__':
    main()
