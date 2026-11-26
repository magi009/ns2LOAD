#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""How source/ui_canvas.h and source/ui_views.h were made from toronja's (src/citra_switch/): run once, on fresh copies of the
two files. Kept so that the changes are on record and can be made again over a newer copy.

    python tools/adapt_ui.py
"""
import os

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'source')


def patch(name, pairs):
    p = os.path.join(HERE, name)
    s = open(p, encoding='utf-8').read()
    for a, b in pairs:
        assert s.count(a) == 1, (name, a[:70], s.count(a))
        s = s.replace(a, b)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)


BAKED = r'''#ifdef UI_FONT_FREETYPE
class Font {'''

BAKED_END = r'''#else  // !UI_FONT_FREETYPE
// The same font, from glyph pictures made beforehand (tools/bake_font.py, font_baked.h): no library is needed for it, on the
// console or on a PC. Each glyph was rendered at one size and is scaled here to the size asked for, by the area each new pixel
// covers of the old ones. The face has the Latin letters without accents: an accented letter is drawn as its plain one, and what
// has no plain one (another script) as nothing. Built with UI_FONT_FREETYPE, the class above is used instead, with any face.
class Font {
public:
    bool InitLibrary() {
        return true;
    }

    bool AddMemoryFace(const void*, std::size_t) {
        return true;
    }

    bool AddFileFace(const char*) {
        return true;
    }

    bool Valid() const {
        return true;
    }

    void Shutdown() {
        cache.clear();
    }

    int Draw(Canvas& canvas, int x, int baseline, std::string_view text, int size, u32 color) {
        int pen = x;
        std::size_t i = 0;
        while (i < text.size()) {
            const u32 cp = DecodeUtf8(text, i);
            const Glyph* g = GetGlyph(cp, size);
            if (!g) {
                continue;
            }
            const int gx = pen + g->left;
            const int gy = baseline - g->top;
            for (int row = 0; row < g->h; ++row) {
                for (int col = 0; col < g->w; ++col) {
                    canvas.Blend(gx + col, gy + row, color, g->coverage[row * g->w + col]);
                }
            }
            pen += g->advance;
        }
        return pen - x;
    }

    int Measure(std::string_view text, int size) {
        int w = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            const u32 cp = DecodeUtf8(text, i);
            if (const Glyph* g = GetGlyph(cp, size)) {
                w += g->advance;
            }
        }
        return w;
    }

    std::string Truncate(std::string_view text, int size, int maxw) {
        if (Measure(text, size) <= maxw) {
            return std::string{text};
        }
        const int ell = Measure("…", size);
        std::string out;
        int w = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            const std::size_t start = i;
            const u32 cp = DecodeUtf8(text, i);
            const Glyph* g = GetGlyph(cp, size);
            const int adv = g ? g->advance : 0;
            if (w + adv + ell > maxw) {
                break;
            }
            out.append(text.substr(start, i - start));
            w += adv;
        }
        out.append("…");
        return out;
    }

    std::string TruncateFront(std::string_view text, int size, int maxw) {
        if (Measure(text, size) <= maxw) {
            return std::string{text};
        }
        const int ell = Measure("…", size);
        std::size_t i = 0;
        while (i < text.size()) {
            DecodeUtf8(text, i);
            const std::string_view tail = text.substr(i);
            if (ell + Measure(tail, size) <= maxw) {
                return "…" + std::string{tail};
            }
        }
        return "…";
    }

private:
    struct Glyph {
        int w{}, h{}, left{}, top{}, advance{};
        std::vector<u8> coverage;
    };

    /// The letter without its accent, for the letters the face lacks; 0 when there is none.
    static u32 Plain(u32 cp) {
        static const char* const latin1 = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        if (cp >= 0xC0 && cp <= 0xFF) {
            return static_cast<u8>(latin1[cp - 0xC0]);
        }
        if (cp >= 0x100 && cp <= 0x17F) {
            static const char* const ext =
                "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
            return static_cast<u8>(ext[cp - 0x100]);
        }
        switch (cp) {
        case 0xA0:
            return ' ';
        case 0x2010: case 0x2011: case 0x2012: case 0x2212:
            return '-';
        case 0x2032:
            return '\'';
        case 0x2033:
            return '"';
        case 0x3000:
            return ' ';
        default:
            return 0;
        }
    }

    static const BakedFont::Glyph* Baked(u32 cp) {
        int lo = 0, hi = BakedFont::kCount - 1;
        // (sorted by code point up to the last of the ranges; the few after it are looked at one by one)
        for (int k = 0; k < BakedFont::kCount; ++k) {
            if (BakedFont::kGlyphs[k].cp == cp) {
                return &BakedFont::kGlyphs[k];
            }
        }
        (void)lo, (void)hi;
        return nullptr;
    }

    const Glyph* GetGlyph(u32 cp, int size) {
        const u64 key = (static_cast<u64>(size) << 32) | cp;
        if (auto it = cache.find(key); it != cache.end()) {
            return it->second.advance < 0 ? nullptr : &it->second;
        }
        const BakedFont::Glyph* b = Baked(cp);
        if (!b) {
            if (const u32 plain = Plain(cp)) {
                b = Baked(plain);
            }
        }
        if (!b) {
            Glyph none;
            none.advance = -1;
            cache.emplace(key, std::move(none));
            return nullptr;
        }
        const float s = static_cast<float>(size) / BakedFont::kBase;
        Glyph g;
        g.advance = static_cast<int>(std::lround(b->advance * s));
        if (b->w && b->h) {
            const float x0 = b->left * s, y0 = -b->top * s; // The glyph's box, from the pen, scaled.
            const int left = static_cast<int>(std::floor(x0)), top = static_cast<int>(std::floor(y0));
            g.left = left;
            g.top = -top;
            g.w = static_cast<int>(std::ceil(x0 + b->w * s)) - left;
            g.h = static_cast<int>(std::ceil(y0 + b->h * s)) - top;
            g.coverage.assign(static_cast<std::size_t>(g.w) * g.h, 0);
            const u8* src = BakedFont::kCoverage + b->at;
            const float inv = 1.0f / s;
            for (int row = 0; row < g.h; ++row) {
                // The stretch of the old picture this new pixel covers, in old pixels.
                const float sy0 = (top + row - y0) * inv, sy1 = (top + row + 1 - y0) * inv;
                for (int col = 0; col < g.w; ++col) {
                    const float sx0 = (left + col - x0) * inv, sx1 = (left + col + 1 - x0) * inv;
                    float sum = 0.0f;
                    for (int yy = std::max(0, static_cast<int>(std::floor(sy0)));
                         yy < b->h && yy < sy1; ++yy) {
                        const float wy = std::min(sy1, yy + 1.0f) - std::max(sy0, static_cast<float>(yy));
                        if (wy <= 0.0f) {
                            continue;
                        }
                        for (int xx = std::max(0, static_cast<int>(std::floor(sx0)));
                             xx < b->w && xx < sx1; ++xx) {
                            const float wx =
                                std::min(sx1, xx + 1.0f) - std::max(sx0, static_cast<float>(xx));
                            if (wx > 0.0f) {
                                sum += wx * wy * src[yy * b->w + xx];
                            }
                        }
                    }
                    g.coverage[static_cast<std::size_t>(row) * g.w + col] =
                        static_cast<u8>(std::clamp(sum * s * s + 0.5f, 0.0f, 255.0f));
                }
            }
        }
        return &cache.emplace(key, std::move(g)).first->second;
    }

    static u32 DecodeUtf8(std::string_view s, std::size_t& i) {
        const u8 c = static_cast<u8>(s[i++]);
        if (c < 0x80) {
            return c;
        }
        int extra = 0;
        u32 cp = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return '?';
        }
        for (int k = 0; k < extra && i < s.size(); ++k) {
            cp = (cp << 6) | (static_cast<u8>(s[i++]) & 0x3F);
        }
        return cp;
    }

    std::unordered_map<u64, Glyph> cache;
};
#endif  // UI_FONT_FREETYPE

inline int CenterBaseline(int y, int h, int size) {'''

patch('ui_canvas.h', [
    ('''// The launcher's software canvas and its FreeType text. Nothing here touches the console, so
// the same code draws the UI previews built on a PC (tools/ui-preview).
''', '''// The launcher's software canvas and its text. Nothing here touches the console, so the same
// code draws the UI previews built on a PC (preview/).
//
// From toronja (src/citra_switch/ui_canvas.h), changed for nx2-flow by tools/adapt_ui.py: the
// font can be one made of glyph pictures (font_baked.h), which needs no FreeType; toronja's own
// FreeType font is kept behind UI_FONT_FREETYPE.
'''),
    ('''#include <ft2build.h>
#include FT_FREETYPE_H
''', '''#ifdef UI_FONT_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#else
#include "font_baked.h"
#endif
'''),
    ('''class Font {''', BAKED),
    ('''inline int CenterBaseline(int y, int h, int size) {''', BAKED_END),
])

patch('ui_views.h', [
    ('#include "citra_switch/ui_canvas.h"', '#include "ui_canvas.h"'),
    ("""#pragma once
