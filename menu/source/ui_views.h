// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The launcher's larger views: the library (a shelf of covers standing on a reflective floor),
// the bar under it, the navigation drawer of the options screen, and the small sheets shared
// with it. Colours come from the theme in ui_canvas.h.
//
// Everything here draws on a Ui::Canvas from plain data and knows nothing about the console,
// which is what lets tools/ui-preview render these views on a PC.

// From toronja (src/citra_switch/ui_views.h), changed for nx2-flow by tools/adapt_ui.py: the
// case on the shelf has the shape of this console's game cases, and toronja's own home screen
// is left out (the menu's is in app.cpp).

#pragma once

#include <functional>
#include <span>

#include "ui_canvas.h"

namespace SwitchFrontend::Ui {

// ---------------------------------------------------------------------------------------------
// Pixels

/// An RGBA picture in the canvas's own pixel layout (MakeColor).
struct Image {
    int w{};
    int h{};
    std::vector<u32> px;

    bool Empty() const {
        return w <= 0 || h <= 0 || px.empty();
    }
};

inline u32 MixColor(u32 a, u32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto ch = [&](int shift) {
        const float x = static_cast<float>((a >> shift) & 0xFF);
        const float y = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<u32>(x + (y - x) * t + 0.5f) & 0xFF;
    };
    return (0xFFu << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

inline u32 WithAlpha(u32 color, u8 alpha) {
    return (color & 0x00FFFFFFu) | (static_cast<u32>(alpha) << 24);
}

inline void FillGradientV(Canvas& c, int x, int y, int w, int h, u32 top, u32 bottom) {
    for (int row = 0; row < h; ++row) {
        c.FillRect(x, y + row, w, 1, MixColor(top, bottom, h <= 1 ? 0.0f : row / float(h - 1)));
    }
}

/// A disc with a soft edge. `r_inner` > 0 makes it a ring.
inline void FillDisc(Canvas& c, float cx, float cy, float r, u32 color, float r_inner = 0.0f) {
    const int x0 = static_cast<int>(std::floor(cx - r - 1)), x1 = static_cast<int>(cx + r + 2);
    const int y0 = static_cast<int>(std::floor(cy - r - 1)), y1 = static_cast<int>(cy + r + 2);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) +
                                      (y + 0.5f - cy) * (y + 0.5f - cy));
            float cover = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
            if (r_inner > 0.0f) {
                cover *= std::clamp(d - r_inner + 0.5f, 0.0f, 1.0f);
            }
            if (cover > 0.0f) {
                c.Blend(x, y, color, static_cast<u8>(cover * 255.0f));
            }
        }
    }
}

/// A straight stroke with round ends.
inline void StrokeLine(Canvas& c, float ax, float ay, float bx, float by, float width, u32 color) {
    const float half = width / 2;
    const int x0 = static_cast<int>(std::floor(std::min(ax, bx) - half - 1));
    const int x1 = static_cast<int>(std::max(ax, bx) + half + 2);
    const int y0 = static_cast<int>(std::floor(std::min(ay, by) - half - 1));
    const int y1 = static_cast<int>(std::max(ay, by) + half + 2);
    const float dx = bx - ax, dy = by - ay;
    const float len2 = std::max(dx * dx + dy * dy, 0.0001f);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float px = x + 0.5f - ax, py = y + 0.5f - ay;
            const float t = std::clamp((px * dx + py * dy) / len2, 0.0f, 1.0f);
            const float ex = px - dx * t, ey = py - dy * t;
            const float cover = std::clamp(half - std::sqrt(ex * ex + ey * ey) + 0.5f, 0.0f, 1.0f);
            if (cover > 0.0f) {
                c.Blend(x, y, color, static_cast<u8>(cover * 255.0f));
            }
        }
    }
}

/// The ring round whatever has the focus. It breathes: `time` is in seconds.
inline void DrawFocusRing(Canvas& c, int x, int y, int w, int h, int radius, float time,
                          u32 inner) {
    const float pulse = 0.5f + 0.5f * std::sin(time * 4.2f);
    const u32 ring = MixColor(kColFocus, kColAccent, pulse * 0.55f);
    c.RoundBorder(x - 3, y - 3, w + 6, h + 6, radius + 3, 3, ring, inner);
}

/// Bilinear sample of `img` at (u, v) in pixels.
inline u32 SampleImage(const Image& img, float u, float v) {
    u = std::clamp(u, 0.0f, static_cast<float>(img.w - 1));
    v = std::clamp(v, 0.0f, static_cast<float>(img.h - 1));
    const int x0 = static_cast<int>(u), y0 = static_cast<int>(v);
    const int x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
    const float fx = u - x0, fy = v - y0;
    const u32 c00 = img.px[y0 * img.w + x0], c10 = img.px[y0 * img.w + x1];
    const u32 c01 = img.px[y1 * img.w + x0], c11 = img.px[y1 * img.w + x1];
    u32 out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        const float top = ((c00 >> shift) & 0xFF) * (1 - fx) + ((c10 >> shift) & 0xFF) * fx;
        const float bot = ((c01 >> shift) & 0xFF) * (1 - fx) + ((c11 >> shift) & 0xFF) * fx;
        out |= (static_cast<u32>(top * (1 - fy) + bot * fy + 0.5f) & 0xFF) << shift;
    }
    return out;
}

/// Where a picture lands on screen: a quad whose left and right edges are vertical, which is
/// all a shelf of covers turned about a vertical axis needs.
struct Quad {
    float x0{}, x1{};   // Left and right edge.
    float top0{}, h0{}; // Top and height at the left edge.
    float top1{}, h1{}; // Top and height at the right edge.
};

/// Library options: the reflections in the floor, and the hovering of the games.
inline bool g_shelf_reflections = true;
inline bool g_shelf_floating = true;
/// Covers stand as cases, with a side to them.
inline bool g_shelf_cases = true;

