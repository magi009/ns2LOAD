// SPDX-License-Identifier: GPL-2.0-or-later

// The card as the menu sees it: where things are (docs/layout.md), the two kinds of settings
// file, and the list of games: one for each NUP container in the games folder.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ui_views.h"

namespace Flow {

// What is where, as paths of the card ("/ns2LOAD/..."). The card itself is CardRoot():
// "sdmc:" on the console, a folder on a PC (the previews).
inline constexpr const char* kName = "ns2LOAD";
inline constexpr const char* kHome = "/ns2LOAD";
inline constexpr const char* kDefaultGames = "/ns2LOAD/games";
inline constexpr const char* kCovers = "/ns2LOAD/covers";
inline constexpr const char* kCache = "/ns2LOAD/cache";
inline constexpr const char* kMenuCfg = "/ns2LOAD/menu.cfg";
inline constexpr const char* kMenuNro = "/switch/ns2LOAD/ns2LOAD.nro";
inline constexpr const char* kGameNro = "/switch/ns2LOAD/ns2LOAD-game.nro";

const std::string& CardRoot();
void SetCardRoot(std::string root);
/// A path of the card as the C library takes it.
std::string OnCard(const std::string& card_path);

bool Exists(const std::string& card_path);
bool IsFolder(const std::string& card_path);
void MakeFolders(const std::string& card_path); // With every folder on the way.
/// The folders in a folder, by name; `files` too when it is given.
std::vector<std::string> ListFolders(const std::string& card_path,
                                     std::vector<std::string>* files = nullptr);

/// A settings file: lines of key=value. Reading a file that isn't there gives nothing.
using Settings = std::map<std::string, std::string>;
Settings ReadSettings(const std::string& card_path);
bool WriteSettings(const std::string& card_path, const Settings& values);

struct Game {
    std::string path;      // The container, as a path of the card.
    std::string file;      // Its name alone.
    std::uint64_t size{};
    std::int64_t mtime{};
    std::uint64_t title_id{};
    std::string id;        // The title id, 16 hex digits, lower case: the game's folder's name.
    std::string title;
    std::string publisher;
    std::string version;
    std::string note;      // "1 update, 2 added contents" and the like.
    std::string error;     // Why it can't be started; empty when it can.
    std::string slot;      // The slot of saved data in use (game.cfg's "save"); empty: "default".
    // The container's own icon: decoded when first shown.
    std::vector<SwitchFrontend::Ui::u32> icon;
    int icon_size{};
    bool icon_tried{};

    std::string Folder() const {
        return std::string{kHome} + "/" + id;
    }
};

/// The games of a folder, by title. What was read of each container is kept in a small file
/// under kCache, by the container's name, size and time: a library opens without reading the
/// containers again.
std::vector<Game> ScanGames(const std::string& games_folder);

/// Decodes a game's icon if it hasn't been yet (from kCache/<id>.jpg). At most `budget` a call.
void LoadIcon(Game& game, int& budget);

/// The sets of mods a game has: the folders of <game folder>/modsets, by name.
std::vector<std::string> ModSets(const Game& game);

/// The slots of saved data a game has besides "default": the folders of <game folder>/saves.
std::vector<std::string> SaveSlots(const Game& game);
/// Where a slot's data is, as a path of the card. "" and "default" are <game folder>/save.
std::string SlotFolder(const Game& game, const std::string& slot);
/// A name as a slot may have it: letters, digits, spaces, "-", "_" and ".", at most 32 of
/// them, and not "default". Empty when nothing usable is left of what was typed.
std::string CleanSlotName(const std::string& typed);

/// A folder and everything in it, copied to a folder that isn't there yet. False, with why,
/// when something couldn't be read or written (what was copied until then stays).
bool CopyFolder(const std::string& from_card_path, const std::string& to_card_path, std::string& error);
/// A folder and everything in it, deleted.
bool RemoveFolder(const std::string& card_path);

std::string HumanSize(std::uint64_t bytes);

} // namespace Flow
