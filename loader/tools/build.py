"""The build: a game's code relinked into a homebrew program (.nro) that runs it.

  python tools/build.py guest                         the test game in guest/ (no game needed)
  python tools/build.py <exefs folder> [--name x]     a game's ExeFS: main, sdk, subsdk0.. (as nxdumptool or hactool extract it)
  python tools/build.py <game.nsp> --name x           the same from an NSP (tools/nsp.py opens it; --keys <prod.keys> if it isn't in ~/.switch)
  python tools/build.py <game.nup | folder> --name x  a Switch 2 game, from its NUP container or a folder of its loose files (base/,
                                                      updates/<version>/, each with exefs and romfs; tools/game2.py reads them): for a
                                                      Switch 2 alone. --update <nup | folder>: an update kept apart, laid over it
      --target switch | switch2 | both               switch: devkitPro's libnx (default); switch2: ounce-devkit's libounce
      --out <folder>                                  where the .nro goes (default: out/)
      --heap <MB>                                     a Switch 2's port: the game's heap (default: 3200, or 7800 for a Switch 2 game)
      --whole  |  --whole=main,sdk  |  --whole=auto   a Switch 2's port: the modules (all, those named, or those that need it: a Switch 2
                                                      game's default) kept whole and mapped at run time by kaiHEN's
                                                      loader service, code and data together as on the console (not needed so far)
      --title, --author, --version, --icon <jpg>      what the homebrew menu shows for the port. By default the game's own,
                                                      which tools/nsp.py leaves beside the ExeFS (info.json, icon.jpg)
"""
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
sys.path.insert(0, f'{ROOT}/tools')
from progress import progress  # noqa: E402
# (the variable may hold a path of msys's, which this Python can't open: the first place that has the compiler's folder)
DKP = next((d for d in (os.environ.get('DEVKITPRO', '').replace('\\', '/'), 'C:/devkitPro', '/opt/devkitpro')
            if d and os.path.isdir(f'{d}/devkitA64')), 'C:/devkitPro')
BIN = f'{DKP}/devkitA64/bin'
GCC = f'{BIN}/aarch64-none-elf-gcc'
ARCH = ['-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE']


def run(cmd, cwd=None):
    env = dict(os.environ, DEVKITPRO=DKP, DEVKITA64=f'{DKP}/devkitA64',
               PATH=os.pathsep.join([BIN, f'{DKP}/tools/bin', os.environ.get('PATH', '')]))
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if r.returncode:
        if 'NOT SUPPORTED YET' in r.stdout + r.stderr:  # (a refusal, not a fault: said as the tool that refused said it)
            sys.exit((r.stdout + r.stderr).rstrip())
        sys.exit(f'FAILED: {" ".join(cmd)}\n{r.stdout}{r.stderr}')
    return r.stdout + r.stderr


def build_guest(work):
    """The test game: two modules, linked and packed as a game's are."""
    out = f'{work}/guest'
    os.makedirs(out, exist_ok=True)
    flags = ['-O2', '-fPIC', '-ffreestanding', '-fno-builtin', '-march=armv8-a', '-fno-asynchronous-unwind-tables']
    link = ['-shared', '-nostdlib', '-nostartfiles', '-T', f'{DKP}/libnx/switch.ld', '-Wl,-z,text', '-Wl,-z,now', '-Wl,--hash-style=sysv',
            '-Wl,--build-id=sha1']
    for name, srcs in (('main', ['main.c', 'crt.s']), ('sdk', ['sdk.c', 'svc.s', 'crt.s'])):
        run([GCC, *flags, *link, '-o', f'{out}/{name}.elf'] + [f'{ROOT}/guest/{s}' for s in srcs])
        run([f'{DKP}/tools/bin/elf2nso', f'{out}/{name}.elf', f'{out}/{name}'])
    return out


def modules_in(exefs):
    """The game's modules in load order. rtld is left out: runtime/loader.c does its work."""
    names = ['main'] + [f'subsdk{k}' for k in range(10)] + ['sdk']
    found = [(n, f'{exefs}/{n}') for n in names if os.path.isfile(f'{exefs}/{n}')]
    if not found or found[0][0] != 'main':
        sys.exit(f'{exefs}: no "main" there. It must be a game\'s ExeFS folder (main, sdk, subsdk0..).')
    return found


def target_flags(target):
    if target == 'switch':
        return ['-D__SWITCH__', f'-I{DKP}/libnx/include'], [f'-specs={DKP}/libnx/switch.specs', f'-L{DKP}/libnx/lib', '-lnx']
    rules = f'{DKP}/libounce/switch2_rules'
    if not os.path.isfile(rules):
        sys.exit('This needs ounce-devkit (libounce), which isn\'t installed:\n'
                 '  https://github.com/ouncebrew/ounce-devkit  (sh install.sh, in devkitPro\'s msys2 shell)')
    return (['-D__SWITCH__', '-D__SWITCH2__', f'-I{DKP}/libounce/include', f'-I{DKP}/portlibs/switch2/include'],
            [f'-specs={DKP}/libounce/switch.specs', f'-L{DKP}/portlibs/switch2/lib', f'-L{DKP}/libounce/lib', '-lEGL', '-lGLESv2', '-lglapi', '-ldrm_nouveau', '-lnx'])