/// Draws the part of `img` between `u0` and `u1` (0..1 across the picture) into `q`.
/// `shade` moves the picture towards `shade_to` (0 = as is, 1 = that colour). With `mirror`
/// the picture is drawn upside down under the quad and fades out downwards, as a reflection
/// in the floor; `mirror_gap` is how far above the floor the picture hangs. `smooth` filters
/// the picture; without it the nearest pixel is taken, which is several times cheaper and is
/// what the covers at the sides and the reflections get - a shelf redraws every frame, in
/// software.
inline void BlitQuad(Canvas& c, const Image& img, const Quad& q, float u0, float u1, float shade,
                     u32 shade_to, bool mirror, bool smooth, float mirror_gap = 0.0f) {
    if (img.Empty() || q.x1 <= q.x0 || (mirror && !g_shelf_reflections)) {
        return;
    }
    const int xa = std::max(0, static_cast<int>(std::floor(q.x0)));
    const int xb = std::min(c.Width(), static_cast<int>(std::ceil(q.x1)));
    const u32 keep = static_cast<u32>((1.0f - std::clamp(shade, 0.0f, 1.0f)) * 256.0f);
    const u32 lose = 256 - keep;
    const u32 tr = shade_to & 0xFF, tg = (shade_to >> 8) & 0xFF, tb = (shade_to >> 16) & 0xFF;
    u32* const pixels = c.Data();
    const int stride = c.Width();
    for (int x = xa; x < xb; ++x) {
        const float t = std::clamp((x + 0.5f - q.x0) / (q.x1 - q.x0), 0.0f, 1.0f);
        const float top = q.top0 + (q.top1 - q.top0) * t;
        const float h = q.h0 + (q.h1 - q.h0) * t;
        if (h < 1.0f) {
            continue;
        }
        const float u = (u0 + (u1 - u0) * t) * (img.w - 1);
        const int ui = std::clamp(static_cast<int>(u + 0.5f), 0, img.w - 1);
        // The reflection hangs as far below the floor as the picture does above it.
        const float floor = top + h + mirror_gap;
        const float y_begin = mirror ? floor + mirror_gap : top;
        const float y_end = mirror ? y_begin + h * 0.42f : top + h;
        const int ya = std::max(0, static_cast<int>(std::floor(y_begin)));
        const int yb = std::min(c.Height(), static_cast<int>(std::ceil(y_end)));
        const float v_step = (img.h - 1) / h;
        for (int y = ya; y < yb; ++y) {
            float v;
            u32 fade = 256;
            if (mirror) {
                v = (img.h - 1) - (y + 0.5f - y_begin) * v_step; // Bottom row first.
                const float f = 1.0f - (y + 0.5f - y_begin) / (y_end - y_begin);
                fade = static_cast<u32>(64.0f * std::clamp(f * f, 0.0f, 1.0f));
            } else {
                v = (y + 0.5f - top) * v_step;
            }
            if (v < 0.0f || v > static_cast<float>(img.h - 1)) {
                continue;
            }
            const u32 color =
                smooth ? SampleImage(img, u, v)
                       : img.px[static_cast<std::size_t>(static_cast<int>(v + 0.5f)) * img.w + ui];
            // A see-through pixel (round icons have them) leaves what is behind it alone.
            const u32 alpha = color >> 24;
            if (alpha < 32) {
                continue;
            }
            u32 r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
            if (keep < 256) {
                r = (r * keep + tr * lose) >> 8;
                g = (g * keep + tg * lose) >> 8;
                b = (b * keep + tb * lose) >> 8;
            }
            u32& dst = pixels[static_cast<std::size_t>(y) * stride + x];
            if (fade < 256) {
                const u32 inv = 256 - fade;
                r = (r * fade + (dst & 0xFF) * inv) >> 8;
                g = (g * fade + ((dst >> 8) & 0xFF) * inv) >> 8;
                b = (b * fade + ((dst >> 16) & 0xFF) * inv) >> 8;
            }
            dst = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Icons, drawn from primitives so they scale and need no assets.

enum class Icon { Gear, Power, Search, Flip, Shelf, Info, Refresh, Back, Warning, Check, Link };

inline void DrawIcon(Canvas& c, Icon icon, float cx, float cy, float size, u32 color) {
    const float r = size / 2;
    const float w = std::max(2.0f, size / 9);
    switch (icon) {
    case Icon::Gear:
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.7853982f;
            StrokeLine(c, cx + std::cos(a) * r * 0.62f, cy + std::sin(a) * r * 0.62f,
                       cx + std::cos(a) * r * 0.98f, cy + std::sin(a) * r * 0.98f, w * 1.5f, color);
        }
        FillDisc(c, cx, cy, r * 0.72f, color, r * 0.30f);
        break;
    case Icon::Power:
        for (int i = 0; i <= 26; ++i) {
            const float a = -1.5707963f + 0.75f + i * (6.2831853f - 1.5f) / 26;
            FillDisc(c, cx + std::cos(a) * r * 0.78f, cy + std::sin(a) * r * 0.78f + r * 0.08f,
                     w / 2, color);
        }
        StrokeLine(c, cx, cy - r * 0.95f, cx, cy - r * 0.05f, w, color);
        break;
    case Icon::Search:
        FillDisc(c, cx - r * 0.18f, cy - r * 0.18f, r * 0.62f, color, r * 0.62f - w);
        StrokeLine(c, cx + r * 0.30f, cy + r * 0.30f, cx + r * 0.88f, cy + r * 0.88f, w * 1.2f,
                   color);
        break;
    case Icon::Flip:
        StrokeLine(c, cx - r * 0.75f, cy - r * 0.25f, cx + r * 0.55f, cy - r * 0.25f, w, color);
        StrokeLine(c, cx + r * 0.55f, cy - r * 0.25f, cx + r * 0.20f, cy - r * 0.62f, w, color);
        StrokeLine(c, cx + r * 0.75f, cy + r * 0.25f, cx - r * 0.55f, cy + r * 0.25f, w, color);
        StrokeLine(c, cx - r * 0.55f, cy + r * 0.25f, cx - r * 0.20f, cy + r * 0.62f, w, color);
        break;
    case Icon::Shelf:
        for (int i = -1; i <= 1; ++i) {
            const float h = i == 0 ? r * 1.5f : r * 1.05f;
            const float bw = i == 0 ? r * 0.62f : r * 0.40f;
            c.FillRoundRect(static_cast<int>(cx + i * r * 0.64f - bw / 2),
                            static_cast<int>(cy - h / 2), static_cast<int>(bw),
                            static_cast<int>(h), 2, color);
        }
        break;
    case Icon::Info:
        FillDisc(c, cx, cy, r * 0.95f, color, r * 0.95f - w);
        FillDisc(c, cx, cy - r * 0.42f, w * 0.75f, color);
        StrokeLine(c, cx, cy - r * 0.10f, cx, cy + r * 0.48f, w, color);
        break;
    case Icon::Refresh:
        for (int i = 0; i <= 24; ++i) {
            const float a = -0.6f + i * 4.9f / 24;
            FillDisc(c, cx + std::cos(a) * r * 0.75f, cy + std::sin(a) * r * 0.75f, w / 2, color);
        }
        StrokeLine(c, cx + r * 0.62f, cy - r * 0.42f, cx + r * 0.95f, cy - r * 0.80f, w, color);
        StrokeLine(c, cx + r * 0.62f, cy - r * 0.42f, cx + r * 0.20f, cy - r * 0.55f, w, color);
        break;
    case Icon::Back:
        StrokeLine(c, cx + r * 0.75f, cy, cx - r * 0.70f, cy, w, color);
        StrokeLine(c, cx - r * 0.70f, cy, cx - r * 0.15f, cy - r * 0.55f, w, color);
        StrokeLine(c, cx - r * 0.70f, cy, cx - r * 0.15f, cy + r * 0.55f, w, color);
        break;
    case Icon::Warning:
        StrokeLine(c, cx, cy - r * 0.85f, cx - r * 0.92f, cy + r * 0.72f, w, color);
        StrokeLine(c, cx, cy - r * 0.85f, cx + r * 0.92f, cy + r * 0.72f, w, color);
        StrokeLine(c, cx - r * 0.92f, cy + r * 0.72f, cx + r * 0.92f, cy + r * 0.72f, w, color);
        StrokeLine(c, cx, cy - r * 0.30f, cx, cy + r * 0.22f, w, color);
        FillDisc(c, cx, cy + r * 0.48f, w * 0.7f, color);
        break;
    case Icon::Check:
        StrokeLine(c, cx - r * 0.70f, cy + r * 0.05f, cx - r * 0.18f, cy + r * 0.55f, w * 1.2f,
                   color);
        StrokeLine(c, cx - r * 0.18f, cy + r * 0.55f, cx + r * 0.78f, cy - r * 0.50f, w * 1.2f,
                   color);
        break;
    case Icon::Link: {
        // A signal: a dot and two arcs fanning up from it.
        const float by = cy + r * 0.62f;
        FillDisc(c, cx, by, w * 1.1f, color);
        for (const float radius : {r * 0.62f, r * 1.12f}) {
            float px = 0.0f, py = 0.0f;
            for (int i = 0; i <= 10; ++i) {
                const float a = -2.36f + i * 0.157f; // 45 degrees either side of straight up
                const float x = cx + std::cos(a) * radius, y = by + std::sin(a) * radius;
                if (i > 0) {
                    StrokeLine(c, px, py, x, y, w * 1.1f, color);
                }
                px = x;
                py = y;
            }
        }
        break;
    }
    }
}

/// Word-wraps `text` to `max_w` pixels.
inline std::vector<std::string> WrapLines(Font& font, std::string_view text, int size, int max_w) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    while (i <= text.size()) {
        const std::size_t end = std::min(text.find(' ', i), text.size());
        const std::string word{text.substr(i, end - i)};
        const std::string candidate = line.empty() ? word : line + ' ' + word;
        if (!line.empty() && font.Measure(candidate, size) > max_w) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        i = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

// ---------------------------------------------------------------------------------------------
// The shelf

enum class ShelfStyle {
    Coverflow,
    Shelf,
    Carousel,
    Arc,
    Stack,
    Spotlight,
    Bookshelf,
    Pile,
    Showroom,
    Count,
};

inline const char* ShelfStyleName(ShelfStyle style) {
    switch (style) {
    case ShelfStyle::Coverflow:
        return "Coverflow";
    case ShelfStyle::Shelf:
        return "Shelf";
    case ShelfStyle::Carousel:
        return "Carousel";
    case ShelfStyle::Arc:
        return "Arc";
    case ShelfStyle::Stack:
        return "Stack";
    case ShelfStyle::Spotlight:
        return "Spotlight";
    case ShelfStyle::Bookshelf:
        return "Bookshelf";
    case ShelfStyle::Pile:
        return "Pile";
    case ShelfStyle::Showroom:
        return "Showroom";
    default:
        return "";
    }
}

/// One game on the shelf: its cover if there is one, its icon otherwise.
struct ShelfItem {
    const Image* cover{}; // Front, or the full wrap (back + spine + front) when `full_wrap`.
    bool full_wrap{};
    const std::vector<u32>* icon{}; // Square, `icon_size` a side.
    int icon_size{};
    std::string_view title;
    std::string_view publisher;
    std::string_view badge;             // File type and the like.
    std::vector<std::string> back_text; // Shown on the back when there is no artwork for it.
};

/// A game case of this console is taller than wide: 104 mm by 169.
constexpr float kCaseAspect = 0.615f;
/// In a full wrap the back and the front each take this share of the width; the spine the rest.
// By the case's own measures: two faces of 104 mm and a spine of 11 between them.
constexpr float kWrapFace = 0.475f;

struct ShelfPose {
    float cx{};     // Centre.
    float width{};  // On-screen width.
    float h_near{}; // Height of the edge towards the middle of the shelf.
    float h_far{};  // Height of the edge away from it.
    float lift{};   // How far above the floor it hangs.
    float shade{};
};

/// Where the cover `d` places from the selected one sits. `d` is fractional while the shelf
/// slides, which is the whole animation: the caller eases its position and redraws.
inline ShelfPose PoseFor(ShelfStyle style, float d, float cx, float case_h) {
    const float case_w = case_h * kCaseAspect;
    const float ad = std::abs(d);
    const float sign = d < 0 ? -1.0f : 1.0f;
    const float t = std::min(ad, 1.0f); // 0 in the middle, 1 once fully to the side.
    const float beyond = std::max(0.0f, ad - 1.0f);
    ShelfPose p;
    switch (style) {
    case ShelfStyle::Shelf: // Upright cases side by side, the outer ones turned a little inwards.
        p.cx = cx + sign * case_w * (0.96f * t + 0.70f * beyond);
        p.width = case_w * (1.0f - 0.26f * t);
        p.h_near = case_h * (1.0f - 0.20f * t);
        p.h_far = case_h * (1.0f - 0.27f * t);
        p.shade = std::min(0.45f, 0.06f * t + 0.10f * beyond);
        break;
    case ShelfStyle::Carousel: { // Flat, shrinking with distance.
        const float scale = std::max(0.42f, 1.0f - 0.20f * ad);
        p.cx = cx + sign * case_w * (0.94f * t + 0.74f * beyond);
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.shade = std::min(0.55f, 0.16f * ad);
        break;
    }
    case ShelfStyle::Arc: { // Flat, rising towards the sides like the rim of a bowl.
        const float scale = std::max(0.46f, 1.0f - 0.16f * ad);
        p.cx = cx + sign * case_w * (0.86f * t + 0.62f * beyond);
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.lift = case_h * 0.055f * ad * ad;
        p.shade = std::min(0.50f, 0.12f * ad);
        break;
    }
    case ShelfStyle::Stack: { // A fanned hand of cards.
        const float scale = std::max(0.50f, 1.0f - 0.10f * ad);
        p.cx = cx + sign * case_w * (0.46f * t + 0.20f * beyond);
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.shade = std::min(0.70f, 0.22f * ad);
        break;
    }
    case ShelfStyle::Spotlight: { // One large case, the rest small along the floor.
        const float scale = 1.18f - 0.74f * t;
        p.cx = cx + sign * case_w * (1.02f * t + 0.52f * beyond);
        p.width = case_w * scale;
        p.h_near = p.h_far = case_h * scale;
        p.shade = std::min(0.50f, 0.30f * t + 0.05f * beyond);
        break;
    }
    default: // Coverflow: side covers turn away and close ranks.
        // (farther apart than toronja's, in case widths: these cases are half as wide for their height)
        p.cx = cx + sign * case_w * (1.02f * t + 0.50f * beyond);
        p.width = case_w * (1.0f - 0.60f * t);
        p.h_near = case_h * (1.0f - 0.14f * t);
        p.h_far = case_h * (1.0f - 0.30f * t);
        p.shade = std::min(0.60f, 0.12f * t + 0.10f * beyond);
        break;
    }
    return p;
}

// ---------------------------------------------------------------------------------------------
// Cases in three dimensions: a box with six faces, turned and seen in perspective

/// How thick a case is, as a share of its height.
constexpr float kCaseDepth = 0.105f;

struct BoxPose {
    float cx{}, cy{};             // Where the middle of the case is on screen.
    float h{};                    // Its height on screen when it faces the eye.
    float yaw{}, roll{}, pitch{}; // Radians, applied in this order.
    float shade{};
};

inline BoxPose MixPose(const BoxPose& a, const BoxPose& b, float t) {
    BoxPose p;
    p.cx = a.cx + (b.cx - a.cx) * t;
    p.cy = a.cy + (b.cy - a.cy) * t;
    p.h = a.h + (b.h - a.h) * t;
    p.yaw = a.yaw + (b.yaw - a.yaw) * t;
    p.roll = a.roll + (b.roll - a.roll) * t;
    p.pitch = a.pitch + (b.pitch - a.pitch) * t;
    p.shade = a.shade + (b.shade - a.shade) * t;
    return p;
}

/// A corner on screen. `w` is how much perspective has shrunk things there; `u`, `v` are
/// in the picture's own pixels.
struct BoxVertex {
    float x{}, y{}, w{1.0f};
    float u{}, v{};
};

/// What a triangle needs to know about the face of the case it is part of. `u` and `v` of
/// its corners run 0..1 across that face (left to right, top to bottom, seen from outside).
struct BoxFace {
    int kind{};         // 0 front, 1 back, 2 spine, 3 opening side, 4 top, 5 bottom
    float w{1}, h{1};   // The face's size, in case heights.
    float u0{}, u1{1};  // The part of the picture that goes across it.
    u32 plastic{};      // The case itself, where no print covers it.
};

/// The small things that make a case a case rather than a painted block. Returns false for
/// a point outside the face's rounded corners. `plastic` says the bare case shows there;
/// `dark` is how much darker that point is (a groove, a crease).
inline bool CaseDetail(const BoxFace& f, float s, float t, bool& plastic, float& dark) {
    const float x = s * f.w, y = t * f.h;
    // Corners: gently rounded in the plane of the face.
    const float r = std::min(0.024f, std::min(f.w, f.h) * 0.30f);
    const float cx = x < r ? r - x : x > f.w - r ? x - (f.w - r) : 0.0f;
    const float cy = y < r ? r - y : y > f.h - r ? y - (f.h - r) : 0.0f;
    if (cx > 0.0f && cy > 0.0f && cx * cx + cy * cy > r * r) {
        return false;
    }
    plastic = false;
    dark = 0.0f;
    constexpr float rim = 0.014f; // The edge of the case round the printed sleeve.
    switch (f.kind) {
    case 0:   // Front: the spine is at its left.
    case 1: { // Back: the spine is at its right.
        const float from_spine = f.kind == 0 ? x : f.w - x;
        const float from_open = f.w - from_spine;
        if (y < rim || y > f.h - rim || from_open < rim) {
            plastic = true;
            // The lip the sleeve stops at.
            dark = (y < rim ? rim - y : y > f.h - rim ? y - (f.h - rim) : rim - from_open) < 0.004f
                       ? 0.22f
                       : 0.0f;
        } else if (from_spine > 0.030f && from_spine < 0.036f) {
            dark = 0.20f; // The crease where the lid folds.
        } else if (from_spine < 0.030f) {
            dark = 0.06f; // The hinge is a touch duller than the lid.
        }
        break;
    }
    case 2: // Spine: the sleeve runs over it, the case shows at its ends.
        if (y < rim || y > f.h - rim) {
            plastic = true;
        }
        break;
    case 3: { // The side that opens: two halves meeting, and the notch for a thumb.
        plastic = true;
        const float across = s;
        const bool notch = t > 0.44f && t < 0.56f;
        if (std::abs(across - 0.5f) < (notch ? 0.20f : 0.06f)) {
            dark = notch ? 0.50f : 0.42f;
        } else if (std::abs(across - 0.5f) < (notch ? 0.26f : 0.10f)) {
            dark = 0.14f; // The bevel into the gap.
        }
        break;
    }
    default: { // Top and bottom: the same seam, ending at the hinge (s = 0 is the spine end).
        plastic = true;
        if (x > 0.045f) {
            const float across = std::abs(t - 0.5f);
            if (across < 0.06f) {
                dark = 0.42f;
            } else if (across < 0.10f) {
                dark = 0.14f;
            }
        }
        break;
    }
    }
    return true;
}

/// One triangle of a face, with the picture laid on it in true perspective. Without a
/// picture it is filled with `solid`. With `mirror_floor` at or above zero the pixels are a
/// reflection: they fade out over `mirror_span` pixels below that line.
inline void FillBoxTriangle(Canvas& c, const Image* img, u32 solid, const BoxFace& face,
                            const BoxVertex& a, const BoxVertex& b, const BoxVertex& d,
                            float shade, u32 shade_to, bool smooth, float mirror_floor,
                            float mirror_span) {
    const float area = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
    if (std::abs(area) < 0.5f) {
        return;
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, d.x}))));
    const int x1 = std::min(c.Width() - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, d.x}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, d.y}))));
    const int y1 = std::min(c.Height() - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, d.y}))));
    if (x0 > x1 || y0 > y1) {
        return;
    }
    const float inv_area = 1.0f / area;
    // Weights of a, b and d at a point, each a plane over the screen.
    const float wa_dx = (b.y - d.y) * inv_area, wa_dy = (d.x - b.x) * inv_area;
    const float wb_dx = (d.y - a.y) * inv_area, wb_dy = (a.x - d.x) * inv_area;
    const u32 keep = static_cast<u32>((1.0f - std::clamp(shade, 0.0f, 1.0f)) * 256.0f);
    const u32 lose = 256 - keep;
    const u32 tr = shade_to & 0xFF, tg = (shade_to >> 8) & 0xFF, tb = (shade_to >> 16) & 0xFF;
    const bool textured = img != nullptr && !img->Empty();
    const float iw = textured ? static_cast<float>(img->w - 1) : 0.0f;
    const float ih = textured ? static_cast<float>(img->h - 1) : 0.0f;
    u32* const pixels = c.Data();
    const int stride = c.Width();
    for (int y = y0; y <= y1; ++y) {
        const float py = y + 0.5f;
        u32 fade = 256;
        if (mirror_floor >= 0.0f) {
            const float f = 1.0f - (py - mirror_floor) / std::max(mirror_span, 1.0f);
            if (f <= 0.0f || py < mirror_floor) {
                continue;
            }
            fade = static_cast<u32>(64.0f * f * f);
        }
        const float px0 = x0 + 0.5f;
        float wa = ((b.x - px0) * (d.y - py) - (b.y - py) * (d.x - px0)) * inv_area;
        float wb = ((d.x - px0) * (a.y - py) - (d.y - py) * (a.x - px0)) * inv_area;
        (void)wa_dy;
        (void)wb_dy;
        u32* row = pixels + static_cast<std::size_t>(y) * stride;
        for (int x = x0; x <= x1; ++x, wa += wa_dx, wb += wb_dx) {
            const float wd = 1.0f - wa - wb;
            if (wa < -0.0005f || wb < -0.0005f || wd < -0.0005f) {
                continue;
            }
            u32 color = solid;
            const float q = wa * a.w + wb * b.w + wd * d.w;
            if (q <= 0.0f) {
                continue;
            }
            // Where on the face this pixel is, in true perspective.
            const float fs = (wa * a.u * a.w + wb * b.u * b.w + wd * d.u * d.w) / q;
            const float ft = (wa * a.v * a.w + wb * b.v * b.w + wd * d.v * d.w) / q;
            bool bare = false;
            float dark = 0.0f;
            if (!CaseDetail(face, fs, ft, bare, dark)) {
                continue;
            }
            if (bare) {
                color = face.plastic;
            } else if (textured) {
                const float u = (face.u0 + (face.u1 - face.u0) * fs) * iw;
                const float v = ft * ih;
                if (smooth) {
                    color = SampleImage(*img, u, v);
                } else {
                    const int ui = std::clamp(static_cast<int>(u + 0.5f), 0, img->w - 1);
                    const int vi = std::clamp(static_cast<int>(v + 0.5f), 0, img->h - 1);
                    color = img->px[static_cast<std::size_t>(vi) * img->w + ui];
                }
                if ((color >> 24) < 32) {
                    color = solid; // A see-through pixel of an icon: the case shows through.
                }
            }
            u32 r = color & 0xFF, g = (color >> 8) & 0xFF, bl = (color >> 16) & 0xFF;
            if (dark > 0.0f) {
                const u32 lit = static_cast<u32>((1.0f - dark) * 256.0f);
                r = (r * lit) >> 8;
                g = (g * lit) >> 8;
                bl = (bl * lit) >> 8;
            }
            if (keep < 256) {
                r = (r * keep + tr * lose) >> 8;
                g = (g * keep + tg * lose) >> 8;
                bl = (bl * keep + tb * lose) >> 8;
            }
            u32& dst = row[x];
            if (fade < 256) {
                const u32 inv = 256 - fade;
                r = (r * fade + (dst & 0xFF) * inv) >> 8;
                g = (g * fade + ((dst >> 8) & 0xFF) * inv) >> 8;
                bl = (bl * fade + ((dst >> 16) & 0xFF) * inv) >> 8;
            }
            dst = 0xFF000000u | (bl << 16) | (g << 8) | r;
        }
    }
}

