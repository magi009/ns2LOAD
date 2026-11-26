// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// The launcher's software canvas and its text. Nothing here touches the console, so the same
// code draws the UI previews built on a PC (preview/).
//
// From toronja (src/citra_switch/ui_canvas.h), changed for nx2-flow by tools/adapt_ui.py: the
// font can be one made of glyph pictures (font_baked.h), which needs no FreeType; toronja's own
// FreeType font is kept behind UI_FONT_FREETYPE.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef UI_FONT_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#else
#include "font_baked.h"
#endif

namespace SwitchFrontend::Ui {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The panel.
constexpr int kPanelW = 1280;
constexpr int kPanelH = 720;

constexpr u32 MakeColor(u8 r, u8 g, u8 b, u8 a = 0xFF) {
    return (u32{a} << 24) | (u32{b} << 16) | (u32{g} << 8) | u32{r};
}

// The palette. Variables, not constants: there is a dark and a light theme (ApplyTheme), in
// the manner of a console's own settings screens - flat surfaces, one cool accent, and a
// bright ring round whatever has the focus.
inline u32 kColBg = 0;        // Page background.
inline u32 kColRail = 0;      // The drawer.
inline u32 kColSurface = 0;   // Cards and rows.
inline u32 kColSurfaceHi = 0; // The same, raised or focused.
inline u32 kColBadge = 0;     // Chips and other small fills.
inline u32 kColAccent = 0;
inline u32 kColAccentDim = 0;
inline u32 kColText = 0;
inline u32 kColTextDim = 0;
inline u32 kColOnAccent = 0;  // Text on an accent fill.
inline u32 kColError = 0;
inline u32 kColHintBar = 0;
inline u32 kColLine = 0;      // Hairlines between rows and sections.
inline u32 kColFocus = 0;     // The focus ring.
inline u32 kColWarnBg = 0;
inline u32 kColWarnText = 0;
inline u32 kColShelfTop = 0;  // Backdrop of the library, top to horizon...
inline u32 kColShelfHorizon = 0;
inline u32 kColFloorNear = 0; // ...and its floor, horizon to the bottom edge.
inline u32 kColFloorFar = 0;
inline bool g_theme_light = false;

inline void ApplyTheme(bool light) {
    g_theme_light = light;
    if (light) {
        kColBg = MakeColor(0xEB, 0xEB, 0xEB);
        kColRail = MakeColor(0xF4, 0xF4, 0xF4);
        kColSurface = MakeColor(0xFB, 0xFB, 0xFB);
        kColSurfaceHi = MakeColor(0xFF, 0xFF, 0xFF);
        kColBadge = MakeColor(0xD6, 0xD6, 0xD6);
        kColAccent = MakeColor(0x2F, 0x4F, 0xEE);
        kColAccentDim = MakeColor(0xC3, 0xCC, 0xF8);
        kColText = MakeColor(0x2D, 0x2D, 0x2D);
        kColTextDim = MakeColor(0x6C, 0x6C, 0x6C);
        kColOnAccent = MakeColor(0xFF, 0xFF, 0xFF);
        kColError = MakeColor(0xD6, 0x30, 0x30);
        kColHintBar = MakeColor(0xEB, 0xEB, 0xEB);
        kColLine = MakeColor(0xCD, 0xCD, 0xCD);
        kColFocus = MakeColor(0x00, 0xBE, 0xDC);
        kColWarnBg = MakeColor(0xFF, 0xEE, 0xC2);
        kColWarnText = MakeColor(0x7A, 0x52, 0x00);
        kColShelfTop = MakeColor(0xFC, 0xFC, 0xFC);
        kColShelfHorizon = MakeColor(0xDE, 0xDE, 0xDE);
        kColFloorNear = MakeColor(0xCF, 0xCF, 0xCF);
        kColFloorFar = MakeColor(0xF2, 0xF2, 0xF2);
    } else {
        kColBg = MakeColor(0x2D, 0x2D, 0x2D);
        kColRail = MakeColor(0x32, 0x32, 0x32);
        kColSurface = MakeColor(0x39, 0x39, 0x39);
        kColSurfaceHi = MakeColor(0x46, 0x46, 0x46);
        kColBadge = MakeColor(0x58, 0x58, 0x58);
        kColAccent = MakeColor(0x00, 0xE0, 0xC0);
        kColAccentDim = MakeColor(0x0C, 0x5E, 0x54);
        kColText = MakeColor(0xFF, 0xFF, 0xFF);
        kColTextDim = MakeColor(0xA8, 0xA8, 0xA8);
        kColOnAccent = MakeColor(0x1A, 0x1A, 0x1A);
        kColError = MakeColor(0xFF, 0x5C, 0x5C);
        kColHintBar = MakeColor(0x2D, 0x2D, 0x2D);
        kColLine = MakeColor(0x4E, 0x4E, 0x4E);
        kColFocus = MakeColor(0x5C, 0xE6, 0xFF);
        kColWarnBg = MakeColor(0x4A, 0x3C, 0x14);
        kColWarnText = MakeColor(0xFF, 0xD7, 0x6E);
        kColShelfTop = MakeColor(0x16, 0x16, 0x16);
        kColShelfHorizon = MakeColor(0x3C, 0x3C, 0x3C);
        kColFloorNear = MakeColor(0x30, 0x30, 0x30);
        kColFloorFar = MakeColor(0x0E, 0x0E, 0x0E);
    }
}

// The dark theme until something says otherwise.
inline const bool g_theme_initialised = (ApplyTheme(false), true);

class Canvas {
public:
    Canvas() : pixels(static_cast<std::size_t>(kPanelW) * kPanelH) {}

