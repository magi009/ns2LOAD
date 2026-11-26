"""The game loader of nx2-flow: the runtime built with no game in it, as one program that reads a game from its NUP file when it is
started (runtime/game.c).

  python tools/build_loader.py [--out <folder>]      out/nx2-game.nro, for a Switch 2
"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
sys.path.insert(0, f'{ROOT}/tools')
import build  # noqa: E402

VERSION = '0.1.3'  # nx2-flow's game loader's own (in its crash reports)

# what a port is built with, found by the loader as it starts: variables of the runtime's, in place of numbers
FOUND = ['-DNX2_LOADER', '-DNX2_GAME_MB=nx2_game_mb', '-DNX2_GAME_ROOM_MB=nx2_game_room_mb', '-DNX2_GAME_APART_MB=nx2_game_apart_mb', '-DNX2_GAME_REMOTE_MB=nx2_game_remote_mb',
         '-DNX2_GAME_SYSRES_KB=nx2_game_sysres_kb']


def main():
    args = sys.argv[1:]
    out_dir = args[args.index('--out') + 1] if '--out' in args else f'{ROOT}/out'
    work = f'{ROOT}/build'
    cflags, ldflags = build.target_flags('switch2')
    obj = f'{work}/obj_loader'
    os.makedirs(obj, exist_ok=True)
    objs = []
    zstd = f'{ROOT}/third_party/zstd'
    # (Zstandard's decoder, as its makers wrote it: built with nothing of its own switched on but what a decoder needs)
    for s in sorted(glob.glob(f'{zstd}/common/*.c') + glob.glob(f'{zstd}/decompress/*.c')):
        s = s.replace('\\', '/')
        o = f'{obj}/zstd_{os.path.basename(s)}.o'
        build.run([build.GCC, '-c', '-O2', '-ffunction-sections', *build.ARCH, *cflags, '-DZSTD_DISABLE_ASM', '-DZSTD_LEGACY_SUPPORT=0', '-DDEBUGLEVEL=0',
                   '-DZSTD_NO_TRACE', f'-I{zstd}', s, '-o', o])
        objs.append(o)
    for s in sorted(glob.glob(f'{ROOT}/runtime/*.c') + glob.glob(f'{ROOT}/runtime/*.s')):
        s = s.replace('\\', '/')
        o = f'{obj}/{os.path.basename(s)}.o'
        said = build.run([build.GCC, '-c', '-O2', '-g', '-Wall', '-ffunction-sections', *build.ARCH, *cflags, *FOUND, f'-DNX2_VERSION="{VERSION}"', f'-I{ROOT}/runtime',
                          f'-I{zstd}', s, '-o', o])
        if said.strip():
            print(said.rstrip())
        objs.append(o)
    elf = f'{work}/nx2-game.elf'
    build.run([build.GCC, *build.ARCH, '-g', '-o', elf, *objs, *ldflags, '-Wl,--wrap=__libnx_exception_entry',
               '-Wl,--wrap=svcBreak,--wrap=svcExitProcess,--wrap=svcReturnFromException,--wrap=abort,--wrap=svcExitThread', f'-Wl,-Map,{work}/nx2-game.map'])
    os.makedirs(out_dir, exist_ok=True)
    nro, nacp = f'{out_dir}/nx2-game.nro', f'{work}/nx2-game.nacp'
    build.run([f'{build.DKP}/tools/bin/nacptool', '--create', 'nx2-flow game loader', 'nx2-flow', VERSION, nacp])
    build.run([f'{build.DKP}/tools/bin/elf2nro', elf, nro, f'--nacp={nacp}'])
    print(f'{nro}: {os.path.getsize(nro) >> 10} KB; symbols in {elf}')


if __name__ == '__main__':
    main()
