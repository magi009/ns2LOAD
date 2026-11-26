# What is where, and how the programs talk

Two homebrew programs for a Switch 2, both in one folder of the card so that the homebrew menu shows one entry:

```
/switch/nx2-flow/nx2-flow.nro      the menu: lists the games, starts one, receives files from a computer
/switch/nx2-flow/nx2-game.nro      the game loader: runs the game of one NUP file. Started by the menu, not by hand
```

## On the card

```
/nx2-flow/menu.cfg                 the menu's settings, lines of key=value
/nx2-flow/games/                   the NUP files (the default; menu.cfg's "games" names another folder)
/nx2-flow/covers/<ID>.jpg|png      a game's front cover;  covers/full/<ID>.jpg|png  the whole wrap
/nx2-flow/covers/ids.txt           lines of <title id>=<GameTDB code>: which code a game's cover is filed under
/nx2-flow/cache/                   the menu's own: what it read of each NUP file (library.txt) and the games' icons
/nx2-flow/transfer.log             the menu's own: the figures of every large file received from a computer
/nx2-flow/<title id>/              one game's folder, named by its title id (16 hex digits, lower case):
    save/                            its saved data: the slot named "default"
    saves/<slot>/                    its saved data, other slots: one folder each, named by the user
    log.txt  log.prev.txt  crash.txt the loader's log of the last run, of the one before, and a run that ended badly
    mods/                            files laid over the game's own (the default set)
    modsets/<name>/                  other sets of such files, one folder each
    game.cfg                         the game's settings, lines of key=value
    debug                            an empty file: the detailed log is on
```

`<ID>` is the title id, or the code GameTDB names the game's cover by.

## menu.cfg

| key | meaning |
|---|---|
| `games` | the folder the NUP files are in (`/nx2-flow/games` when absent) |
| `style` | how the games stand on the shelf, by name (`Showroom` when absent) |
| `covers` | `hidden`: every game shows its own icon; anything else: covers are shown |
| `cases` | `off`: covers are flat pictures; anything else: they stand as cases |
| `theme` | `light`, or dark |
| `transfer_kbps` | the most the menu's file receiver takes from the network, in KB a second; 0 or absent: no limit |
| `transfer_block_kb` | the receiver's one write to the card, in KB (64 to 4096; 1024 when absent) |
| `transfer_queue` | how many such blocks may wait for the card before the network is held back (2 to 64; 8 when absent) |
| `transfer_rcvbuf_kb` | the receiving socket's own buffer, in KB (64 to 2048; 256 when absent) |

The loader reads none of these: they are the menu's.

## game.cfg

Written by the menu, read by the loader. A key that is absent or empty means the default.

| key | meaning | default |
|---|---|---|
| `modset` | the folder under `modsets/` to lay over the game | `mods/` |
| `save` | the slot of saved data to use: the name of a folder under `saves/` | `save/` (the slot "default") |
| `memory` | MB the game is told of (2048 to 8992). **Advanced** | decided by the loader |
| `room` | MB the loader keeps for itself (128 to 2048). **Advanced** | 384 |
| `modules` | `whole`: no module is merged with another; `merge`: all that may be are. **Advanced**, by hand: the menu has no row for it | merged only when the game needs the places |

The advanced ones are shown behind a warning: a wrong value keeps a game from starting. Removing the key puts it right again.
The detailed log is not a key: it is the file `debug` in the game's folder.

A slot's name, as the menu makes them: letters, digits, spaces, `-`, `_` and `.`, at most 32 characters, never `default` (that
is `save/`). A `save` that names a folder that isn't there is the loader's to make.

## Starting a game

The menu asks the homebrew loader to start `nx2-game.nro` next (`envSetNextLoad`) and exits. The arguments:

```
argv[0]   sdmc:/switch/nx2-flow/nx2-game.nro
argv[1]   the NUP file, as a path of the card: sdmc:/nx2-flow/games/Some Game.nup
argv[2..] menu=sdmc:/switch/nx2-flow/nx2-flow.nro      (optional: what to start again when the game is closed)
          room=<MB>                                    (optional: game.cfg's "room", which the loader must know before it can read files)
```

Everything else the loader needs it reads from the NUP file and from the game's folder.

## Receiving files from a computer

The menu has a screen that does what Toronja Transfer does (toronja's `tools/transfer-nx`): it listens on TCP port 28302 and speaks
the same protocol ("TTX1": PING, LIST, PUT, GET, DELETE, HASH, MKDIR), so the same program on the computer sends to either. It
listens while that screen is open, and looks for games again when it is closed.

Nothing is different on the wire. What is different is how a file is received (`menu/source/transfer.h`): the network fills
blocks of one size (`transfer_block_kb`), a second thread writes each to the card with a single write, into a file given its whole
size first, and at most `transfer_queue` blocks wait between the two: when the card falls behind, the menu stops reading the
socket, whose buffer is small (`transfer_rcvbuf_kb`), so the sender is held back by the connection itself. `transfer_kbps` caps the
pace from the first byte. Every file of 8 MB or more leaves three lines in `/nx2-flow/transfer.log`: seconds, MB a second, the
longest time with nothing written, how full the queue got, how long the writes took, and where the time went.