    u32* Data() {
        return pixels.data();
    }

    int Width() const {
        return width;
    }

    int Height() const {
        return height;
    }

    void Resize(int w, int h) {
        width = w;
        height = h;
        pixels.assign(static_cast<std::size_t>(w) * h, 0);
    }

    void Clear(u32 color) {
        std::fill(pixels.begin(), pixels.end(), color);
    }

    void Blend(int x, int y, u32 color, u8 coverage) {
        if (x < 0 || y < 0 || x >= width || y >= height || coverage == 0) {
            return;
        }
        const u32 a = ((color >> 24) & 0xFF) * coverage / 255;
        u32& dst = pixels[static_cast<std::size_t>(y) * width + x];
        if (a == 0) {
            return;
        }
        if (a >= 0xFF) {
            dst = (dst & 0xFF000000u) | (color & 0x00FFFFFFu);
            return;
        }
        const u32 inv = 255 - a;
        const u32 sr = color & 0xFF, sg = (color >> 8) & 0xFF, sb = (color >> 16) & 0xFF;
        const u32 dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF;
        const u32 rr = (sr * a + dr * inv) / 255;
        const u32 rg = (sg * a + dg * inv) / 255;
        const u32 rb = (sb * a + db * inv) / 255;
        dst = MakeColor(static_cast<u8>(rr), static_cast<u8>(rg), static_cast<u8>(rb));
    }

    void FillRect(int x, int y, int w, int h, u32 color) {
        const int x0 = std::max(0, x), y0 = std::max(0, y);
        const int x1 = std::min(width, x + w), y1 = std::min(height, y + h);
        const u8 alpha = (color >> 24) & 0xFF;
        for (int yy = y0; yy < y1; ++yy) {
            if (alpha >= 0xFF) {
                std::fill_n(pixels.data() + static_cast<std::size_t>(yy) * width + x0, x1 - x0,
                            color);
            } else {
                for (int xx = x0; xx < x1; ++xx) {
                    Blend(xx, yy, color, alpha);
                }
            }
        }
    }

    void FillRoundRect(int x, int y, int w, int h, int r, u32 color) {
        r = std::clamp(r, 0, std::min(w, h) / 2);
        for (int row = 0; row < h; ++row) {
            int cut = 0;
            if (row < r) {
                const int t = r - 1 - row;
                cut = r - static_cast<int>(std::sqrt(static_cast<float>(r * r - t * t)));
            } else if (row >= h - r) {
                const int t = row - (h - r);
                cut = r - static_cast<int>(std::sqrt(static_cast<float>(r * r - t * t)));
            }
            FillRect(x + cut, y + row, w - 2 * cut, 1, color);
        }
    }

    void RoundBorder(int x, int y, int w, int h, int r, int thickness, u32 border, u32 inner) {
        FillRoundRect(x, y, w, h, r, border);
        FillRoundRect(x + thickness, y + thickness, w - 2 * thickness, h - 2 * thickness,
                      std::max(0, r - thickness), inner);
    }

