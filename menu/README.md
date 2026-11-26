# nx2-flow, the menu

A shelf of the games found as NUP files on the card: pick one, and the game loader (`nx2-game.nro`) is started with it.
What is where on the card, and how the programs talk, is in [`../docs/layout.md`](../docs/layout.md).

The interface is toronja's (its software canvas, its shelf and cases, its covers), with everything of the emulator left
out. It is under the **GNU General Public License, version 2 or later**, as toronja's is: see [`LICENSE`](LICENSE).

## What it does

- **Lists the games**: every `*.nup` in the games folder (`/nx2-flow/games`, or the folder chosen in Settings). Of each file it
  reads only the superblock, the packages and their facts (title id, title, publisher, version, icon): a few kilobytes,
  whatever the file's size. What it read is kept in `/nx2-flow/cache/`, by the file's name, size and time, so a library opens
  without reading the files again.
- **Shows them as cases on a shelf**, in nine ways (`L` / `R`): a game's cover from `/nx2-flow/covers` when there is one, and
  otherwise a case made of the icon the NUP file has and the game's name.
- **A game's options** (`Y`):
  - the detailed log on or off (the empty file `debug` in the game's folder);
  - which set of mods is laid over the game (`mods/`, or a folder of `modsets/`; `game.cfg`'s `modset`);
  - its **slots of saved data**: `default` is `save/`, the others are folders of `saves/`. The list says which is in use; a slot
    is picked (`game.cfg`'s `save`), an empty one made, the one in use copied into a new one, one deleted after a yes. A new
    slot's name is typed on the console's keyboard; when no name comes back, the menu offers `slot 2`, `slot 3`... The default
    slot isn't deleted from here. The slot in use shows under the game's name on the shelf and on the back of its case;
  - **Advanced**, behind a warning that has to be accepted each time: the memory the game is told of (`memory`, 2048 to 8992 MB)
    and the room the loader keeps for itself (`room`, 128 to 2048 MB). Left and right step them, `X` puts the focused one back
    to automatic (the key is taken out of `game.cfg`);
  - where the game's folder, log, crash report and saved data are.
- **Settings** (`-`): the games folder (picked among the card's folders), the shelf, covers shown or hidden, cases or flat
  pictures, the theme, downloading covers, files from a computer and the pace they are taken at,
  looking for games again.
- **Starts a game** (`A`): asks the homebrew loader to start `/switch/nx2-flow/nx2-game.nro` next, with the NUP file, this
  menu's own path and, when the game has one set, `room=<MB>` as its arguments, and exits. If `nx2-game.nro` isn't there, it
  says so.
- **Receives files from a computer** (`ZL`, or Settings): what Toronja Transfer does, in the menu. See below.

| Button | On the shelf | In a panel |
|---|---|---|
| Left / Right | the game before, the next | change the focused line |
| `A` | start the game | change it, open it, say yes |
| `X` | turn the case round | advanced: automatic; slots: delete; folder picker: use this folder |
| `Y` | the game's options | |
| `L` / `R` | how the games stand | |
| `ZL` | files from a computer | |
| `-` | Settings | |
| `B` | | back, or no |
| `+` | leave the menu | |

## Building

For the console (ounce-devkit, in devkitPro's shell):

```
make                    out/nx2-flow.nro
```

Nothing else is needed: the text is drawn from glyph pictures made beforehand (`source/font_baked.h`, from
`tools/bake_font.py`), and pictures are decoded by `stb_image.h`. Copy `out/nx2-flow.nro` to `/switch/nx2-flow/` on the card,
beside `nx2-game.nro`.

(toronja's own FreeType text is still in `ui_canvas.h`, behind `UI_FONT_FREETYPE`, for a later build that loads the console's
font: it would draw every script. Nothing builds it yet.)

The previews, on a PC (a PC `g++`, Python and Pillow):

```
make preview            or, on Windows:   sh tools/render_previews.sh
```

That builds `preview/main.cpp` with the menu's own code, makes a pretend card (`tools/make_fake_card.py`: tiny NUP files of
games that don't exist, with made-up names, icons, covers and saved data), plays the menu through its screens and writes them
to `out/preview/*.png`. What the previews show is what the console's screen shows: the menu draws on a plain canvas either way.
In the previews the receiver of files is a stand-in that says what a scene needs; the receiver itself runs on a PC too:

```
make nettest            out/preview/transfer_test
```

## Files from a computer

The screen shows the console's address and listens on port 28302 with Toronja Transfer's protocol ("TTX1": PING, LIST, PUT, GET,
DELETE, HASH, MKDIR), so the program that sends to that one sends to this one, unchanged. It listens while the screen is open;
leaving it looks for games again if anything came. While a file is on its way the screen shows its name, how much has come, the
pace, and how many blocks wait for the card.

How a file is received (`source/transfer.cpp`):

- the network thread fills blocks of one size and hands them to a second thread, which writes each to the card with one write;
- at most a fixed number of blocks wait between the two. When the card falls behind, the network thread stops reading the
  socket, whose own buffer is small, so the sender is held back by the connection itself and nothing piles up in memory;
- the file is given its whole size before the first byte is written (`<name>.ttpart`, renamed at the end);
- the pace can be capped, from the first byte, for a card that takes long over a write now and then.

The four numbers are keys of `/nx2-flow/menu.cfg`, always written there so that they are found:

| key | meaning | default |
|---|---|---|
| `transfer_block_kb` | one write to the card, in KB (64 to 4096) | 1024 |
| `transfer_queue` | blocks that may wait for the card before the network is held back (2 to 64) | 8 |
| `transfer_kbps` | the most taken from the network, KB a second; 0: no limit. Also in Settings, "Most the receiver takes" | 0 |
| `transfer_rcvbuf_kb` | the receiving socket's own buffer, in KB (64 to 2048) | 256 |

Every file of 8 MB or more leaves its figures in `/nx2-flow/transfer.log`: seconds, MB a second, **the longest time with nothing
written** and when it was, **how full the queue got**, how many writes took 100 ms, 500 ms, 2 s or more, the slowest write, and
where the time went (writing, waiting for the network, held back by the queue, held back by the cap). A queue that fills and a
long slowest write say the card is the limit: a cap just under the card's steady pace then gives an even transfer. A queue that
stays at 1 says the network is the limit.

On a PC, with a folder for a card and the sender pointed at `127.0.0.1`:

```
out/preview/transfer_test <card folder> <seconds> [block KB] [queue] [KB a second] [socket buffer KB]
```

## Covers

```
/nx2-flow/covers/<ID>.jpg|png         the front
/nx2-flow/covers/full/<ID>.jpg|png    the whole wrap: back, spine and front (the shelf can turn it)
/nx2-flow/covers/ids.txt              <title id>=<GameTDB code>, one line a game
```

`<ID>` is the game's title id (16 hex digits) or the code GameTDB files it under.

**GameTDB and title ids.** GameTDB names a game's art by a short code of its own, five letters and digits for this family of
consoles (`https://art.gametdb.com/switch/cover/<region>/<code>.jpg`, and `coverfull`, `coverHQ`, `coverfullHQ`: checked
against the site, by asking for headers only). **A NUP file has the game's title id and not that code**, and no public table
that ties the two was found. So nothing is guessed: a game gets covers from GameTDB once `ids.txt` says which code it has.

- `tools/fetch_covers.py`, on a computer: reads a folder of NUP files, takes codes from `--id <title id>=<code>` or an
  existing `ids.txt`, downloads the covers and writes them, with `ids.txt`, as the menu looks for them (`--card E:\` or
  `--out <folder>`). With `--match-titles` it looks a game's code up by its exact title in GameTDB's own list of games; that
  part could not be tried here (the site refused this computer's automated request for the list), and two games can share a
  name, so it is off unless asked for.
- On the console: Settings, Download covers. For the games `ids.txt` has a code for, when the console has internet access.
- **Games of the newer console**: whether GameTDB files them under `switch/` too, or anywhere yet, isn't known. A game with no
  cover shows a case made of its own icon and name.

## What is where

| | |
|---|---|
| `source/app.cpp`, `app.h` | the menu: the shelf, a game's options, its slots and advanced options, the settings, the folder picker, the file receiver's screen |
| `source/library.cpp`, `library.h` | the card's layout, the two settings files, the list of games and its cache, slots of saved data |
| `source/nup.cpp`, `nup.h` | what the menu reads of a NUP file |
| `source/covers.cpp`, `covers.h` | cover files, `ids.txt`, the download |
| `source/transfer.cpp`, `transfer.h` | files from a computer: Toronja Transfer's protocol, received through a queue |
| `source/images.cpp`, `stb_image.h` | JPEG and PNG, decoded and made smaller |
| `source/ui_canvas.h`, `ui_views.h` | toronja's canvas, text, shelf, cases and buttons, as changed by `tools/adapt_ui.py` |
| `source/font_baked.h` | the font, as glyph pictures (Aileron Regular, CC0) |
| `source/main_switch.cpp` | the console: the framebuffer, the pad, the network, the keyboard, handing over to the game loader |
| `preview/main.cpp` | the same menu on a PC, rendered to pictures |
| `preview/transfer_test.cpp` | the file receiver on a PC, with a folder for a card |
| `tools/` | the font, the pretend card, the previews, the covers script |

## Not tried on a console

This was written and looked at through the previews, and the file receiver was tried on a PC against a real sender (the
program that sends to Toronja Transfer). Still to be seen on the console itself:

- **Presenting**: the framebuffer on the console's window (as toronja's menu makes it), and how fast the shelf redraws, in
  software, at 1280x720.
- **The pad**: buttons and the repeat of a held direction.
- **Starting the game loader**: `envSetNextLoad` with the arguments in quotes (a NUP file's name has spaces), `room=<MB>`
  among them, and coming back to the menu after a game.
- **The keyboard**: the console's own, for a slot's name, shown over the menu. If it gives no name the menu asks whether to make
  the slot as `slot N`, so a console where it can't be shown still makes slots.
- **The download**: the console's sockets, and GameTDB reached plainly (`http://`); a secure connection through the system's
  own TLS is there for a redirect to `https://` and is the least sure part.
- **Files from a computer, on the card**: the commands and the checksums were tried on a PC (every command; a 200 MB file each
  way; a capped pace), where a write takes no time. What the design is for can only be seen on the console: the pace into a real
  card, whether the longest time with nothing written stays short, and which of the four numbers suit the card. The socket's
  buffer sizes are asked of the system and may be refused (the defaults are then used). The menu's own thread comes before the
  two that move a file, so the screen is redrawn only a few times a second meanwhile.
- **Text**: the font has the Latin letters without accents (an accented letter is drawn as its plain one); a title in another
  script shows no text of its own, only its icon.
