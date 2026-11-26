"""How far a long step has got, said by the tools as they work.

A tool calls progress(kind, detail, done, total). Run by nxrelink.py (which sets NX2_PROGRESS) it writes a line for nxrelink.py to read:
    @@progress|<kind>|<detail>|<done>|<total>
and nxrelink.py shows it as a percentage on a terminal, or hands it to the window, which draws a bar. Run by hand on a terminal, the tool
shows the percentage itself. kind is one of NAMES' keys; done and total are bytes, or a count where NAMES says so.
"""
import os
import sys
import time

NAMES = {'extract': 'extracting the game\'s files', 'aoc': 'extracting add-on content', 'relink': 'relinking the game\'s code',
         'compile': 'compiling', 'link': 'linking', 'copy': 'copying', 'verify': 'reading back', 'shaders': 'translating shaders'}
COUNTED = ('compile', 'relink', 'link')  # (a count of things, not bytes)
_last = {}


def progress(kind, detail, done, total):
    now = time.time()
    percent = 100 if total <= 0 else int(done * 100 / total)
    last = _last.get((kind, detail))
    if done < total and last and (now - last[0] < 0.15 or percent == last[1]):
        return
    _last[(kind, detail)] = (now, percent)
    if os.environ.get('NX2_PROGRESS'):
        print(f'@@progress|{kind}|{detail}|{done}|{total}', flush=True)
    elif sys.stdout.isatty():
        show(kind, detail, done, total)


def show(kind, detail, done, total):
    """A percentage on a terminal, on one line that is written over until the step is done."""
    percent = 100 if total <= 0 else int(done * 100 / total)
    amount = f'{done}/{total}' if kind in COUNTED else f'{done / (1 << 20):.0f} of {total / (1 << 20):.0f} MB'
    line = f'  {NAMES.get(kind, kind)}{" " + detail if detail else ""}: {percent:3d}%  ({amount})'
    sys.stdout.write('\r' + line[:110].ljust(110) + ('\n' if done >= total else ''))
    sys.stdout.flush()
