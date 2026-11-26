// SPDX-License-Identifier: GPL-2.0-or-later

// A NUP container, read for the menu: which game it is. Only its superblock, the head of its
// table of contents (the packages) and the packages' blocks of facts are read: never a file's
// bytes, so a container of tens of gigabytes costs a few kilobytes of reading.
//
// The layout (all little-endian) is the one nx2-relink's tools/nup.py reads: a superblock of
// 0x80 bytes ("NUP\1": where the table of contents is and how long, how many packages); the
// table of contents ("TOC\1": the packages first, 0x40 bytes each); a package has its title's
// number, a version, a kind (0 the game, 1 an update, 2 added content) and a block of facts:
// its NPDM, its NACP, its icon (a JPEG), its display version, its title, its publisher.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Nup {

struct Info {
    std::uint64_t title_id{};      // Of the game's own package (kind 0); 0 when there is none.
    std::uint32_t version{};       // Of the newest package that has the game's program.
    int packages{};
    int updates{};                 // Packages of kind 1.
    int addons{};                  // Packages of kind 2.
    std::string title;             // Empty when the container gives none.
    std::string publisher;
    std::string display_version;   // Of the newest update, or the game's.
    std::vector<std::uint8_t> icon; // A JPEG; empty when there is none.
};

/// Reads what the menu shows of a container. False, with `error` in words, when the file
/// isn't one or can't be read.
bool Read(const std::string& path, Info& out, std::string& error);

} // namespace Nup