""", """// From toronja (src/citra_switch/ui_views.h), changed for nx2-flow by tools/adapt_ui.py: the
// case on the shelf has the shape of this console's game cases, and toronja's own home screen
// is left out (the menu's is in app.cpp).

#pragma once
"""),
    ("""/// A 3DS case is a little wider than tall.
constexpr float kCaseAspect = 1.13f;""", """/// A game case of this console is taller than wide: 104 mm by 169.
constexpr float kCaseAspect = 0.615f;"""),
    ("""// Measured on GameTDB's scans (1616 x 680): the spine runs from 47.9% to 52.1%. A wider
// guess put a sliver of the front and of the back on the spine.
constexpr float kWrapFace = 0.479f;""", """// By the case's own measures: two faces of 104 mm and a spine of 11 between them.
constexpr float kWrapFace = 0.475f;"""),
    ("""    default: // Coverflow: side covers turn away and close ranks.
        p.cx = cx + sign * case_w * (0.70f * t + 0.30f * beyond);""", """    default: // Coverflow: side covers turn away and close ranks.
        // (farther apart than toronja's, in case widths: these cases are half as wide for their height)
        p.cx = cx + sign * case_w * (1.02f * t + 0.50f * beyond);"""),
])

# toronja's home screen (its bar of buttons, its link button) is not this menu's
p = os.path.join(HERE, 'ui_views.h')
s = open(p, encoding='utf-8').read()
a = s.index('// ' + '-' * 93 + '\n// The home screen')
s = s[:a] + '} // namespace SwitchFrontend::Ui\n'
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')