inline void DrawTextBack(Canvas& c, Font& font, const ShelfItem& item, int x, int y, int w, int h);

/// Draws one game as a case: front, back, spine and the plain sides, whichever of them
/// face the eye. A full wrap supplies the back and the spine; a front-only cover gets a
/// spine in its own colours and the text back once it is square on. A game with no cover
/// wears its icon on the front. With `mirror_floor` at or above zero the case is drawn as
/// its own reflection in a floor at that height.
inline void DrawCase3D(Canvas& c, Font& font, const ShelfItem& item, const BoxPose& pose,
                       u32 shade_to, bool smooth, float mirror_floor = -1.0f) {
    static Image icon_face;
    const Image* art = item.cover != nullptr && !item.cover->Empty() ? item.cover : nullptr;
    bool wrap = art != nullptr && item.full_wrap;
    if (art == nullptr && item.icon != nullptr && !item.icon->empty() && item.icon_size > 0) {
        icon_face.w = icon_face.h = item.icon_size;
        icon_face.px = *item.icon;
        art = &icon_face;
        wrap = false;
    }
    const u32 plastic = g_theme_light ? MakeColor(214, 218, 224) : MakeColor(226, 229, 234);
    const u32 plastic_dark = MakeColor(150, 155, 164);
    u32 card = MakeColor(58, 64, 78); // The face of a case with nothing printed on it.
    if (art != nullptr && !wrap) {
        // No spine was printed for this one: give it the cover's own colour, darkened, so
        // the case reads as one object.
        u32 r = 0, g = 0, b = 0, n = 0;
        for (int gy = 1; gy <= 5; ++gy) {
            for (int gx = 1; gx <= 5; ++gx) {
                const u32 px = art->px[static_cast<std::size_t>(art->h * gy / 6) * art->w +
                                       static_cast<std::size_t>(art->w * gx / 6)];
                if ((px >> 24) < 32) {
                    continue;
                }
                r += px & 0xFF;
                g += (px >> 8) & 0xFF;
                b += (px >> 16) & 0xFF;
                ++n;
            }
        }
        if (n > 0) {
            card = MakeColor(static_cast<u8>(r * 7 / (n * 10)), static_cast<u8>(g * 7 / (n * 10)),
                             static_cast<u8>(b * 7 / (n * 10)));
        }
    }

    const float H = pose.h, W = H * kCaseAspect, D = H * kCaseDepth;
    const float cyaw = std::cos(pose.yaw), syaw = std::sin(pose.yaw);
    const float croll = std::cos(pose.roll), sroll = std::sin(pose.roll);
    const float cpitch = std::cos(pose.pitch), spitch = std::sin(pose.pitch);
    const float eye = 3.4f * H;
    struct P3 {
        float x, y, z;
    };
    const auto turn = [&](float x, float y, float z) {
        const float x1 = x * cyaw + z * syaw;
        const float z1 = -x * syaw + z * cyaw;
        const float x2 = x1 * croll - y * sroll;
        const float y2 = x1 * sroll + y * croll;
        return P3{x2, y2 * cpitch - z1 * spitch, y2 * spitch + z1 * cpitch};
    };
    const auto project = [&](int sx, int sy, int sz) {
        const P3 p = turn(sx * W / 2, sy * H / 2, sz * D / 2);
        const float s = eye / std::max(eye + p.z, eye * 0.2f);
        BoxVertex out;
        out.x = pose.cx + p.x * s;
        out.y = pose.cy + p.y * s;
        out.w = s;
        return out;
    };

    struct Face {
        int corner[4][3]; // TL, TR, BR, BL as seen from outside.
        int normal[3];
        int kind; // 0 front, 1 back, 2 spine, 3 right, 4 top, 5 bottom
    };
    static constexpr Face faces[] = {
        {{{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}}, {0, 0, -1}, 0},
        {{{1, -1, 1}, {-1, -1, 1}, {-1, 1, 1}, {1, 1, 1}}, {0, 0, 1}, 1},
        {{{-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1}}, {-1, 0, 0}, 2},
        {{{1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1}}, {1, 0, 0}, 3},
        {{{-1, -1, 1}, {1, -1, 1}, {1, -1, -1}, {-1, -1, -1}}, {0, -1, 0}, 4},
        {{{-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}}, {0, 1, 0}, 5},
    };
    const float mirror_span = H * 0.42f;
    for (const Face& face : faces) {
        BoxVertex v[4];
        for (int i = 0; i < 4; ++i) {
            v[i] = project(face.corner[i][0], face.corner[i][1], face.corner[i][2]);
        }
        // Clockwise on screen means it faces the eye.
        const float facing =
            (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x);
        if (facing <= 0.5f) {
            continue;
        }
        const P3 n = turn(static_cast<float>(face.normal[0]), static_cast<float>(face.normal[1]),
                          static_cast<float>(face.normal[2]));
        // Faces turned away from the eye take less light.
        const float light = 0.62f + 0.38f * std::clamp(-n.z, 0.0f, 1.0f);
        const float shade = std::min(1.0f, pose.shade + (1.0f - light) * 0.75f);

        const Image* img = nullptr;
        u32 solid = plastic;
        float u0 = 0.0f, u1 = 1.0f;
        switch (face.kind) {
        case 0:
            img = art;
            solid = card;
            u0 = wrap ? 1.0f - kWrapFace : 0.0f;
            break;
        case 1:
            img = wrap ? art : nullptr;
            solid = wrap || art == nullptr
                        ? (g_theme_light ? MakeColor(236, 238, 242) : MakeColor(44, 48, 58))
                        : card;
            u1 = wrap ? kWrapFace : 1.0f;
            break;
        case 2:
            // The spine of a wrap; otherwise the cover's colour.
            img = wrap ? art : nullptr;
            solid = card;
            u0 = kWrapFace;
            u1 = 1.0f - kWrapFace;
            break;
        case 3:
            solid = plastic;
            break;
        default:
            solid = face.kind == 4 ? plastic : plastic_dark;
            break;
        }
        v[0].u = v[3].u = 0.0f;
        v[1].u = v[2].u = 1.0f;
        v[0].v = v[1].v = 0.0f;
        v[2].v = v[3].v = 1.0f;
        BoxFace info;
        info.kind = face.kind;
        info.w = face.kind == 2 || face.kind == 3 ? kCaseDepth : kCaseAspect;
        info.h = face.kind >= 4 ? kCaseDepth : 1.0f;
        info.u0 = u0;
        info.u1 = u1;
        info.plastic = face.kind == 5 ? plastic_dark : plastic;
        if (mirror_floor >= 0.0f) {
            for (BoxVertex& vertex : v) {
                vertex.y = 2.0f * mirror_floor - vertex.y;
            }
        }
        FillBoxTriangle(c, img, solid, info, v[0], v[1], v[2], shade, shade_to, smooth,
                        mirror_floor, mirror_span);
        FillBoxTriangle(c, img, solid, info, v[0], v[2], v[3], shade, shade_to, smooth,
                        mirror_floor, mirror_span);

        // A back with nothing printed on it says what is known about the game, once it is
        // square on and large enough to read.
        if (face.kind == 1 && !wrap && mirror_floor < 0.0f && std::abs(v[1].y - v[0].y) < 2.5f &&
            std::abs(v[3].x - v[0].x) < 2.5f && v[1].x - v[0].x > 150.0f) {
            DrawTextBack(c, font, item, static_cast<int>(v[0].x), static_cast<int>(v[0].y),
                         static_cast<int>(v[1].x - v[0].x), static_cast<int>(v[3].y - v[0].y));
        }
    }
}

