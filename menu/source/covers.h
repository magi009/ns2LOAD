// SPDX-License-Identifier: GPL-2.0-or-later

// Cover artwork for the shelf, after toronja's (src/citra_switch/covers.h).
//
// Files live in /ns2LOAD/covers:
//   covers/full/<ID>.jpg|png   the whole wrap: back, spine and front (the shelf can turn it)
//   covers/<ID>.jpg|png        the front only
// where <ID> is the game's title id (16 hex digits) or the code GameTDB files the game under.
// A NUP container has the title id and not that code, so the two are tied by a line of
//   covers/ids.txt             <title id>=<GameTDB code>
// which tools/fetch_covers.py writes on a PC, or anyone by hand.
//
// They can be copied there by hand, fetched on a PC (tools/fetch_covers.py), or downloaded by
// the console itself when it has internet access and ids.txt says which code a game has.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "library.h"

namespace Covers {

struct Art {
    SwitchFrontend::Ui::Image image;
    bool full_wrap{};
};

/// The cover for a game, or null. Decoding happens here, at most `budget` files per call
/// across all games (pass the same counter for a whole frame); a game over budget reports
/// null until a later frame.
std::shared_ptr<const Art> Get(const Flow::Game& game, int& budget);

/// Whether there is a cover file for this game at all (it may not be decoded yet).
bool HasFile(const Flow::Game& game);

/// The code GameTDB files a game under, by covers/ids.txt; empty when the file doesn't say.
std::string CodeFor(const Flow::Game& game);

/// Creates the covers folders and a note saying what goes in them.
void EnsureFolders();

/// Forgets what is decoded, "this game has no cover" answers and ids.txt.
void Reset();

/// Downloads, in the background, the covers that are missing for the games that have a code
/// in ids.txt. False when nothing was started (no network here, nothing to fetch, or a
/// download is already under way); Status() says which.
bool StartDownload(const std::vector<Flow::Game>& games);
bool Downloading();
/// One line on the last or the running download; empty before the first.
std::string Status();
/// True once after a download that brought something: the shelf should look again.
bool TakeChanged();

} // namespace Covers