    void BlitIcon(const std::vector<u32>& icon, int src_size, int dx, int dy, int dst_size) {
        if (icon.empty() || src_size <= 0) {
            return;
        }
        for (int oy = 0; oy < dst_size; ++oy) {
            const float sy = (oy + 0.5f) * src_size / dst_size - 0.5f;
            const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src_size - 1);
            const int y1 = std::min(y0 + 1, src_size - 1);
            const float fy = std::clamp(sy - y0, 0.0f, 1.0f);
            for (int ox = 0; ox < dst_size; ++ox) {
                const float sx = (ox + 0.5f) * src_size / dst_size - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src_size - 1);
                const int x1 = std::min(x0 + 1, src_size - 1);
                const float fx = std::clamp(sx - x0, 0.0f, 1.0f);
                const u32 c00 = icon[y0 * src_size + x0], c10 = icon[y0 * src_size + x1];
                const u32 c01 = icon[y1 * src_size + x0], c11 = icon[y1 * src_size + x1];
                float ch[3];
                for (int i = 0; i < 3; ++i) {
                    const int s = i * 8;
                    const float top = ((c00 >> s) & 0xFF) * (1 - fx) + ((c10 >> s) & 0xFF) * fx;
                    const float bot = ((c01 >> s) & 0xFF) * (1 - fx) + ((c11 >> s) & 0xFF) * fx;
                    ch[i] = top * (1 - fy) + bot * fy;
                }
                const int px = dx + ox, py = dy + oy;
                if (px >= 0 && py >= 0 && px < width && py < height) {
                    pixels[static_cast<std::size_t>(py) * width + px] = MakeColor(
                        static_cast<u8>(ch[0]), static_cast<u8>(ch[1]), static_cast<u8>(ch[2]));
                }
            }
        }
    }

private:
    std::vector<u32> pixels;
    int width = kPanelW;
    int height = kPanelH;
};

#ifdef UI_FONT_FREETYPE
class Font {
public:
    /// Starts FreeType. Faces are added afterwards; the first one that has a glyph wins.
    bool InitLibrary() {
        if (initialised) {
            return library != nullptr;
        }
        initialised = true;
        return FT_Init_FreeType(&library) == 0;
    }

    /// `data` must stay valid for as long as the font is used.
    bool AddMemoryFace(const void* data, std::size_t size) {
        FT_Face face{};
        if (library == nullptr ||
            FT_New_Memory_Face(library, static_cast<const FT_Byte*>(data),
                               static_cast<FT_Long>(size), 0, &face) != 0) {
            return false;
        }
        faces.push_back(face);
        return true;
    }

    bool AddFileFace(const char* path) {
        FT_Face face{};
        if (library == nullptr || FT_New_Face(library, path, 0, &face) != 0) {
            return false;
        }
        faces.push_back(face);
        return true;
    }

    bool Valid() const {
        return !faces.empty();
    }

    void Shutdown() {
        if (!initialised) {
            return;
        }
        cache.clear();
        for (FT_Face face : faces) {
            FT_Done_Face(face);
        }
        faces.clear();
        if (library) {
            FT_Done_FreeType(library);
            library = nullptr;
        }
        initialised = false;
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

    const Glyph* GetGlyph(u32 cp, int size) {
        const u64 key = (static_cast<u64>(size) << 32) | cp;
        if (auto it = cache.find(key); it != cache.end()) {
            return &it->second;
        }
        FT_Face face = faces.empty() ? nullptr : faces.front();
        for (FT_Face candidate : faces) {
            if (FT_Get_Char_Index(candidate, cp) != 0) {
                face = candidate;
                break;
            }
        }
        if (!face) {
            return nullptr;
        }
        FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size));
        // NO_AUTOHINT keeps rendering on the font's native TrueType hinter.
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_NO_AUTOHINT) != 0) {
            return nullptr;
        }
        const FT_GlyphSlot slot = face->glyph;
        Glyph g;
        g.w = static_cast<int>(slot->bitmap.width);
        g.h = static_cast<int>(slot->bitmap.rows);
        g.left = slot->bitmap_left;
        g.top = slot->bitmap_top;
        g.advance = static_cast<int>(slot->advance.x >> 6);
        g.coverage.resize(static_cast<std::size_t>(g.w) * g.h);
        for (int row = 0; row < g.h; ++row) {
            std::memcpy(g.coverage.data() + row * g.w,
                        slot->bitmap.buffer + row * slot->bitmap.pitch, g.w);
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

    bool initialised{};
    FT_Library library{};
    std::vector<FT_Face> faces;
    std::unordered_map<u64, Glyph> cache;
};

#else  // !UI_FONT_FREETYPE
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

inline int CenterBaseline(int y, int h, int size) {
    return y + (h + static_cast<int>(size * 0.7f)) / 2;
}

} // namespace SwitchFrontend::Ui
