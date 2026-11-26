#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Makes a pretend card for the previews (preview/): a few tiny NUP containers of games that don't exist, with made-up
names, icons and covers, laid out as docs/layout.md says. Nothing of a real game is in it.

    python tools/make_fake_card.py <folder>          (needs Pillow)

A container made here has what the menu reads of one and nothing else: the superblock, a table of contents with its packages
(no files), and each package's block of facts.
"""
import hashlib
import io
import os
import random
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

# (title, publisher, version, updates, added contents, a cover: None, 'front' or 'full')
GAMES = [
    ('Lantern Harbor', 'Driftwood Works', '1.2.0', 1, 0, 'full'),
    ('Paper Comet', 'Studio Marmalade', '1.0.0', 0, 0, 'front'),
    ('Mossy Circuit', 'Hollow Pine Games', '2.1.3', 2, 1, 'front'),
    ('Tin Robots Parade', 'Brass Button', '1.0.5', 1, 0, None),
    ('Quiet Tides', 'Driftwood Works', '1.1.0', 1, 2, 'full'),
    ('Orchard of Clocks', 'Nine Lamps', '1.0.0', 0, 0, None),
    ('Café Nébula', 'Studio Marmalade', '3.0.1', 1, 0, 'front'),
]
FACT_NPDM, FACT_ICON, FACT_VERSION, FACT_TITLE, FACT_PUBLISHER = 1, 3, 4, 5, 6  # (numbered from 1, as one kind of container has them)


def picture(seed, size, title=None, wrap=False):
    """An abstract picture: bands and discs in a few colours that go together."""
    rng = random.Random(seed)
    w, h = size
    hue = rng.random()

    def colour(shift, light):
        import colorsys
        r, g, b = colorsys.hls_to_rgb((hue + shift) % 1.0, light, 0.55)
        return int(r * 255), int(g * 255), int(b * 255)

    img = Image.new('RGB', size, colour(0, 0.25))
    d = ImageDraw.Draw(img)
    for k in range(7):
        y = int(h * k / 7)
        d.rectangle([0, y, w, y + h // 7], fill=colour(0.02 * k, 0.22 + 0.05 * k))
    for _ in range(9):
        r = rng.randint(w // 14, w // 4)
        x, y = rng.randint(0, w), rng.randint(0, h)
        d.ellipse([x - r, y - r, x + r, y + r], fill=colour(rng.choice([0.08, 0.45, 0.55]), rng.uniform(0.45, 0.7)))
    if title:
        face_x = int(w * (1 - 0.475)) if wrap else 0
        face_w = w - face_x
        font = ImageFont.load_default(max(14, face_w // 9))
        d.rectangle([face_x, int(h * 0.70), w, int(h * 0.86)], fill=(245, 245, 240))
        words, lines = title.split(), ['']
        for word in words:
            trial = (lines[-1] + ' ' + word).strip()
            if d.textlength(trial, font=font) > face_w * 0.9 and lines[-1]:
                lines.append(word)
            else:
                lines[-1] = trial
        ty = int(h * 0.78) - len(lines) * font.size // 2 - 2
        for line in lines:
            d.text((face_x + (face_w - d.textlength(line, font=font)) / 2, ty), line, fill=(30, 30, 30), font=font)
            ty += font.size + 2
        if wrap:
            spine_x0, spine_x1 = int(w * 0.475), int(w * (1 - 0.475))
            d.rectangle([spine_x0, 0, spine_x1, h], fill=(232, 232, 232))
            d.rectangle([0, 0, spine_x0, h], fill=colour(0, 0.16))
            small = ImageFont.load_default(max(11, h // 34))
            for k in range(6):
                d.rectangle([int(w * 0.04), int(h * (0.56 + 0.05 * k)), int(w * 0.42), int(h * (0.56 + 0.05 * k)) + 4], fill=colour(0, 0.5))
            d.text((int(w * 0.04), int(h * 0.1)), title, fill=(240, 240, 240), font=small)
    return img


def jpeg(img):
    out = io.BytesIO()
    img.save(out, 'JPEG', quality=88)
    return out.getvalue()


def nup(packages):
    """packages: [(title id, version, kind, {fact number: bytes})]. The container's bytes."""
    n = len(packages)
    pool = b''
    toc_size = 0x30 + 0x40 * n + 0x18 * n  # (a root folder each, no files)
    pool_at = toc_size
    toc_size += len(pool)
    meta_at = 0x80 + toc_size
    rows, blocks = b'', b''
    for k, (title, version, kind, facts) in enumerate(packages):
        block = b''.join(struct.pack('<HHI', number, 0, len(data)) + data for number, data in sorted(facts.items()))
        rows += struct.pack('<QIIIIBB', title, k, 0xFFFFFFFF, 0xFFFFFFFF, version, kind, 0).ljust(0x20, b'\0')
        rows += struct.pack('<QI', meta_at + len(blocks), len(block)).ljust(0x20, b'\0')
        blocks += block
    toc = (b'TOC\x01' + struct.pack('<IQQQQ', n, n, 0, pool_at, len(pool))).ljust(0x30, b'\0') + rows
    toc += b''.join(struct.pack('<IIII', 0, 0, 0, 0).ljust(0x18, b'\0') for _ in range(n)) + pool
    assert len(toc) == toc_size
    head = (b'NUP\x01' + b'\0' * 4 + struct.pack('<QQ', 0x80, toc_size) + hashlib.sha256(toc).digest()
            + struct.pack('<QQI', meta_at + len(blocks), 0, n)).ljust(0x80, b'\0')
    return head + toc + blocks


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    card = sys.argv[1]
    games = os.path.join(card, 'nx2-flow', 'games')
    covers = os.path.join(card, 'nx2-flow', 'covers')
    for d in (games, os.path.join(covers, 'full'), os.path.join(card, 'switch', 'nx2-flow'), os.path.join(card, 'roms', 'more', 'deeper'), os.path.join(card, 'empty')):
        os.makedirs(d, exist_ok=True)
    open(os.path.join(card, 'switch', 'nx2-flow', 'nx2-game.nro'), 'wb').write(b'not a program: a stand-in for the previews\n')
    ids = []
    for k, (title, publisher, version, updates, addons, cover) in enumerate(GAMES):
        title_id = 0x0400F00000001000 + (k << 16)
        facts = {FACT_NPDM: b'META' + b'\0' * 0x7C, FACT_ICON: jpeg(picture(k, (256, 256))), FACT_VERSION: b'1.0.0',
                 FACT_TITLE: title.encode('utf-8'), FACT_PUBLISHER: publisher.encode('utf-8')}
        packages = [(title_id, 0, 0, facts)]
        for u in range(updates):
            packages.append((title_id | 0x800, (u + 1) << 16, 1, {FACT_VERSION: version.encode()} if u == updates - 1 else {}))
        for a in range(addons):
            packages.append((title_id + 0x1001 + a, 0, 2, {}))
        open(os.path.join(games, f'{title} [{title_id:016X}].nup'), 'wb').write(nup(packages))
        code = f'ZZ{k:02d}A'  # (a made-up code, in the shape of the ones GameTDB uses)
        if cover == 'front':
            picture(k, (384, 624), title).save(os.path.join(covers, f'{title_id:016x}.jpg'), quality=88)
        elif cover == 'full':
            picture(k, (808, 624), title, wrap=True).save(os.path.join(covers, 'full', f'{code}.jpg'), quality=88)
            ids.append(f'{title_id:016x}={code}')
    open(os.path.join(covers, 'ids.txt'), 'w', newline='\n').write('\n'.join(ids) + '\n')
    # (the previews walk through the styles from the first of the list, which is not the one the menu starts with)
    open(os.path.join(card, 'nx2-flow', 'menu.cfg'), 'w', newline='\n').write('style=Coverflow\n')
    open(os.path.join(games, 'not a container.nup'), 'wb').write(b'this is not a NUP container' * 8)
    # one game's folder, as after a few runs: two sets of mods, a log, a report
    folder = os.path.join(card, 'nx2-flow', f'{0x0400F00000001000:016x}')
    for d in ('save', 'mods', os.path.join('modsets', 'big textures'), os.path.join('modsets', 'night palette')):
        os.makedirs(os.path.join(folder, d), exist_ok=True)
    for name in ('log.txt', 'crash.txt'):
        open(os.path.join(folder, name), 'w').write('a pretend file, for the previews\n')
    # and its saved data: the default slot and two more, the game set to use one of those
    for slot in ('save', os.path.join('saves', 'second run'), os.path.join('saves', 'before the last door')):
        os.makedirs(os.path.join(folder, slot, 'profile'), exist_ok=True)
        open(os.path.join(folder, slot, 'progress.bin'), 'wb').write(slot.encode() * 4000)
        open(os.path.join(folder, slot, 'profile', 'settings.bin'), 'wb').write(b'pretend' * 100)
    open(os.path.join(folder, 'game.cfg'), 'w', newline='\n').write('save=second run\n')
    print(f'{len(GAMES)} pretend games in {games}')


if __name__ == '__main__':
    main()
