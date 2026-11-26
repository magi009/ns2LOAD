#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the program's icon (icon.jpg, 256 x 256): three cases with nothing printed on them, standing on the menu's own floor,
as the shelf shows games that have no cover yet.

    python tools/make_icon.py [icon.jpg]          (needs Pillow)
"""
import sys

from PIL import Image, ImageDraw, ImageFilter

S = 4  # (drawn four times as large, then made smaller: smooth edges)
N = 256 * S
FLOOR = 0.715  # (where the cases stand, as a share of the height)


def grey(v, a=255):
    v = max(0, min(255, round(v)))
    return (v, v, v, a)


def backdrop():
    img = Image.new('RGBA', (N, N))
    px = img.load()
    floor = round(N * FLOOR)
    for y in range(N):
        if y < floor:
            v = 22 + 30 * (y / floor) ** 1.6
        else:
            v = 64 - 52 * ((y - floor) / (N - floor)) ** 0.55
        for x in range(N):
            px[x, y] = grey(v)
    return img


def quad_shaded(layer, corners, top, bottom):
    """A four-cornered face, lighter at its top than at its bottom. corners: top left, top right, bottom right, bottom left."""
    mask = Image.new('L', (N, N), 0)
    ImageDraw.Draw(mask).polygon(corners, fill=255)
    ys = [c[1] for c in corners]
    y0, y1 = min(ys), max(ys)
    shade = Image.new('RGBA', (N, N))
    d = ImageDraw.Draw(shade)
    for y in range(int(y0), int(y1) + 1):
        t = (y - y0) / max(1, y1 - y0)
        d.line([(0, y), (N, y)], fill=grey(top + (bottom - top) * t))
    layer.paste(shade, (0, 0), mask)


def case(layer, x0, x1, top0, top1, bottom0, bottom1, light, spine, selected=False):
    """One case: its face between x0 and x1 (shares of the width), each side's top and bottom given (shares of the height), and a
    spine on the side named ('left', 'right' or None)."""
    p = lambda x, y: (x * N, y * N)  # noqa: E731
    face = [p(x0, top0), p(x1, top1), p(x1, bottom1), p(x0, bottom0)]
    quad_shaded(layer, face, light, light - 42)
    d = ImageDraw.Draw(layer)
    if spine:
        w = 0.022
        if spine == 'left':
            side = [p(x0 - w, top0 + 0.004), p(x0, top0), p(x0, bottom0), p(x0 - w, bottom0 - 0.004)]
        else:
            side = [p(x1, top1), p(x1 + w, top1 + 0.004), p(x1 + w, bottom1 - 0.004), p(x1, bottom1)]
        quad_shaded(layer, side, light - 62, light - 86)
    edge = grey(250) if selected else grey(light + 34)
    d.line(face + [face[0]], fill=edge, width=round((2.6 if selected else 1.3) * S), joint='curve')
    if selected:
        # (the fold of the sleeve, a little in from the spine's side)
        fx = x0 + (x1 - x0) * 0.055
        d.line([p(fx, top0 + 0.012), p(fx, bottom0 - 0.012)], fill=grey(light - 26), width=round(1.0 * S))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'icon.jpg'
    cases = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    # the two at the sides, turned a little towards the middle: the outer side is the nearer, so the taller
    case(cases, 0.085, 0.300, 0.300, 0.318, FLOOR, FLOOR - 0.022, 150, 'left')
    case(cases, 0.700, 0.915, 0.318, 0.300, FLOOR - 0.022, FLOOR, 150, 'right')
    # the one in the middle, square on
    case(cases, 0.352, 0.648, 0.226, 0.226, FLOOR + 0.004, FLOOR + 0.004, 214, None, selected=True)

    img = backdrop()
    # the floor shows the cases again, upside down and fading
    floor = round(N * (FLOOR + 0.004))
    mirror = cases.transpose(Image.FLIP_TOP_BOTTOM)
    shown = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    shown.paste(mirror, (0, 2 * floor - N))
    fade = Image.new('L', (N, N), 0)
    fd = ImageDraw.Draw(fade)
    reach = N * 0.20
    for y in range(floor, N):
        t = (y - floor) / reach
        if t >= 1:
            break
        fd.line([(0, y), (N, y)], fill=round(70 * (1 - t) ** 1.5))
    shown.putalpha(Image.composite(fade, Image.new('L', (N, N), 0), shown.getchannel('A')))
    img.alpha_composite(shown.filter(ImageFilter.GaussianBlur(0.6 * S)))
    img.alpha_composite(cases)
    img.convert('RGB').resize((256, 256), Image.LANCZOS).save(out, quality=95)
    print('written:', out)


if __name__ == '__main__':
    main()
