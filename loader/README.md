# nx2-flow: the game loader

`nx2-game.nro`: one homebrew program that runs a Switch 2 game straight from its **NUP file** on the card. No program is built
for each game, and no image of the game's files is made: the loader reads the game from the NUP file as it starts.

It is started by nx2-flow's menu (`../menu`), which gives it the NUP file to run; `../docs/layout.md` says what is where on the
card and how the two programs talk.

> **Experimental. Use it at your own risk**, and with dumps of games you own only. No game, key or console file is part of this.

## What it does when it starts

1. Reads the NUP file's table of contents and takes the packages that make the game: the game's own and its updates, the later
   over the earlier. Added content isn't taken yet.
2. Unpacks the modules of the newest program (LZ4 or ZBIC) into memory and has the console's loader service map them, code and
   data together as on the console. That service maps seven images for a program besides its own: when a game has more
   modules than that, or loads modules of its own as it runs, the ones that needn't be whole are made into one image first
   (`runtime/merge.c`), their code rewritten in memory as nx2-relink rewrites it on a computer.
3. Makes the header and tables of an image of the game's files in memory, from the files of every package and then of the
   game's `mods/` folder (or the set `game.cfg` names). The files' own bytes stay in the NUP file: a read of the image is
   answered from there. A file kept packed in the NUP file is unpacked whole the first time it is read.
4. Reads the game's own table of heaps, where it has one, to know what to tell it of memory.
5. Starts the game, with its saved data, log and crash report in the game's folder (`/nx2-flow/<title id>/`).

## Limits

- **Seven images besides the program's own** is what the loader service maps. The main module, the SDK's and any module with
  thread-local storage of its own have to be whole, an image each; the others share one. A game with more than six that have
  to be whole, or one that has more modules of its own loaded at a time than the places left, needs a port made for it with
  nx2-relink. A module that was merged is less surely right than one kept whole: `modules=whole` in `game.cfg` merges none.
- **The host game matters**, as for a port: the memory for graphics, the system resource and the services a program may use are
  those of the game the homebrew menu was started over. A service a game can run without is stood in for; the log says so.
- Tried with kaiHEN 20261117.1 only.

## Building

Needs devkitPro (devkitA64) and ounce-devkit (libounce), and Python 3.10 or newer.

```bash
python tools/build_loader.py          # out/nx2-game.nro
```

## Trying it without the menu

Started with no file named, the loader runs the NUP file that `/nx2-flow/next.txt` names (one line, a path of the card:
`sdmc:/nx2-flow/games/Some Game.nup`).

## License

MIT (see `LICENSE`). `third_party/zstd` is Zstandard's decoder, under its own license (BSD), in that folder.