/// The back of a case that has no back artwork: what is known about the game, as text.
inline void DrawTextBack(Canvas& c, Font& font, const ShelfItem& item, int x, int y, int w, int h) {
    c.FillRoundRect(x, y, w, h, 10, kColSurface);
    c.FillRoundRect(x, y, std::max(3, w / 40), h, 2, kColAccent);
    if (w < 120) {
        return;
    }
    const int pad = w / 12;
    int ty = y + pad + 22;
    for (const std::string& line : WrapLines(font, item.title, 22, w - 2 * pad)) {
        font.Draw(c, x + pad, ty, line, 22, kColText);
        ty += 28;
    }
    if (!item.publisher.empty()) {
        font.Draw(c, x + pad, ty, font.Truncate(item.publisher, 16, w - 2 * pad), 16, kColAccent);
        ty += 26;
    }
    ty += 8;
    for (const std::string& line : item.back_text) {
        if (ty > y + h - pad) {
            break;
        }
        font.Draw(c, x + pad, ty, font.Truncate(line, 16, w - 2 * pad), 16, kColTextDim);
        ty += 23;
    }
}

struct ShelfView {
    int x{}, y{}, w{}, h{}; // Area the shelf may use.
    ShelfStyle style{ShelfStyle::Coverflow};
    float position{}; // Eased index of the selected game.
    int count{};
    float flip{}; // 0 = front, 1 = back, in between while turning.
    float time{}; // Seconds; drives the gentle bobbing of the games that are not selected.
    float zoom{};       // 0..1: the selected case brought forward, before it is started.
    float spin_yaw{};   // The selected case turned by hand (the right stick), radians.
    float spin_pitch{};
    float look_scale{1.0f};     // Forward: nearer or farther (ZL / ZR).
    float look_x{}, look_y{};   // Forward: the view moved sideways and up or down, pixels.
};