from edition import VERSION  # noqa: E402  (the tool's own: in a port's crash report)
EXTRA = []  # more for the compiler, from the options
WHOLE = []  # (--whole: the modules kept whole and mapped at run time, code and data together as on the console)


def build(exefs, name, target, work, out_dir, shown=None):
    beside = f'{os.path.dirname(exefs.rstrip("/"))}/info.json'
    info = json.load(open(beside, encoding='utf-8')) if os.path.isfile(beside) else {}
    extra = list(EXTRA)
    if info.get('switch2'):
        # A Switch 2 game (tools/nup.py port wrote this): its own SDK drives the display and the graphics processor, and its heaps
        # are larger than a Switch game's. NX2_GAME_MB says both to the runtime.
        if target != 'switch2':
            print(f'{name}: its port is for its own console alone, not for a Switch')
            return None
        if not any(x.startswith('-DNX2_GAME_MB=') for x in extra):
            # (With no figure given for it, a game that keeps nothing apart is told of what the port really has less 400 MB,
            # 8192 at most: some take all they are told of in one piece and then map a little more as they run, and some size
            # an allocator of their own by the figure and end themselves when it is too small. Told of 7800, as every such game
            # was while a port kept more for itself, two ended so and ran with a larger figure; reports, 2026-11-25.)
            if int(info.get('heap') or 7800) == 7800 and not info.get('apart'):
                extra += ['-DNX2_GAME_MB=8192', '-DNX2_GAME_ROOM_MB=400']
            else:
                extra.append(f'-DNX2_GAME_MB={int(info.get("heap") or 7800)}')
        if info.get('apart') and not any(x.startswith('-DNX2_GAME_APART_MB=') for x in extra):
            extra.append(f'-DNX2_GAME_APART_MB={int(info["apart"])}')  # (address space the game keeps apart from its heap: tools/heapdef.py)
        # (the memory for graphics the game's own description asks for, main.npdm: the runtime says so when the title a port is
        # started over has less, runtime/main.c)
        described = f'{exefs.rstrip("/")}/main.npdm'
        if os.path.isfile(described):
            head = open(described, 'rb').read(0x18)
            remote = int.from_bytes(head[0x10:0x14], 'little') >> 20 if head[:4] == b'META' and len(head) >= 0x14 else 0
            system = int.from_bytes(head[0x14:0x18], 'little') >> 10 if head[:4] == b'META' and len(head) >= 0x18 else 0
            if 0 < remote <= 4096:
                extra.append(f'-DNX2_GAME_REMOTE_MB={remote}')
            if head[:4] == b'META' and system <= 4096 << 10:
                extra.append(f'-DNX2_GAME_SYSRES_KB={system}')
            if 0 < remote <= 4096:
                print(f'{name}: made for {remote} MB of memory for graphics and {system >> 10} MB of system resource. A port has those from '
                      'the game the homebrew menu is started over: the game itself is the surest choice (the log says what it had)')
        changed = False
        for x in extra:  # (given on the command line: kept with the port, for the next time it is built)
            if x.startswith('-DNX2_GAME_MB=') and info.get('heap') != int(x[14:]):
                info['heap'], changed = int(x[14:]), True
        if WHOLE and info.get('whole') != WHOLE[0]:
            info['whole'], changed = WHOLE[0], True
        if changed:
            json.dump(info, open(beside, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    gen = f'{work}/gen'
    os.makedirs(gen, exist_ok=True)
    for f in glob.glob(f'{gen}/*'):
        os.remove(f)
    mods = modules_in(exefs)
    progress('relink', target, 0, 1)
    # (a Switch 2 game's modules: those that need it are kept whole, unless it was said which; tools/relink.py, has_own_tls)
    whole = WHOLE[:1] or ([info['whole']] if info.get('whole') else ['--whole=auto'] if info.get('switch2') else [])
    print(run([sys.executable, f'{ROOT}/tools/relink.py', gen] + whole + [f'{n}={p}' for n, p in mods]), end='')
    cflags, ldflags = target_flags(target)
    obj = f'{work}/obj_{target}'
    os.makedirs(obj, exist_ok=True)
    objs = []
    sources = sorted(glob.glob(f'{ROOT}/runtime/*.c') + glob.glob(f'{ROOT}/runtime/*.s')) + [f'{gen}/modules.c'] + [f'{gen}/{n}.s' for n, _ in mods]
    progress('relink', target, 1, 1)
    for k, s in enumerate(sources):
        progress('compile', target, k, len(sources))
        s = s.replace('\\', '/')
        o = f'{obj}/{os.path.basename(s)}.o'
        # (the modules' .s files name their .bin files by name alone: assembled from the folder they are in)
        run([GCC, '-c', '-O2', '-g', '-Wall', '-ffunction-sections', *ARCH, *cflags, *extra, f'-DNX2_NAME="{name}"', f'-DNX2_VERSION="{VERSION}"', f'-I{ROOT}/runtime', s, '-o', o], cwd=gen)
        objs.append(o)
    progress('compile', target, len(sources), len(sources))
    progress('link', target, 0, 1)
    elf = f'{work}/{name}_{target}.elf'
    run([GCC, *ARCH, '-g', '-o', elf, *objs, *ldflags, '-Wl,--wrap=__libnx_exception_entry', '-Wl,--wrap=svcBreak,--wrap=svcExitProcess,--wrap=svcReturnFromException,--wrap=abort,--wrap=svcExitThread', f'-Wl,-Map,{elf[:-4]}.map'])
    os.makedirs(out_dir, exist_ok=True)
    nro = f'{out_dir}/{name}{"-switch2" if target == "switch2" else ""}.nro'
    # what the homebrew menu shows: the game's own name, version and icon (tools/nsp.py reads them from its control data)
    shown = dict(shown or {})
    if info:
        for k in ('title', 'author', 'version'):
            shown.setdefault(k, info.get(k) or None)
        if info.get('icon'):
            shown.setdefault('icon', f'{os.path.dirname(beside)}/{info["icon"]}')
    tool = 'nx2-relink' if target == 'switch2' else 'nx-relink'  # (the folder of the card its files are in has the same name)
    title = shown.get('title') or name
    author = f'{shown["author"]} ({tool} port)' if shown.get('author') else tool
    nacp = f'{work}/{name}.nacp'
    run([f'{DKP}/tools/bin/nacptool', '--create', title, author, shown.get('version') or '0.1', nacp])
    icon = shown.get('icon')
    run([f'{DKP}/tools/bin/elf2nro', elf, nro, f'--nacp={nacp}'] + ([f'--icon={icon}'] if icon and os.path.isfile(icon) else []))
    progress('link', target, 1, 1)
    print(f'{nro}: {os.path.getsize(nro) >> 10} KB ({target}); "{title}" {shown.get("version") or ""}{", with its icon" if icon else ""}; '
          f'symbols in {elf}')
    return nro


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)

    def opt(flag, default):
        if flag in args:
            i = args.index(flag)
            v = args[i + 1]
            del args[i:i + 2]
            return v
        return default
    target = opt('--target', 'switch')
    out_dir = opt('--out', f'{ROOT}/out').replace('\\', '/')
    name = opt('--name', None)
    work = opt('--work', f'{ROOT}/build').replace('\\', '/')
    keys_path = opt('--keys', None)
    for a in [a for a in args if a == '--whole' or a.startswith('--whole=')]:
        args.remove(a)
        WHOLE.append(a)
    updates = []
    while '--update' in args:
        updates.append(opt('--update', None))
    heap = opt('--heap', None)  # (MB for the game's heap on a Switch 2, where 3200 is given: a Switch 2 game's own needs more)
    if heap:
        EXTRA.append(f'-DNX2_GAME_MB={int(heap)}')
    shown = {k: v for k, v in (('title', opt('--title', None)), ('author', opt('--author', None)), ('version', opt('--version', None)),
                               ('icon', opt('--icon', None))) if v}
    src = args[0].replace('\\', '/')
    if src == 'guest':
        src = build_guest(work)
        name = name or 'guest'
    elif src.lower().endswith('.nsp'):
        name = name or 'game'
        dump = f'{work}/{name}_nsp'
        print(run([sys.executable, f'{ROOT}/tools/nsp.py', src, dump] + (['--keys', keys_path] if keys_path else [])), end='')
        src = f'{dump}/exefs'
    elif src.lower().endswith('.nup') or (os.path.isdir(src) and not os.path.isfile(f'{src}/main')):
        # a Switch 2 game's dump: a container, or a folder that isn't an ExeFS itself
        name = name or 'game'
        dump = f'{work}/{name}_nsp'  # (the same folder a Switch game's dump has: tools/install.py and nxrelink.py look there)
        print(run([sys.executable, f'{ROOT}/tools/game2.py', 'port', src, dump] + [x for u in updates for x in ('--update', u)]), end='')
        src = f'{dump}/exefs'
    name = name or os.path.basename(src.rstrip('/')) or 'game'
    for t in (['switch', 'switch2'] if target == 'both' else [target]):
        build(src, name, t, work, out_dir, shown)


if __name__ == '__main__':
    main()
