#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetches cover art on a computer, for a console that is kept off the internet.

GameTDB files a game's art under a short code of its own (five letters and digits for this family of consoles), and a NUP file
has the game's title id, not that code. So the two have to be tied first, one line a game, in covers/ids.txt:

    <title id>=<GameTDB code>

This script reads the NUP files of a folder (their first kilobytes: nothing is read whole), ties what it can, downloads the whole
wrap when GameTDB has one and the front otherwise, and writes everything as the menu looks for it:

    python fetch_covers.py <folder of NUP files> --card E:\\             into E:\\nx2-flow\\covers (the card in this computer)
    python fetch_covers.py <folder of NUP files> --out covers           into a folder, to copy to /nx2-flow/covers yourself

How a game gets its code, in this order:
    --id 0400f00000001000=ABCDE     given here, as many times as needed
    an ids.txt already in the covers folder
    --match-titles                  looked up by the game's exact title in GameTDB's own list of games, which this downloads.
                                    Off unless asked for: two games can share a name, so look at what it prints.

A game with no code is listed and left alone: nothing is guessed.
"""
import argparse
import io
import os
import re
import struct
import sys
import urllib.error
import urllib.request
import zipfile

ART = 'https://art.gametdb.com/switch'
LIST = 'https://www.gametdb.com/switchtdb.zip?LANG=EN'
AGENT = {'User-Agent': 'Mozilla/5.0 (covers for a homebrew menu)'}
FACTS = ['NPDM', 'NACP', 'icon', 'display version', 'title', 'publisher']


def read_nup(path):
    """(title id, title) of a container, or None: its superblock, its packages and their facts are read, nothing else."""
    with open(path, 'rb') as f:
        head = f.read(0x80)
        if head[:4] != b'NUP\x01':
            return None
        toc_at, _ = struct.unpack_from('<QQ', head, 8)
        count = struct.unpack_from('<I', head, 0x48)[0]
        if not 0 < count <= 64:
            return None
        f.seek(toc_at)
        toc = f.read(0x30 + 0x40 * count)
        if toc[:4] != b'TOC\x01':
            return None
        packages, first_fact = [], None
        for k in range(count):
            at = 0x30 + 0x40 * k
            title, version, kind = struct.unpack_from('<Q', toc, at)[0], struct.unpack_from('<I', toc, at + 0x14)[0], toc[at + 0x18]
            meta_at, meta_size = struct.unpack_from('<QI', toc, at + 0x20)
            meta = {}
            if 0 < meta_size <= 8 << 20:
                f.seek(meta_at)
                block, p = f.read(meta_size), 0
                while p + 8 <= len(block):
                    number, _, size = struct.unpack_from('<HHI', block, p)
                    meta[number] = block[p + 8:p + 8 + size]
                    if first_fact is None and meta[number][:4] == b'META':
                        first_fact = number
                    p += 8 + size
            packages.append((kind, version, title, meta))
    first_fact = first_fact or 0
    game = next((p for p in packages if p[0] == 0), None)
    updates = sorted((p for p in packages if p[0] == 1), key=lambda p: p[1])
    if not game and not updates:
        return None
    title_id = game[2] if game else updates[-1][2] & ~0xFFF
    title = ''
    for p in ([updates[-1]] if updates else []) + ([game] if game else []):
        text = p[3].get(first_fact + FACTS.index('title'), b'').split(b'\0')[0].decode('utf-8', 'replace')
        nacp = p[3].get(first_fact + FACTS.index('NACP'), b'')
        if not text and len(nacp) >= 0x3000:
            text = next((nacp[k * 0x300:k * 0x300 + 0x200].split(b'\0')[0].decode('utf-8', 'replace') for k in range(16)
                         if nacp[k * 0x300]), '')
        title = title or text
    return f'{title_id:016x}', title or os.path.basename(path)[:-4]


def get(url):
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=40) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code != 404:
            print(f'    {url}: the site answered {e.code}')
        return None
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        print(f'    {url}: {e}')
        return None


def plain(title):
    return re.sub(r'[^a-z0-9]+', ' ', title.lower()).strip()


def gametdb_titles():
    """{a title, plainly: [codes]} from GameTDB's own list of this console family's games."""
    data = get(LIST)
    if not data:
        sys.exit('GameTDB\'s list of games couldn\'t be downloaded: give the codes with --id, or in ids.txt')
    try:
        xml = zipfile.ZipFile(io.BytesIO(data)).read('switchtdb.xml').decode('utf-8', 'replace')
    except (zipfile.BadZipFile, KeyError):
        sys.exit('what GameTDB sent isn\'t the list this reads (a zip with switchtdb.xml in it)')
    out = {}
    for game in re.findall(r'<game\b.*?</game>', xml, re.S):
        code = re.search(r'<id>([A-Za-z0-9]+)</id>', game)
        if not code:
            continue
        for title in re.findall(r'<title>([^<]+)</title>', game):
            out.setdefault(plain(title.replace('&amp;', '&')), []).append(code.group(1))
    return out


def regions_for(code):
    last = code[-1:]
    if last == 'J':
        return ['JA', 'US', 'EN']
    if last in 'PXYZDFSI':
        return ['EN', 'US', 'JA']
    if last == 'K':
        return ['KO', 'US', 'EN']
    return ['US', 'EN', 'JA']


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folder', help='the folder the NUP files are in')
    ap.add_argument('--card', help='the card\'s root (its drive): the covers go to <card>/nx2-flow/covers')
    ap.add_argument('--out', help='a folder for the covers instead, laid out as /nx2-flow/covers is')
    ap.add_argument('--id', action='append', default=[], metavar='TITLEID=CODE', help='a game\'s GameTDB code, given by hand')
    ap.add_argument('--match-titles', action='store_true', help='look codes up by exact title in GameTDB\'s list of games')
    args = ap.parse_args()
    if not args.card and not args.out:
        ap.error('say where the covers go: --card <the card\'s root> or --out <a folder>')
    covers = os.path.join(args.card, 'nx2-flow', 'covers') if args.card else args.out
    os.makedirs(os.path.join(covers, 'full'), exist_ok=True)

    ids_path = os.path.join(covers, 'ids.txt')
    codes = {}
    if os.path.isfile(ids_path):
        for line in open(ids_path, encoding='utf-8', errors='replace'):
            if '=' in line and not line.startswith('#'):
                key, value = line.strip().split('=', 1)
                codes[key.strip().lower()] = value.strip()
    for pair in args.id:
        if '=' not in pair:
            ap.error(f'--id {pair}: write it as <title id>=<code>')
        key, value = pair.split('=', 1)
        codes[key.strip().lower()] = value.strip()

    games = []
    for name in sorted(os.listdir(args.folder)):
        if name.lower().endswith('.nup'):
            read = read_nup(os.path.join(args.folder, name))
            if read:
                games.append(read)
            else:
                print(f'{name}: not a NUP container this reads')
    if not games:
        sys.exit(f'no NUP files in {args.folder}')

    by_title = gametdb_titles() if args.match_titles and any(t not in codes for t, _ in games) else {}
    fetched = missing = untied = 0
    for title_id, title in games:
        code = codes.get(title_id)
        if not code and by_title:
            found = sorted(set(by_title.get(plain(title), [])))
            if len(found) == 1:
                code = codes[title_id] = found[0]
                print(f'{title}: {code}, by its title in GameTDB\'s list')
            elif found:
                print(f'{title}: {len(found)} games of that name in GameTDB\'s list ({", ".join(found)}): give the right one with --id {title_id}=CODE')
        if not code:
            untied += 1
            print(f'{title} ({title_id}): no GameTDB code known: left alone')
            continue
        if any(os.path.isfile(os.path.join(covers, sub, code + ext)) for sub in ('full', '') for ext in ('.jpg', '.png')):
            print(f'{title}: {code}, already there')
            continue
        got = False
        for folder, sub in (('coverfull', 'full'), ('cover', '')):
            for region in regions_for(code):
                data = get(f'{ART}/{folder}/{region}/{code}.jpg')
                if data and len(data) > 2000:
                    open(os.path.join(covers, sub, code + '.jpg'), 'wb').write(data)
                    print(f'{title}: {code}, {"the whole wrap" if sub else "the front"} ({region})')
                    got = True
                    break
            if got:
                break
        if got:
            fetched += 1
        else:
            missing += 1
            print(f'{title}: {code}, GameTDB has no cover under that code')
    with open(ids_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join(f'{key}={value}\n' for key, value in sorted(codes.items())))
    print(f'\n{fetched} downloaded, {missing} not on GameTDB, {untied} with no code; in {covers}')
    if args.out:
        print('Copy that folder\'s contents to /nx2-flow/covers on the card.')


if __name__ == '__main__':
    main()