inline float ShelfCaseHeight(const ShelfView& v) {
    return std::min(v.h * 0.58f, v.w * 0.40f / kCaseAspect * 1.6f);
}

/// Y of the floor the games stand on (and are reflected in).
inline float ShelfFloorY(const ShelfView& v) {
    return v.y + v.h * 0.47f + ShelfCaseHeight(v) / 2;
}

/// A number between -1 and 1 that is always the same for the same game: the small
/// irregularities of things stacked by hand.
inline float ShelfJitter(int index, int salt) {
    u32 h = static_cast<u32>(index) * 2654435761u + static_cast<u32>(salt) * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return static_cast<float>(h & 0xFFFF) / 32767.5f - 1.0f;
}

/// Where the selected case goes when it is brought forward (`zoom` 0..1) and turned by hand.
inline BoxPose ForwardPose(const ShelfView& v, BoxPose pose) {
    if (v.zoom > 0.0f) {
        const float t = v.zoom * v.zoom * (3.0f - 2.0f * v.zoom);
        const float big = std::min(v.h * 0.74f, pose.h * 1.34f);
        pose.cx += (v.x + v.w / 2.0f - pose.cx) * t;
        pose.cy += (v.y + v.h * 0.47f - pose.cy) * t;
        pose.h += (big - pose.h) * t;
        pose.shade *= 1.0f - t;
        pose.roll *= 1.0f - t;
        // Square on once it is forward, whatever the shelf had it doing.
        pose.yaw *= 1.0f - t;
        pose.pitch *= 1.0f - t;
        // Looked at from nearer or farther, and from a little to one side.
        pose.h *= 1.0f + (v.look_scale - 1.0f) * t;
        pose.cx += v.look_x * t;
        pose.cy += v.look_y * t;
    }
    pose.yaw += -3.14159265f * v.flip + v.spin_yaw;
    pose.pitch += v.spin_pitch;
    return pose;
}

/// Darkens the shelf behind a case that has been brought forward.
inline void DimShelf(Canvas& c, const ShelfView& v, u32 towards) {
    if (v.zoom <= 0.01f) {
        return;
    }
    const u32 lose = static_cast<u32>(std::clamp(v.zoom, 0.0f, 1.0f) * 150.0f);
    const u32 keep = 256 - lose;
    const u32 tr = (towards & 0xFF) * lose, tg = ((towards >> 8) & 0xFF) * lose,
              tb = ((towards >> 16) & 0xFF) * lose;
    u32* const pixels = c.Data();
    const int y1 = c.Height(); // The whole screen: a band would show its edges.
    for (int y = 0; y < y1; ++y) {
        u32* row = pixels + static_cast<std::size_t>(y) * c.Width();
        for (int x = 0; x < c.Width(); ++x) {
            const u32 p = row[x];
            const u32 r = ((p & 0xFF) * keep + tr) >> 8;
            const u32 g = (((p >> 8) & 0xFF) * keep + tg) >> 8;
            const u32 b = (((p >> 16) & 0xFF) * keep + tb) >> 8;
            row[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

/// The styles made of real cases: a bookshelf of spines, a pile the selected game is drawn
/// out of, and a showroom row.
inline void DrawShelfCases(Canvas& c, Font& font, const ShelfView& v,
                           const std::function<ShelfItem(int)>& get) {
    const float case_h = ShelfCaseHeight(v);
    const float case_w = case_h * kCaseAspect;
    const float cx = v.x + v.w / 2.0f;
    const float floor = ShelfFloorY(v);
    const int centre = static_cast<int>(std::lround(v.position));
    const u32 shade_to = g_theme_light ? kColShelfHorizon : MakeColor(0, 0, 0);
    const float wobble_on = g_shelf_floating ? 1.0f : 0.0f;
    constexpr float half_pi = 1.5707963f;

    const auto pose_for = [&](int index) {
        const float d = index - v.position;
        const float ad = std::abs(d);
        const float sign = d < 0 ? -1.0f : 1.0f;
        const float t = std::min(ad, 1.0f);
        const float ease = t * t * (3.0f - 2.0f * t);
        const float beyond = std::max(0.0f, ad - 1.0f);
        const float sway = wobble_on * std::sin(v.time * 1.2f + index * 2.1f);
        BoxPose p;
        switch (v.style) {
        case ShelfStyle::Bookshelf: {
            // A row of spines that never moves aside: every book keeps its place. The
            // selected one is lifted out of its slot, towards the room, and turned to show
            // its front; choosing another slides it back in while the next comes out.
            BoxPose slot;
            // A spine is the near end of a case seen end-on, so perspective draws it
            // larger than the case's nominal height: size the case for the spine wanted.
            const float seen = case_h * 0.80f;
            const float nearer = 3.4f / (3.4f - kCaseAspect / 2);
            slot.h = seen / nearer;
            const float thick = seen * kCaseDepth;
            slot.cx = cx + d * (thick + 3.0f);
            slot.cy = floor - seen / 2;
            slot.yaw = -half_pi;
            slot.shade = std::min(0.42f, 0.04f + 0.012f * ad);
            BoxPose out;
            out.h = case_h * 0.92f;
            out.cx = slot.cx;
            // Held above the row, which stays in view under it.
            out.cy = floor - out.h / 2 - seen * 0.42f - wobble_on * case_h * 0.006f * (1.0f + sway);
            out.yaw = 0.0f;
            p = MixPose(slot, out, 1.0f - ease);
            (void)sign;
            (void)beyond;
            break;
        }
        case ShelfStyle::Pile: {
            // Lying flat one on another, spines to the room. The selected game is drawn
            // out of the pile and stood up beside it.
            const float len = case_h * 0.80f;
            const float thick = len * kCaseDepth;
            BoxPose lying;
            lying.h = len;
            lying.cx = v.x + v.w * 0.24f + ShelfJitter(index, 1) * 12.0f;
            lying.cy = floor - thick / 2 - (d + 4.0f) * (thick + 1.5f);
            lying.yaw = -half_pi + ShelfJitter(index, 2) * 0.05f;
            lying.roll = -half_pi;
            lying.pitch = 0.17f;
            lying.shade = std::min(0.40f, 0.04f + 0.03f * ad);
            BoxPose standing;
            standing.h = case_h;
            standing.cx = v.x + v.w * 0.62f;
            standing.cy = floor - case_h / 2;
            standing.yaw = -0.40f + 0.03f * sway;
            standing.pitch = 0.05f;
            p = MixPose(lying, standing, 1.0f - ease);
            break;
        }
        default: { // Showroom: a row of cases, each turned towards the one in the middle.
            p.h = case_h * (1.0f - 0.20f * ease);
            p.cx = cx + sign * case_w * (0.98f * ease + 0.80f * beyond);
            p.cy = floor - p.h / 2 - wobble_on * t * case_h * 0.012f * (1.0f + sway);
            p.yaw = sign * 0.66f * ease + 0.05f * sway * t;
            p.pitch = 0.06f * t;
            p.shade = std::min(0.50f, 0.07f * t + 0.10f * beyond);
            break;
        }
        }
        return p;
    };

    const bool pile = v.style == ShelfStyle::Pile;
    const int reach = v.style == ShelfStyle::Bookshelf ? 26 : pile ? 9 : 3;
    const auto draw = [&](int index, bool mirror) {
        if (index < 0 || index >= v.count) {
            return;
        }
        const bool selected = index == centre;
        BoxPose p = pose_for(index);
        if (pile && !selected && index - v.position + 4.0f < -0.4f) {
            return; // Under the floor: the pile goes on, out of sight.
        }
        if (selected) {
            p = ForwardPose(v, p);
        }
        if (p.cx < v.x - p.h || p.cx > v.x + v.w + p.h) {
            return;
        }
        const ShelfItem item = get(index);
        if (mirror) {
            DrawCase3D(c, font, item, p, shade_to, false, floor);
        } else {
            DrawCase3D(c, font, item, p, shade_to, selected);
        }
    };

    if (pile) {
        // Bottom to top, so each case covers the one under it.
        for (int k = -reach; k <= reach; ++k) {
            if (centre + k != centre) {
                draw(centre + k, false);
            }
        }
    } else {
        if (g_shelf_reflections) {
            for (int k = reach; k >= 1; --k) {
                draw(centre - k, true);
                draw(centre + k, true);
            }
        }
        for (int k = reach; k >= 1; --k) {
            draw(centre - k, false);
            draw(centre + k, false);
        }
    }
    DimShelf(c, v, shade_to);
    if (g_shelf_reflections && v.zoom < 0.5f) {
        draw(centre, true);
    }
    draw(centre, false);
}

/// Draws the shelf. `get` supplies each game on demand, so nothing is built for games that are
/// off screen.
inline void DrawShelf(Canvas& c, Font& font, const ShelfView& v,
                      const std::function<ShelfItem(int)>& get) {
    if (v.count <= 0) {
        return;
    }
    if (v.style == ShelfStyle::Bookshelf || v.style == ShelfStyle::Pile ||
        v.style == ShelfStyle::Showroom) {
        DrawShelfCases(c, font, v, get);
        return;
    }
    const float case_h = ShelfCaseHeight(v);
    const float cx = v.x + v.w / 2.0f;
    const float floor = ShelfFloorY(v);
    const int centre = static_cast<int>(std::lround(v.position));
    constexpr int reach = 5;
    // Covers darken into the backdrop: towards black on dark, towards the floor on light.
    const u32 shade_to = g_theme_light ? kColShelfHorizon : MakeColor(0, 0, 0);
    static Image icon_image; // Reused: an icon is copied into it to be drawn like a cover.

    const auto draw_one = [&](int index) {
        const float d = index - v.position;
        const float away = std::min(std::abs(d), 1.0f);
        const ShelfPose p = PoseFor(v.style, d, cx, case_h);
        // Games that are not selected hover, each to its own beat.
        const float bob = !g_shelf_floating ? 0.0f
                                            : away * case_h * 0.014f *
                                                  (1.0f + std::sin(v.time * 1.7f + index * 1.9f));
        const float gap = p.lift + bob;
        const bool left_side = d < 0;

        const ShelfItem item = get(index);
        const bool selected = index == centre;
        const float turn = selected ? v.flip : 0.0f;
        const bool show_back = turn > 0.5f;
        // Turning about the vertical axis: the case narrows to nothing, then widens again
        // showing its other side.
        const float squash =
            turn > 0.0f ? std::max(std::abs(std::cos(turn * 3.14159265f)), 0.02f) : 1.0f;

        const bool has_art = item.cover != nullptr && !item.cover->Empty();
        const bool has_icon = item.icon != nullptr && !item.icon->empty() && item.icon_size > 0;

        if (selected && g_shelf_cases && has_art) {
            // The selected game is a real case: it can be turned over, turned by hand and
            // brought forward. Its place and lean come from the flat pose it stands in for.
            BoxPose box;
            box.h = p.h_near;
            box.cx = p.cx;
            box.cy = floor - gap - p.h_near / 2;
            box.shade = p.shade;
            const float lean = v.style == ShelfStyle::Coverflow ? 1.10f
                               : v.style == ShelfStyle::Shelf   ? 0.42f
                                                                : 0.0f;
            box.yaw = -d * lean;
            const BoxPose forward = ForwardPose(v, box);
            if (g_shelf_reflections && v.zoom < 0.5f) {
                DrawCase3D(c, font, item, forward, shade_to, false, floor);
            }
            DrawCase3D(c, font, item, forward, shade_to, true);
            return;
        }

        if (!has_art) {
            // No cover: the game's icon on its own, standing on the floor like the cases do.
            // Brought forward, it grows and rises to the middle like a case does.
            const float forward = selected ? v.zoom * v.zoom * (3.0f - 2.0f * v.zoom) : 0.0f;
            const float side = p.h_near * 0.62f * (1.0f + 0.55f * forward);
            const float rise =
                forward * std::max(0.0f, floor - gap - side / 2 - (v.y + v.h * 0.50f));
            if (show_back) {
                const float w = case_h * kCaseAspect * squash;
                DrawTextBack(c, font, item, static_cast<int>(p.cx - w / 2),
                             static_cast<int>(floor - case_h), static_cast<int>(w),
                             static_cast<int>(case_h));
                return;
            }
            Quad q;
            q.x0 = p.cx - side * squash / 2;
            q.x1 = p.cx + side * squash / 2;
            q.h0 = q.h1 = side;
            q.top0 = q.top1 = floor - gap - side - rise;
            if (has_icon) {
                icon_image.w = icon_image.h = item.icon_size;
                icon_image.px = *item.icon;
                if (forward < 0.5f) {
                    BlitQuad(c, icon_image, q, 0.0f, 1.0f, p.shade, shade_to, true, false, gap);
                }
                BlitQuad(c, icon_image, q, 0.0f, 1.0f, p.shade, shade_to, false, selected);
            } else {
                // Not even an icon: a soft disc with the first letter of the title.
                const float r = side / 2;
                FillDisc(c, p.cx, floor - gap - r, r * squash, MixColor(kColBadge, shade_to, p.shade));
                if (!item.title.empty() && squash > 0.6f) {
                    const std::string letter{item.title.substr(0, 1)};
                    const int size = static_cast<int>(r);
                    font.Draw(c, static_cast<int>(p.cx) - font.Measure(letter, size) / 2,
                              static_cast<int>(floor - gap - r + size * 0.35f), letter, size,
                              MixColor(kColText, shade_to, p.shade));
                }
            }
            return;
        }

        Quad q;
        const float half = p.width * squash / 2;
        q.x0 = p.cx - half;
        q.x1 = p.cx + half;
        // The edge towards the middle of the shelf is the tall one.
        const float h_left = left_side ? p.h_far : p.h_near;
        const float h_right = left_side ? p.h_near : p.h_far;
        q.h0 = p.h_near + (h_left - p.h_near) * away;
        q.h1 = p.h_near + (h_right - p.h_near) * away;
        if (g_shelf_floating && g_shelf_cases && !selected) {
            // Hovering turns them a touch to and fro, enough to catch a side.
            const float lean = 0.022f * away * std::sin(v.time * 1.25f + index * 2.1f);
            q.h0 *= 1.0f + lean;
            q.h1 *= 1.0f - lean;
        }
        q.top0 = floor - gap - q.h0;
        q.top1 = floor - gap - q.h1;

        float u0 = 0.0f, u1 = 1.0f;
        if (item.full_wrap) {
            // back | spine | front
            u0 = show_back ? 0.0f : 1.0f - kWrapFace;
            u1 = show_back ? kWrapFace : 1.0f;
        }
        if (show_back && !item.full_wrap) {
            DrawTextBack(c, font, item, static_cast<int>(q.x0), static_cast<int>(q.top0),
                         static_cast<int>(q.x1 - q.x0), static_cast<int>(q.h0));
            return;
        }
        if (g_shelf_cases && turn <= 0.0f) {
            // A case, not a picture: the side that the angle shows. The tall edge is the
            // one nearer the eye, so the side beyond it is in view; seen square on, a sliver
            // of the spine keeps it a box.
            static Image plastic;
            if (plastic.Empty()) {
                plastic.w = plastic.h = 2;
                plastic.px.assign(4, MakeColor(226, 229, 234) | 0xFF000000u);
            }
            const float depth = case_h * 0.085f;
            const float tall = std::max(q.h0, q.h1);
            const float tilt = tall > 1.0f ? std::abs(q.h0 - q.h1) / tall : 0.0f;
            const float turned = std::min(1.0f, tilt * 5.0f);
            const bool right_near = q.h1 > q.h0;
            const float left_w = depth * (right_near ? 0.3f * (1.0f - std::min(1.0f, tilt * 12.0f))
                                                     : std::max(0.3f, turned));
            const float right_w = right_near ? depth * turned : 0.0f;
            const float side_shade = std::min(1.0f, p.shade + 0.22f);
            const auto side = [&](float x_outer, float x_inner, float h_inner, bool spine) {
                Quad s;
                const bool outer_left = x_outer < x_inner;
                const float h_outer = h_inner * (1.0f - 0.07f * std::abs(x_outer - x_inner) / depth);
                s.x0 = outer_left ? x_outer : x_inner;
                s.x1 = outer_left ? x_inner : x_outer;
                s.h0 = outer_left ? h_outer : h_inner;
                s.h1 = outer_left ? h_inner : h_outer;
                s.top0 = floor - gap - s.h0;
                s.top1 = floor - gap - s.h1;
                const bool art = spine && item.full_wrap;
                const Image& img = art ? *item.cover : plastic;
                const float su0 = art ? kWrapFace : 0.0f, su1 = art ? 1.0f - kWrapFace : 1.0f;
                BlitQuad(c, img, s, su0, su1, side_shade, shade_to, true, false, gap);
                BlitQuad(c, img, s, su0, su1, side_shade, shade_to, false, false);
            };
            if (left_w >= 1.0f) {
                side(q.x0 - left_w, q.x0, q.h0, true);
            }
            if (right_w >= 1.0f) {
                side(q.x1 + right_w, q.x1, q.h1, false);
            }
        }
        BlitQuad(c, *item.cover, q, u0, u1, p.shade, shade_to, true, false, gap);
        BlitQuad(c, *item.cover, q, u0, u1, p.shade, shade_to, false, selected);
    };

    // Far to near, so nearer covers overlap the ones behind; the selected one goes last.
    for (int k = reach; k >= 1; --k) {
        if (centre - k >= 0) {
            draw_one(centre - k);
        }
        if (centre + k < v.count) {
            draw_one(centre + k);
        }
    }
    DimShelf(c, v, shade_to);
    if (centre >= 0 && centre < v.count) {
        draw_one(centre);
    }
}

// ---------------------------------------------------------------------------------------------
// Buttons and bars

/// A round button with an icon, and optionally the pad button that triggers it as a small
/// chip. Hit-testing is the caller's (centre + radius).
inline void DrawRoundButton(Canvas& c, Font& font, Icon icon, int cx, int cy, int radius,
                            std::string_view pad_hint, bool highlighted) {
    const u32 fill = highlighted ? kColAccent : kColSurfaceHi;
    const u32 fg = highlighted ? kColOnAccent : kColText;
    FillDisc(c, static_cast<float>(cx), static_cast<float>(cy), radius + 1.5f, kColLine);
    FillDisc(c, static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(radius), fill);
    DrawIcon(c, icon, static_cast<float>(cx), static_cast<float>(cy), radius * 1.05f, fg);
    if (!pad_hint.empty()) {
        const int tw = font.Measure(pad_hint, 15);
        const int chip_w = std::max(22, tw + 12);
        const int chip_x = cx + radius - chip_w / 2 - 2;
        const int chip_y = cy - radius - 6;
        c.FillRoundRect(chip_x, chip_y, chip_w, 22, 11, kColBadge);
        font.Draw(c, chip_x + (chip_w - tw) / 2, CenterBaseline(chip_y, 22, 15), pad_hint, 15,
                  kColText);
    }
}

/// Width DrawPill gives a pill.
inline int PillWidth(Font& font, std::string_view pad_hint, std::string_view label) {
    const int hint_w = pad_hint.empty() ? 0 : std::max(24, font.Measure(pad_hint, 16) + 14);
    return 14 + hint_w + (hint_w ? 8 : 0) + font.Measure(label, 17) + 16;
}

/// A text button (pill).
inline int DrawPill(Canvas& c, Font& font, int x, int y, std::string_view pad_hint,
                    std::string_view label, bool highlighted = false) {
    constexpr int h = 34;
    const int hint_w = pad_hint.empty() ? 0 : std::max(24, font.Measure(pad_hint, 16) + 14);
    const int w = PillWidth(font, pad_hint, label);
    c.FillRoundRect(x, y, w, h, h / 2, highlighted ? kColAccent : kColSurfaceHi);
    int pen = x + 14;
    if (hint_w) {
        c.FillRoundRect(pen - 6, y + 5, hint_w, h - 10, (h - 10) / 2,
                        highlighted ? kColOnAccent : kColBadge);
        font.Draw(c, pen - 6 + (hint_w - font.Measure(pad_hint, 16)) / 2,
                  CenterBaseline(y + 5, h - 10, 16), pad_hint, 16,
                  highlighted ? kColAccent : kColText);
        pen += hint_w + 2;
    }
    font.Draw(c, pen, CenterBaseline(y, h, 17), label, 17, highlighted ? kColOnAccent : kColText);
    return w;
}

// ---------------------------------------------------------------------------------------------
// Options: the navigation drawer and its companions

struct DrawerItem {
    std::string_view label;
    bool header{};   // A section title, not selectable.
    bool advanced{}; // Listed under the Advanced section.
};

constexpr int kDrawerW = 268;
constexpr int kDrawerItemH = 46;
constexpr int kDrawerHeaderH = 38;

inline int DrawerItemHeight(const DrawerItem& item) {
    return item.header ? kDrawerHeaderH : kDrawerItemH;
}

/// Y of item `index` relative to the top of the list.
inline int DrawerItemOffset(std::span<const DrawerItem> items, int index) {
    int y = 0;
    for (int i = 0; i < index && i < static_cast<int>(items.size()); ++i) {
        y += DrawerItemHeight(items[i]);
    }
    return y;
}

/// Draws the drawer. `active` is the page on screen, `cursor` the highlighted item while the
/// drawer has the focus. `scroll` is in pixels, `time` in seconds (the focus ring breathes).
inline void DrawDrawer(Canvas& c, Font& font, int x, int y, int h, std::string_view title,
                       std::string_view subtitle, std::span<const DrawerItem> items, int active,
                       int cursor, bool focused, int scroll, float time) {
    c.FillRect(x, y, kDrawerW, h, kColRail);
    const int list_top = y + 84;
    const int list_bottom = y + h - 8;
    int iy = list_top - scroll;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const DrawerItem& item = items[i];
        const int ih = DrawerItemHeight(item);
        if (iy + ih > list_top && iy < list_bottom) {
            if (item.header) {
                if (iy + 8 > list_top) {
                    c.FillRect(x + 20, iy + 8, kDrawerW - 40, 1, kColLine);
                }
                const u32 color = item.advanced ? kColWarnText : kColTextDim;
                int tx = x + 24;
                if (item.advanced) {
                    DrawIcon(c, Icon::Warning, tx + 7.0f, iy + 24.0f, 15.0f, color);
                    tx += 22;
                }
                font.Draw(c, tx, iy + 30, item.label, 14, color);
            } else {
                const bool on_cursor = focused && i == cursor;
                const bool is_active = i == active;
                if (on_cursor) {
                    DrawFocusRing(c, x + 14, iy + 4, kDrawerW - 28, ih - 8, 6, time, kColSurfaceHi);
                }
                if (is_active) {
                    // The page on screen: accent text and a bar at its left, as a console's
                    // settings list marks it.
                    c.FillRoundRect(x + 22, iy + 11, 4, ih - 22, 2, kColAccent);
                }
                font.Draw(c, x + 38, CenterBaseline(iy, ih, 19), item.label, 19,
                          is_active ? kColAccent : kColText);
            }
        }
        iy += ih;
    }
    // The title stays put; the list scrolls under it.
    c.FillRect(x, y, kDrawerW, 84, kColRail);
    font.Draw(c, x + 24, y + 44, title, 26, kColText);
    if (!subtitle.empty()) {
        font.Draw(c, x + 24, y + 66, subtitle, 14, kColTextDim);
    }
    c.FillRect(x + 20, y + 83, kDrawerW - 40, 1, kColLine);
    c.FillRect(x + kDrawerW - 1, y, 1, h, kColLine);
}

/// A notice across the top of a page. Returns its height.
inline int DrawBanner(Canvas& c, Font& font, int x, int y, int w, std::string_view title,
                      std::string_view text) {
    const std::vector<std::string> lines = WrapLines(font, text, 16, w - 76);
    const int h = 40 + static_cast<int>(lines.size()) * 21 + 10;
    c.FillRoundRect(x, y, w, h, 8, kColWarnBg);
    c.FillRoundRect(x, y, 6, h, 3, kColWarnText);
    DrawIcon(c, Icon::Warning, x + 34.0f, y + 26.0f, 24.0f, kColWarnText);
    font.Draw(c, x + 58, y + 30, title, 18, kColWarnText);
    int ty = y + 54;
    for (const std::string& line : lines) {
        font.Draw(c, x + 58, ty, line, 16, kColWarnText);
        ty += 21;
    }
    return h;
}

/// Geometry of the info sheet, for hit-testing a tap outside it.
struct SheetRect {
    int x{}, y{}, w{}, h{};
};

/// A sheet rising from the bottom that explains the focused option.
inline SheetRect DrawInfoSheet(Canvas& c, Font& font, int screen_w, int screen_h, int left,
                               std::string_view title, std::string_view body,
                               std::span<const std::string> facts,
                               std::string_view dismiss_hint) {
    const int w = std::min(screen_w - left - 48, 860);
    const int x = left + (screen_w - left - w) / 2;
    const std::vector<std::string> lines = WrapLines(font, body, 19, w - 64);
    const int h = 96 + static_cast<int>(lines.size()) * 27 + static_cast<int>(facts.size()) * 24 +
                  (facts.empty() ? 0 : 14) + 44;
    const int y = screen_h - h - 28;
    // Scrim.
    c.FillRect(left, 0, screen_w - left, screen_h, WithAlpha(MakeColor(0, 0, 0), 110));
    c.FillRoundRect(x - 1, y - 1, w + 2, h + 2, 13, kColLine);
    c.FillRoundRect(x, y, w, h, 12, kColSurfaceHi);
    c.FillRoundRect(x + w / 2 - 24, y + 10, 48, 4, 2, kColBadge);
    DrawIcon(c, Icon::Info, x + 44.0f, y + 54.0f, 26.0f, kColAccent);
    font.Draw(c, x + 70, y + 62, font.Truncate(title, 24, w - 110), 24, kColText);
    int ty = y + 104;
    for (const std::string& line : lines) {
        font.Draw(c, x + 32, ty, line, 19, kColText);
        ty += 27;
    }
    if (!facts.empty()) {
        ty += 6;
        for (const std::string& fact : facts) {
            font.Draw(c, x + 32, ty, font.Truncate(fact, 16, w - 64), 16, kColTextDim);
            ty += 24;
        }
    }
    const int hw = font.Measure(dismiss_hint, 15);
    font.Draw(c, x + w - 32 - hw, y + h - 18, dismiss_hint, 15, kColTextDim);
    return {x, y, w, h};
}

} // namespace SwitchFrontend::Ui
