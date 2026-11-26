// SPDX-License-Identifier: GPL-2.0-or-later

#include "app.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "covers.h"
#include "platform.h"
#include "transfer.h"

namespace Flow {

using namespace SwitchFrontend::Ui;

namespace {

constexpr int kRowH = 52;
constexpr int kRowGap = 8;
constexpr int kPickerRows = 8;
constexpr int kSettingsVisible = 7;
constexpr int kSavesVisible = 6;

// The rows of a game's options, of the settings.
enum SheetRow { kSheetLog, kSheetMods, kSheetSaves, kSheetAdvanced };
enum AdvancedRow { kAdvancedMemory, kAdvancedRoom };
enum SettingRow { kSetFolder, kSetShelf, kSetCovers, kSetCases, kSetTheme, kSetDownload, kSetTransfer, kSetCap, kSetRescan };

// What the two advanced numbers may be (docs/layout.md), and the steps the pad takes.
constexpr int kMemoryLow = 2048, kMemoryHigh = 8992, kMemoryStep = 128;
constexpr int kRoomLow = 128, kRoomHigh = 2048, kRoomStep = 64, kRoomDefault = 384;

// The paces the receiver can be held to, KB a second (0: none). Any other number can be
// written in menu.cfg by hand.
constexpr int kCapChoices[] = {0, 512, 1024, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152};

bool EndsWithNup(const std::string& name) {
    if (name.size() < 4) {
        return false;
    }
    std::string tail = name.substr(name.size() - 4);
    for (char& ch : tail) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return tail == ".nup";
}

std::string LowerCase(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return s;
}

std::string Value(const Settings& cfg, const char* key) {
    const auto it = cfg.find(key);
    return it == cfg.end() ? std::string{} : it->second;
}

int Number(const std::string& text, int fallback) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 7) {
        return fallback;
    }
    return std::atoi(text.c_str());
}

/// A number one step on, between two ends, with "automatic" (empty) beyond both of them.
std::string Step(const std::string& now, int dir, int lowest, int highest, int step) {
    if (now.empty()) {
        return std::to_string(dir > 0 ? lowest : highest);
    }
    const int v = std::clamp(Number(now, lowest), lowest, highest);
    if (dir > 0) {
        return v >= highest ? std::string{} : std::to_string(std::min(highest, lowest + ((v - lowest) / step + 1) * step));
    }
    return v <= lowest ? std::string{} : std::to_string(std::max(lowest, lowest + ((v - lowest + step - 1) / step - 1) * step));
}

std::string CapText(int kbps) {
    if (kbps <= 0) {
        return "No limit";
    }
    if (kbps % 1024 == 0) {
        return std::to_string(kbps / 1024) + " MB/s";
    }
    if (kbps == 512) {
        return "0.5 MB/s";
    }
    return std::to_string(kbps) + " KB/s";
}

std::string Megabytes(double bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", bytes / 1048576.0);
    return text;
}

std::string Count(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

void Dim(Canvas& c) {
    c.FillRect(0, 0, c.Width(), c.Height(), g_theme_light ? MakeColor(0xF2, 0xF2, 0xF2, 0xC8) : MakeColor(0x10, 0x11, 0x13, 0xC8));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Settings and the library

void App::Start() {
    font.InitLibrary();
    LoadSettings();
    ApplyTheme(light);
    g_shelf_cases = cases;
    Covers::EnsureFolders();
    if (games_folder == kDefaultGames) {
        MakeFolders(kDefaultGames); // (so that there is somewhere to put the first game)
    }
    Rescan();
}

void App::LoadSettings() {
    const Settings cfg = ReadSettings(kMenuCfg);
    if (const std::string v = Value(cfg, "games"); !v.empty()) {
        games_folder = v;
        while (games_folder.size() > 1 && games_folder.back() == '/') {
            games_folder.pop_back();
        }
    }
    const std::string name = Value(cfg, "style");
    style = static_cast<int>(ShelfStyle::Showroom); // (how the games stand when menu.cfg doesn't say)
    for (int k = 0; k < static_cast<int>(ShelfStyle::Count); ++k) {
        if (name == ShelfStyleName(static_cast<ShelfStyle>(k))) {
            style = k;
        }
    }
    covers_shown = Value(cfg, "covers") != "hidden";
    cases = Value(cfg, "cases") != "off";
    light = Value(cfg, "theme") == "light";
    transfer_kbps = Number(Value(cfg, "transfer_kbps"), 0);
    transfer_block_kb = Number(Value(cfg, "transfer_block_kb"), 1024);
    transfer_queue = Number(Value(cfg, "transfer_queue"), 8);
    transfer_rcvbuf_kb = Number(Value(cfg, "transfer_rcvbuf_kb"), 256);
}

void App::SaveSettings() {
    Settings cfg = ReadSettings(kMenuCfg); // (a key this version doesn't know is kept)
    cfg["games"] = games_folder;
    cfg["style"] = ShelfStyleName(static_cast<ShelfStyle>(style));
    cfg["covers"] = covers_shown ? "shown" : "hidden";
    cfg["cases"] = cases ? "on" : "off";
    cfg["theme"] = light ? "light" : "dark";
    // The receiver's four numbers are always written, so that whoever opens the file finds them.
    cfg["transfer_kbps"] = std::to_string(transfer_kbps);
    cfg["transfer_block_kb"] = std::to_string(transfer_block_kb);
    cfg["transfer_queue"] = std::to_string(transfer_queue);
    cfg["transfer_rcvbuf_kb"] = std::to_string(transfer_rcvbuf_kb);
    if (!WriteSettings(kMenuCfg, cfg)) {
        Notice("The settings couldn't be written to the card", true);
    }
    settings_changed = false;
}

void App::Finish() {
    if (settings_changed) {
        SaveSettings();
    }
}

void App::Rescan() {
    const std::string keep = Selected() ? Selected()->path : std::string{};
    games = ScanGames(games_folder);
    for (Game& game : games) {
        if (!game.id.empty()) {
            game.slot = Value(ReadSettings(game.Folder() + "/game.cfg"), "save");
        }
    }
    Covers::Reset();
    made.clear();
    selected = 0;
    for (std::size_t k = 0; k < games.size(); ++k) {
        if (games[k].path == keep) {
            selected = static_cast<int>(k);
        }
    }
    position = static_cast<float>(selected);
    flipped = false;
    flip = 0.0f;
}

void App::Notice(std::string text, bool error) {
    notice = std::move(text);
    notice_error = error;
    notice_left = 5.0f;
}

Game* App::Selected() {
    return selected >= 0 && selected < static_cast<int>(games.size()) ? &games[selected] : nullptr;
}

float App::FramePause() const {
    if (mode == Mode::Transfer) {
        return Transfer::Look().busy ? 0.08f : 0.02f;
    }
    return 0.0f;
}

// ---------------------------------------------------------------------------------------------
// What was pressed

Request App::Update(const Input& in, float dt) {
    time += dt;
    const float ease = std::min(1.0f, dt * 11.0f);
    position += (static_cast<float>(selected) - position) * ease;
    flip += ((flipped ? 1.0f : 0.0f) - flip) * std::min(1.0f, dt * 9.0f);
    if (notice_left > 0.0f) {
        notice_left -= dt;
    }
    if (Covers::TakeChanged()) {
        Covers::Reset();
        Notice(Covers::Status());
    }
    if (!copy_to.empty()) {
        FinishCopy(); // (the frame that says "copying" has been shown)
        return {};
    }
    if (ask != Ask::None) {
        UpdateAsk(in);
        return {};
    }
    switch (mode) {
    case Mode::Shelf:
        return UpdateShelf(in);
    case Mode::Sheet:
        UpdateSheet(in);
        break;
    case Mode::Advanced:
        UpdateAdvanced(in);
        break;
    case Mode::Saves:
        UpdateSaves(in);
        break;
    case Mode::Settings:
        UpdateSettings(in);
        break;
    case Mode::Picker:
        UpdatePicker(in);
        break;
    case Mode::Transfer:
        UpdateTransfer(in, dt);
        break;
    }
    return {};
}

Request App::UpdateShelf(const Input& in) {
    if (in.plus) {
        Finish();
        return {Request::Kind::Exit, {}, {}};
    }
    if (in.minus) {
        mode = Mode::Settings;
        settings_cursor = 0;
        return {};
    }
    if (in.zl) {
        OpenTransfer();
        return {};
    }
    const int count = static_cast<int>(games.size());
    if (count == 0) {
        return {};
    }
    if (in.left && selected > 0) {
        --selected;
        flipped = false;
    }
    if (in.right && selected < count - 1) {
        ++selected;
        flipped = false;
    }
    if (in.l || in.r) {
        const int n = static_cast<int>(ShelfStyle::Count);
        style = (style + (in.r ? 1 : n - 1)) % n;
        settings_changed = true;
        Notice(std::string{"Shelf: "} + ShelfStyleName(static_cast<ShelfStyle>(style)));
    }
    if (in.x) {
        flipped = !flipped;
    }
    if (in.y) {
        OpenSheet();
    }
    if (in.a) {
        const Game& game = games[selected];
        if (!game.error.empty()) {
            Notice("This file can't be started: " + game.error, true);
        } else if (!Exists(kGameNro)) {
            Notice(std::string{"The game loader isn't on the card: "} + kGameNro, true);
        } else {
            Finish();
            // The room the loader keeps is the one setting it must be told as an argument: it
            // needs it before it can read a file.
            std::string room;
            if (!game.id.empty()) {
                const int mb = Number(Value(ReadSettings(game.Folder() + "/game.cfg"), "room"), 0);
                if (mb >= kRoomLow && mb <= kRoomHigh) {
                    room = std::to_string(mb);
                }
            }
            return {Request::Kind::Launch, game.path, room};
        }
    }
    return {};
}

void App::PutQuestion(Ask what, std::string title, std::string text, std::string yes, std::string no, bool warning) {
    ask = what;
    ask_title = std::move(title);
    ask_text = std::move(text);
    ask_yes = std::move(yes);
    ask_no = std::move(no);
    ask_warning = warning;
}

void App::UpdateAsk(const Input& in) {
    if (in.b) {
        ask = Ask::None;
        return;
    }
    if (!in.a) {
        return;
    }
    const Ask what = ask;
    ask = Ask::None;
    Game* game = Selected();
    switch (what) {
    case Ask::OpenAdvanced:
        advanced_cursor = 0;
        mode = Mode::Advanced;
        break;
    case Ask::DeleteSlot:
        if (game) {
            const bool in_use = SlotInUse() == ask_name;
            if (!RemoveFolder(SlotFolder(*game, ask_name))) {
                Notice("Not all of \"" + ask_name + "\" could be deleted", true);
            } else if (in_use) {
                Notice("\"" + ask_name + "\" deleted. The game uses the default slot again.");
            } else {
                Notice("\"" + ask_name + "\" deleted");
            }
            if (in_use) {
                game_cfg["save"].clear();
                game->slot.clear();
                WriteGameCfg();
            }
            LookAtSlots();
            saves_cursor = std::min(saves_cursor, static_cast<int>(save_slots.size()) - 1);
        }
        break;
    case Ask::MakeSlot:
        MakeSlot(ask_name, ask_copy);
        break;
    case Ask::LeaveTransfer:
        LeaveTransfer();
        break;
    case Ask::None:
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// A game's options

void App::OpenSheet() {
    Game* game = Selected();
    if (!game || game->id.empty()) {
        Notice("This file has no game this can read", true);
        return;
    }
    game_cfg = ReadSettings(game->Folder() + "/game.cfg");
    mod_sets = ModSets(*game);
    sheet_cursor = 0;
    mode = Mode::Sheet;
}

bool App::WriteGameCfg() {
    const Game* game = Selected();
    if (!game) {
        return false;
    }
    MakeFolders(game->Folder());
    if (!WriteSettings(game->Folder() + "/game.cfg", game_cfg)) {
        Notice("The card couldn't be written to", true);
        return false;
    }
    return true;
}

std::vector<App::Row> App::SheetRows() {
    std::vector<Row> rows;
    const Game* game = Selected();
    if (!game) {
        return rows;
    }
    rows.push_back({"Detailed log", Exists(game->Folder() + "/debug") ? "On" : "Off",
                    "Every call the game makes is written to its log. Slower: for a report of a game that doesn't run.", true});
    const std::string set = Value(game_cfg, "modset");
    const bool there = set.empty() || std::find(mod_sets.begin(), mod_sets.end(), set) != mod_sets.end();
    rows.push_back({"Mod set", set.empty() ? "mods (default)" : there ? set : set + " (not there)",
                    "The folder of files laid over the game's own: mods/, or one of the folders of modsets/ in the game's folder.", true});
    rows.push_back({"Saved data", SlotInUse(),
                    "The slot of saved data the game uses. A opens the slots: pick another, make an empty one, copy this one."});
    std::string advanced;
    if (const std::string memory = Value(game_cfg, "memory"); !memory.empty()) {
        advanced = "memory " + memory + " MB";
    }
    if (const std::string room = Value(game_cfg, "room"); !room.empty()) {
        advanced += (advanced.empty() ? "" : ", ") + std::string{"room "} + room + " MB";
    }
    rows.push_back({"Advanced", advanced.empty() ? "Automatic" : advanced,
                    "The memory the game is told of and the room the loader keeps. They can keep a game from starting."});
    return rows;
}

void App::ChangeSheet(int row, int dir, bool press) {
    Game* game = Selected();
    if (!game) {
        return;
    }
    const std::string folder = game->Folder();
    switch (row) {
    case kSheetLog: {
        const std::string file = folder + "/debug";
        if (Exists(file)) {
            std::remove(OnCard(file).c_str());
        } else {
            MakeFolders(folder);
            if (std::FILE* f = std::fopen(OnCard(file).c_str(), "wb")) {
                std::fclose(f);
            } else {
                Notice("The card couldn't be written to", true);
            }
        }
        break;
    }
    case kSheetMods: {
        std::vector<std::string> choices{""};
        choices.insert(choices.end(), mod_sets.begin(), mod_sets.end());
        const std::string now = Value(game_cfg, "modset");
        int at = 0;
        for (std::size_t k = 0; k < choices.size(); ++k) {
            if (choices[k] == now) {
                at = static_cast<int>(k);
            }
        }
        const int n = static_cast<int>(choices.size());
        game_cfg["modset"] = choices[(at + dir + n) % n];
        WriteGameCfg();
        break;
    }
    case kSheetSaves:
        if (press) {
            LookAtSlots();
            saves_cursor = 0;
            for (std::size_t k = 0; k < save_slots.size(); ++k) {
                if (save_slots[k] == SlotInUse()) {
                    saves_cursor = static_cast<int>(k);
                }
            }
            mode = Mode::Saves;
        }
        break;
    case kSheetAdvanced:
        if (press) {
            PutQuestion(Ask::OpenAdvanced, "Advanced options",
                        "These can keep a game from starting; change them only if you know what they do. Each can be put back to "
                        "automatic, which is right for nearly every game.",
                        "I understand", "Back", true);
        }
        break;
    default:
        break;
    }
}

void App::UpdateSheet(const Input& in) {
    const int count = static_cast<int>(SheetRows().size());
    if (in.b || in.y) {
        mode = Mode::Shelf;
        return;
    }
    if (in.up && sheet_cursor > 0) {
        --sheet_cursor;
    }
    if (in.down && sheet_cursor < count - 1) {
        ++sheet_cursor;
    }
    if (in.left) {
        ChangeSheet(sheet_cursor, -1, false);
    }
    if (in.right) {
        ChangeSheet(sheet_cursor, 1, false);
    }
    if (in.a) {
        ChangeSheet(sheet_cursor, 1, true);
    }
}

std::vector<App::Row> App::AdvancedRows() {
    std::vector<Row> rows;
    const std::string memory = Value(game_cfg, "memory");
    rows.push_back({"Memory the game is told of", memory.empty() ? "Automatic" : memory + " MB",
                    "How much memory the game believes the console has, from " + std::to_string(kMemoryLow) + " to " +
                        std::to_string(kMemoryHigh) + " MB. Automatic: the loader decides. X puts it back to automatic.",
                    true});
    const std::string room = Value(game_cfg, "room");
    rows.push_back({"Room the loader keeps for itself", room.empty() ? "Automatic (" + std::to_string(kRoomDefault) + " MB)" : room + " MB",
                    "Memory kept out of the game's reach for the loader's own work, from " + std::to_string(kRoomLow) + " to " +
                        std::to_string(kRoomHigh) + " MB. X puts it back to automatic.",
                    true});
    return rows;
}

void App::ChangeAdvanced(int row, int dir) {
    const char* key = row == kAdvancedMemory ? "memory" : "room";
    const std::string now = Value(game_cfg, key);
    if (dir == 0) {
        if (now.empty()) {
            return;
        }
        game_cfg[key].clear();
        Notice(std::string{row == kAdvancedMemory ? "Memory" : "Room"} + ": back to automatic");
    } else if (row == kAdvancedMemory) {
        game_cfg[key] = Step(now, dir, kMemoryLow, kMemoryHigh, kMemoryStep);
    } else {
        game_cfg[key] = Step(now, dir, kRoomLow, kRoomHigh, kRoomStep);
    }
    WriteGameCfg();
}

void App::UpdateAdvanced(const Input& in) {
    if (in.b) {
        mode = Mode::Sheet;
        return;
    }
    if (in.up && advanced_cursor > 0) {
        --advanced_cursor;
    }
    if (in.down && advanced_cursor < 1) {
        ++advanced_cursor;
    }
    if (in.left) {
        ChangeAdvanced(advanced_cursor, -1);
    }
    if (in.right) {
        ChangeAdvanced(advanced_cursor, 1);
    }
    if (in.x) {
        ChangeAdvanced(advanced_cursor, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// Slots of saved data

std::string App::SlotInUse() const {
    const std::string slot = Value(game_cfg, "save");
    return slot.empty() ? "default" : slot;
}

void App::LookAtSlots() {
    save_slots.assign(1, "default");
    if (const Game* game = Selected()) {
        const std::vector<std::string> more = SaveSlots(*game);
        save_slots.insert(save_slots.end(), more.begin(), more.end());
    }
    // (a slot game.cfg names and nobody has made yet: the loader makes it when the game runs)
    if (std::find(save_slots.begin(), save_slots.end(), SlotInUse()) == save_slots.end()) {
        save_slots.push_back(SlotInUse());
    }
}

std::string App::NextSlotName() const {
    for (int n = 2;; ++n) {
        const std::string name = "slot " + std::to_string(n);
        const bool taken = std::any_of(save_slots.begin(), save_slots.end(), [&](const std::string& s) { return LowerCase(s) == name; });
        if (!taken) {
            return name;
        }
    }
}

std::vector<App::Row> App::SaveRows() {
    std::vector<Row> rows;
    const Game* game = Selected();
    if (!game) {
        return rows;
    }
    const std::string in_use = SlotInUse();
    for (const std::string& slot : save_slots) {
        const std::string folder = SlotFolder(*game, slot);
        const bool there = IsFolder(folder);
        Row row;
        row.label = slot;
        row.check = slot == in_use;
        row.value = row.check ? "In use" : "";
        if (!there) {
            row.value += row.value.empty() ? "nothing saved yet" : ", nothing saved yet";
        }
        row.help = folder + "/";
        rows.push_back(std::move(row));
    }
    rows.push_back({"New empty slot", "A", "A slot with nothing saved in it: the game begins from nothing there."});
    rows.push_back({"Copy \"" + in_use + "\" into a new slot", "A", "A second copy of what is saved in the slot in use, to go on from here in two ways."});
    return rows;
}

void App::BeginNewSlot(bool copy) {
    const std::string suggestion = NextSlotName();
    std::string typed;
    if (!Platform::AskText("Name of the new slot", suggestion, typed)) {
        // No name: the user backed out of the keyboard, or it couldn't be shown. Which of the
        // two can't be told, so it is asked rather than guessed.
        ask_name = suggestion;
        ask_copy = copy;
        PutQuestion(Ask::MakeSlot, "No name was given", "Make the slot as \"" + suggestion + "\"?", "Make it", "Cancel");
        return;
    }
    const std::string name = CleanSlotName(typed);
    if (name.empty()) {
        Notice("That name can't be used: letters, digits, spaces, - _ and . only, and not \"default\"", true);
        return;
    }
    const bool taken = std::any_of(save_slots.begin(), save_slots.end(), [&](const std::string& s) { return LowerCase(s) == LowerCase(name); });
    if (taken) {
        Notice("There is a slot of that name already", true);
        return;
    }
    MakeSlot(name, copy);
}

void App::MakeSlot(const std::string& name, bool copy) {
    const Game* game = Selected();
    if (!game) {
        return;
    }
    const std::string from = SlotFolder(*game, SlotInUse()), to = SlotFolder(*game, name);
    if (copy && IsFolder(from)) {
        copy_from = from;
        copy_to = to;
        copy_name = name;
        busy = "Copying the saved data of \"" + SlotInUse() + "\"...";
        return;
    }
    MakeFolders(to);
    if (!IsFolder(to)) {
        Notice("The card couldn't be written to", true);
        return;
    }
    LookAtSlots();
    for (std::size_t k = 0; k < save_slots.size(); ++k) {
        if (save_slots[k] == name) {
            saves_cursor = static_cast<int>(k);
        }
    }
    Notice(copy ? "\"" + name + "\" made, empty: there was nothing saved to copy yet. A uses it." : "\"" + name + "\" made. A uses it.");
}

void App::FinishCopy() {
    std::string error;
    const bool ok = CopyFolder(copy_from, copy_to, error);
    const std::string name = copy_name;
    copy_from.clear();
    copy_to.clear();
    copy_name.clear();
    busy.clear();
    LookAtSlots();
    for (std::size_t k = 0; k < save_slots.size(); ++k) {
        if (save_slots[k] == name) {
            saves_cursor = static_cast<int>(k);
        }
    }
    if (ok) {
        Notice("\"" + name + "\" made, a copy of \"" + SlotInUse() + "\". A uses it.");
    } else {
        Notice("The copy stopped: " + error, true);
    }
}

void App::UpdateSaves(const Input& in) {
    Game* game = Selected();
    const int slots = static_cast<int>(save_slots.size());
    const int count = slots + 2;
    if (in.b || !game) {
        mode = Mode::Sheet;
        return;
    }
    if (in.up && saves_cursor > 0) {
        --saves_cursor;
    }
    if (in.down && saves_cursor < count - 1) {
        ++saves_cursor;
    }
    if (in.a) {
        if (saves_cursor < slots) {
            const std::string& slot = save_slots[saves_cursor];
            game_cfg["save"] = slot == "default" ? std::string{} : slot;
            if (WriteGameCfg()) {
                game->slot = game_cfg["save"];
                Notice("The game now uses \"" + slot + "\"");
            }
        } else {
            BeginNewSlot(saves_cursor == slots + 1);
        }
    }
    if (in.x && saves_cursor < slots) {
        const std::string& slot = save_slots[saves_cursor];
        if (slot == "default") {
            Notice("The default slot isn't deleted from here", true);
        } else if (!IsFolder(SlotFolder(*game, slot))) {
            Notice("There is nothing of \"" + slot + "\" on the card to delete", true);
        } else {
            ask_name = slot;
            PutQuestion(Ask::DeleteSlot, "Delete \"" + slot + "\"?",
                        "Everything saved in this slot is deleted from the card. It can't be brought back." +
                            std::string{slot == SlotInUse() ? " The game uses it now: it would go back to the default slot." : ""},
                        "Delete", "Keep it", true);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The settings

std::vector<App::Row> App::SettingsRows() {
    std::vector<Row> rows;
    rows.push_back({"Games folder", games_folder, "The folder of the card the NUP files are in. A opens the card's folders to pick one.", false});
    rows.push_back({"Shelf", ShelfStyleName(static_cast<ShelfStyle>(style)), "How the games stand in the library. L and R change it from the library too.", true});
    rows.push_back({"Covers", covers_shown ? "Shown" : "Hidden", "Hidden, every game shows the icon its own file has.", true});
    rows.push_back({"Cases", cases ? "On" : "Off", "Covers stand as cases with a side to them, or as flat pictures.", true});
    rows.push_back({"Theme", light ? "Light" : "Dark", "", true});
    const std::string status = Covers::Status();
    rows.push_back({"Download covers", Covers::Downloading() ? "Working..." : "A",
                    status.empty() ? std::string{"From GameTDB, for the games that covers/ids.txt has a code for. Needs internet access."} : status, false});
    rows.push_back({"Receive files from a computer", "A",
                    "Games and anything else, over the local network, from the program that sends to Toronja Transfer. ZL opens it from "
                    "the library too.",
                    false});
    rows.push_back({"Most the receiver takes", CapText(transfer_kbps),
                    "A card that chokes now and then is quicker at a pace it always keeps. No limit: as fast as the card writes.", true});
    rows.push_back({"Look for games again", "A", Count(games.size(), "game", "games") + " found in the games folder.", false});
    return rows;
}

void App::ChangeSetting(int row, int dir, bool press) {
    switch (row) {
    case kSetFolder:
        if (press) {
            OpenPicker();
        }
        return;
    case kSetShelf: {
        const int n = static_cast<int>(ShelfStyle::Count);
        style = (style + dir + n) % n;
        break;
    }
    case kSetCovers:
        covers_shown = !covers_shown;
        break;
    case kSetCases:
        cases = !cases;
        g_shelf_cases = cases;
        break;
    case kSetTheme:
        light = !light;
        ApplyTheme(light);
        made.clear();
        break;
    case kSetDownload:
        if (press && !Covers::StartDownload(games)) {
            Notice(Covers::Status(), true);
        }
        return;
    case kSetTransfer:
        if (press) {
            OpenTransfer();
        }
        return;
    case kSetCap: {
        // The choice at or just under what is set, then one step: a number written by hand
        // that isn't one of the choices moves to the nearest.
        const int n = static_cast<int>(std::size(kCapChoices));
        int at = 0;
        for (int k = 0; k < n; ++k) {
            if (kCapChoices[k] <= transfer_kbps) {
                at = k;
            }
        }
        if (dir < 0 && kCapChoices[at] != transfer_kbps) {
            ++at; // (so that one step down lands on the choice just under)
        }
        transfer_kbps = kCapChoices[std::clamp(at + dir, 0, n - 1)];
        break;
    }
    case kSetRescan:
        if (press) {
            std::remove(OnCard(std::string{kCache} + "/library.txt").c_str());
            Rescan();
            Notice(Count(games.size(), "game", "games") + " found");
        }
        return;
    default:
        return;
    }
    settings_changed = true;
}

void App::UpdateSettings(const Input& in) {
    const int count = static_cast<int>(SettingsRows().size());
    if (in.b || in.minus) {
        Finish();
        mode = Mode::Shelf;
        return;
    }
    if (in.up && settings_cursor > 0) {
        --settings_cursor;
    }
    if (in.down && settings_cursor < count - 1) {
        ++settings_cursor;
    }
    if (in.left) {
        ChangeSetting(settings_cursor, -1, false);
    }
    if (in.right) {
        ChangeSetting(settings_cursor, 1, false);
    }
    if (in.a) {
        ChangeSetting(settings_cursor, 1, true);
    }
}

void App::OpenPicker() {
    picker_path = IsFolder(games_folder) ? games_folder : std::string{};
    if (picker_path == "/") {
        picker_path.clear();
    }
    PickerLook();
    mode = Mode::Picker;
}

void App::PickerLook() {
    std::vector<std::string> files;
    picker_folders = ListFolders(picker_path, &files);
    picker_nups = static_cast<int>(std::count_if(files.begin(), files.end(), EndsWithNup));
    picker_cursor = 0;
}

void App::UpdatePicker(const Input& in) {
    const int count = static_cast<int>(picker_folders.size());
    if (in.up && picker_cursor > 0) {
        --picker_cursor;
    }
    if (in.down && picker_cursor < count - 1) {
        ++picker_cursor;
    }
    if (in.a && picker_cursor < count) {
        picker_path += "/" + picker_folders[picker_cursor];
        PickerLook();
        return;
    }
    if (in.b) {
        if (picker_path.empty()) {
            mode = Mode::Settings;
        } else {
            const std::size_t slash = picker_path.rfind('/');
            const std::string left = picker_path.substr(slash + 1);
            picker_path.resize(slash);
            PickerLook();
            for (std::size_t k = 0; k < picker_folders.size(); ++k) {
                if (picker_folders[k] == left) {
                    picker_cursor = static_cast<int>(k);
                }
            }
        }
        return;
    }
    if (in.x) {
        games_folder = picker_path.empty() ? "/" : picker_path;
        SaveSettings();
        Rescan();
        mode = Mode::Settings;
        Notice(Count(games.size(), "game", "games") + " found in " + games_folder);
    }
}

// ---------------------------------------------------------------------------------------------
// Files from a computer

void App::OpenTransfer() {
    transfer_back = mode == Mode::Settings ? Mode::Settings : Mode::Shelf;
    Finish();
    Platform::StartNetwork();
    Transfer::Tuning tuning;
    tuning.block_kb = transfer_block_kb;
    tuning.queue = transfer_queue;
    tuning.kbps = transfer_kbps;
    tuning.rcvbuf_kb = transfer_rcvbuf_kb;
    Transfer::Start(tuning);
    Platform::KeepAwake(true);
    transfer_files_before = Transfer::Look().files;
    transfer_seen = 0;
    transfer_speed = 0.0f;
    transfer_since = 0.0f;
    address = Platform::Address();
    address_age = 0.0f;
    mode = Mode::Transfer;
}

void App::LeaveTransfer() {
    const std::uint32_t files = Transfer::Look().files - transfer_files_before;
    Transfer::Stop();
    Platform::KeepAwake(false);
    mode = transfer_back;
    if (files != 0) {
        // A game may be among what came, or a cover.
        Rescan();
        Notice(Count(files, "file", "files") + " moved; " + Count(games.size(), "game", "games") + " in the games folder");
    }
}

void App::UpdateTransfer(const Input& in, float dt) {
    address_age += dt;
    if (address_age > 2.0f) {
        address = Platform::Address();
        address_age = 0.0f;
    }
    const Transfer::State state = Transfer::Look();
    // The pace, over about a second: what the bar moved by, smoothed.
    transfer_since += dt;
    if (!state.busy) {
        transfer_seen = 0;
        transfer_speed = 0.0f;
        transfer_since = 0.0f;
    } else if (transfer_since >= 0.25f) {
        const std::uint64_t moved = state.done >= transfer_seen ? state.done - transfer_seen : state.done;
        const float now = static_cast<float>(moved) / transfer_since;
        transfer_speed = transfer_speed == 0.0f ? now : transfer_speed + (now - transfer_speed) * 0.3f;
        transfer_seen = state.done;
        transfer_since = 0.0f;
    }
    if (in.b || in.zl) {
        if (state.busy) {
            PutQuestion(Ask::LeaveTransfer, "A file is on its way", "Leaving stops it. What has arrived of that file is deleted.", "Stop and leave",
                        "Stay", true);
        } else {
            LeaveTransfer();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing

void App::Draw(Canvas& c) {
    if (mode == Mode::Transfer) {
        // (no shelf behind this one: drawing it would take time from the threads at work)
        DrawBackdrop(c);
        DrawTransfer(c);
    } else {
        DrawShelfScreen(c);
        switch (mode) {
        case Mode::Sheet:
            DrawSheet(c);
            break;
        case Mode::Advanced:
            DrawAdvanced(c);
            break;
        case Mode::Saves:
            DrawSaves(c);
            break;
        case Mode::Settings:
            DrawSettings(c);
            break;
        case Mode::Picker:
            DrawSettings(c);
            DrawPicker(c);
            break;
        default:
            break;
        }
    }
    if (ask != Ask::None) {
        DrawAsk(c);
    }
    if (!busy.empty()) {
        DrawBusy(c);
    }
    DrawNotice(c);
}

void App::DrawBackdrop(Canvas& c) {
    FillGradientV(c, 0, 0, c.Width(), c.Height(), kColShelfTop, kColShelfHorizon);
}

void App::DrawShelfScreen(Canvas& c) {
    const int W = c.Width(), H = c.Height();
    const int count = static_cast<int>(games.size());

    ShelfView shelf;
    shelf.x = 0;
    shelf.y = 58;
    shelf.w = W;
    shelf.h = H - 58 - 164;
    shelf.style = static_cast<ShelfStyle>(style);
    shelf.position = position;
    shelf.count = count;
    shelf.flip = flip;
    shelf.time = time;

    // Backdrop: a wall that brightens towards the horizon, then a floor that takes the
    // reflections and falls away into the bottom of the screen.
    const int floor = static_cast<int>(ShelfFloorY(shelf));
    FillGradientV(c, 0, 0, W, floor, kColShelfTop, kColShelfHorizon);
    FillGradientV(c, 0, floor, W, H - floor, kColFloorNear, kColFloorFar);

    // Top line: the menu on the left, where we are in the library on the right.
    font.Draw(c, 28, 40, kName, 24, kColText);
    if (count > 0) {
        const std::string counter = std::to_string(selected + 1) + " of " + std::to_string(count);
        font.Draw(c, W - 28 - font.Measure(counter, 17), 38, counter, 17, kColTextDim);
    }

    const int radius = 30;
    const int bar_y = H - 22 - radius;
    if (count == 0) {
        const char* lines[] = {"No games here yet", nullptr, nullptr, nullptr};
        const std::string where = "Put NUP files in " + games_folder + " on the card,";
        const std::string or_else = "or pick another folder in Settings.";
        const std::string by_network = "ZL receives them from a computer, over the local network.";
        lines[1] = where.c_str();
        lines[2] = or_else.c_str();
        lines[3] = by_network.c_str();
        int y = H / 2 - 64;
        for (int k = 0; k < 4; ++k) {
            const int size = k == 0 ? 28 : 19;
            font.Draw(c, (W - font.Measure(lines[k], size)) / 2, y, lines[k], size, k == 0 ? kColText : kColTextDim);
            y += k == 0 ? 44 : 28;
        }
    } else {
        // What the shelf shows of each game: its cover when there is one and covers are shown,
        // the icon its file has otherwise. A few are decoded a frame, so a new library doesn't
        // stall the menu; what isn't ready yet shows as a case with the game's name.
        int budget = 2;
        std::vector<std::shared_ptr<const Covers::Art>> held; // (kept until the frame is drawn)
        DrawShelf(c, font, shelf, [&](int index) {
            ShelfItem item;
            Game& game = games[index];
            if (covers_shown) {
                if (auto art = Covers::Get(game, budget)) {
                    item.cover = &art->image;
                    item.full_wrap = art->full_wrap;
                    held.push_back(std::move(art));
                }
            }
            LoadIcon(game, budget);
            if (!game.icon.empty()) {
                item.icon = &game.icon;
                item.icon_size = game.icon_size;
            }
            if (!item.cover && (game.icon_tried || game.id.empty())) {
                item.cover = MadeCover(game); // (a case of its own, from its icon and its name)
            }
            item.title = game.title;
            item.publisher = game.publisher;
            item.badge = game.version;
            if (!game.version.empty()) {
                item.back_text.push_back("Version " + game.version);
            }
            if (!game.note.empty()) {
                item.back_text.push_back(game.note);
            }
            if (!game.id.empty()) {
                item.back_text.push_back("Title " + game.id);
                item.back_text.push_back("Saved data: " + (game.slot.empty() ? std::string{"default"} : game.slot));
            }
            item.back_text.push_back(game.file);
            item.back_text.push_back(HumanSize(game.size));
            if (!game.error.empty()) {
                item.back_text.push_back("Can't be started: " + game.error);
            }
            return item;
        });

        // Title block, centred under the shelf.
        const Game& game = games[selected];
        const int text_w = W - 2 * 220;
        const std::string title = font.Truncate(game.title, 28, text_w);
        font.Draw(c, (W - font.Measure(title, 28)) / 2, H - 126, title, 28, kColText);
        std::string under = game.publisher;
        if (!game.version.empty()) {
            under += (under.empty() ? "" : "   ") + std::string{"v"} + game.version;
        }
        if (!game.slot.empty()) {
            under += (under.empty() ? "" : "   ") + std::string{"Saved data: "} + game.slot;
        }
        if (!game.error.empty()) {
            under = "Can't be started: " + game.error;
        }
        under = font.Truncate(under, 17, text_w);
        font.Draw(c, (W - font.Measure(under, 17)) / 2, H - 100, under, 17, game.error.empty() ? kColTextDim : kColError);

        if (mode == Mode::Shelf) {
            struct Pill {
                const char* hint;
                const char* label;
            };
            const Pill pills[] = {{"A", "Play"}, {"X", "Flip"}, {"Y", "Options"}, {"L R", "Shelf"}};
            int total = 0;
            for (const Pill& p : pills) {
                total += PillWidth(font, p.hint, p.label) + 10;
            }
            int px = (W - (total - 10)) / 2;
            for (const Pill& p : pills) {
                px += DrawPill(c, font, px, bar_y - 17, p.hint, p.label, p.hint[0] == 'A') + 10;
            }
        }
    }
    // The bar's corners: the settings and the receiver on the left, the way out on the right.
    DrawRoundButton(c, font, Icon::Gear, 26 + radius, bar_y, radius, "-", mode == Mode::Settings || mode == Mode::Picker);
    DrawRoundButton(c, font, Icon::Link, 26 + radius + 2 * radius + 22, bar_y, radius, "ZL", false);
    DrawRoundButton(c, font, Icon::Power, W - 26 - radius, bar_y, radius, "+", false);
}

const Image* App::MadeCover(Game& game) {
    if (const auto it = made.find(game.path); it != made.end()) {
        return &it->second;
    }
    constexpr int W = 320, H = 520; // (a case's own shape)
    Canvas canvas;
    canvas.Resize(W, H);
    // The case takes its colour from the icon: what the icon is, on the whole. With no icon,
    // from the name, so that two such games don't look alike.
    u32 sum[3] = {0x50, 0x58, 0x64};
    if (!game.icon.empty()) {
        u64 total[3] = {0, 0, 0};
        for (const u32 px : game.icon) {
            total[0] += px & 0xFF;
            total[1] += (px >> 8) & 0xFF;
            total[2] += (px >> 16) & 0xFF;
        }
        for (int k = 0; k < 3; ++k) {
            sum[k] = static_cast<u32>(total[k] / game.icon.size());
        }
    } else {
        u32 hash = 2166136261u;
        for (const char ch : game.title) {
            hash = (hash ^ static_cast<u8>(ch)) * 16777619u;
        }
        sum[0] = 0x40 + (hash & 0x7F);
        sum[1] = 0x40 + ((hash >> 8) & 0x7F);
        sum[2] = 0x40 + ((hash >> 16) & 0x7F);
    }
    const u32 base = MakeColor(static_cast<u8>(sum[0]), static_cast<u8>(sum[1]), static_cast<u8>(sum[2]));
    FillGradientV(canvas, 0, 0, W, H, MixColor(base, MakeColor(0, 0, 0), 0.30f), MixColor(base, MakeColor(0, 0, 0), 0.78f));
    constexpr int side = 256, top = 40;
    if (!game.icon.empty()) {
        canvas.BlitIcon(game.icon, game.icon_size, (W - side) / 2, top, side);
    } else {
        FillDisc(canvas, W / 2.0f, top + side / 2.0f, side * 0.42f, MixColor(base, MakeColor(255, 255, 255), 0.18f));
        const std::string initial = game.title.substr(0, game.title.empty() ? 0 : 1);
        font.Draw(canvas, (W - font.Measure(initial, 120)) / 2, top + side / 2 + 42, initial, 120, MakeColor(255, 255, 255));
    }
    std::vector<std::string> lines = WrapLines(font, game.title, 27, W - 40);
    if (lines.size() > 3) {
        lines.resize(3);
        lines.back() = font.Truncate(lines.back() + " ...", 27, W - 40);
    }
    int y = top + side + 54;
    for (const std::string& line : lines) {
        font.Draw(canvas, (W - font.Measure(line, 27)) / 2, y, line, 27, MakeColor(255, 255, 255));
        y += 34;
    }
    const std::string publisher = font.Truncate(game.publisher, 16, W - 40);
    font.Draw(canvas, (W - font.Measure(publisher, 16)) / 2, H - 26, publisher, 16, MakeColor(255, 255, 255, 0xB0));
    Image image;
    image.w = W;
    image.h = H;
    image.px.assign(canvas.Data(), canvas.Data() + static_cast<std::size_t>(W) * H);
    return &made.emplace(game.path, std::move(image)).first->second;
}

void App::DrawRows(Canvas& c, int x, int y, int w, const std::vector<Row>& rows, int cursor, int visible) {
    const int count = static_cast<int>(rows.size());
    if (visible <= 0 || visible > count) {
        visible = count;
    }
    const int first = std::clamp(cursor - visible / 2, 0, count - visible);
    for (int k = 0; k < visible; ++k) {
        const int index = first + k;
        const Row& row = rows[index];
        const int ry = y + k * (kRowH + kRowGap);
        const bool focused = index == cursor;
        if (focused) {
            DrawFocusRing(c, x, ry, w, kRowH, 10, time, kColSurfaceHi);
        } else {
            c.FillRoundRect(x, ry, w, kRowH, 10, kColSurface);
        }
        int lx = x + 22;
        if (row.check) {
            DrawIcon(c, Icon::Check, lx + 9.0f, ry + kRowH / 2.0f, 20.0f, kColAccent);
            lx += 30;
        }
        const int value_w = font.Measure(row.value, 19);
        const int side = row.arrows ? 64 : 22;
        // The label has what the value leaves; a value that would leave too little is cut from its front.
        const int label_room = std::max(160, w - (lx - x) - 40 - side - std::min(value_w, w / 2));
        const std::string label = font.Truncate(row.label, 19, label_room);
        font.Draw(c, lx, CenterBaseline(ry, kRowH, 19), label, 19, kColText);
        const int room = w - (lx - x) - font.Measure(label, 19) - 40 - side;
        const std::string value = font.TruncateFront(row.value, 19, std::max(60, room));
        const int vw = font.Measure(value, 19);
        int vx = x + w - 22 - vw;
        if (row.arrows) {
            vx -= 30;
            const u32 arrow = focused ? kColText : kColTextDim;
            font.Draw(c, vx - 26, CenterBaseline(ry, kRowH, 19), "<", 19, arrow);
            font.Draw(c, x + w - 22 - font.Measure(">", 19), CenterBaseline(ry, kRowH, 19), ">", 19, arrow);
        }
        font.Draw(c, vx, CenterBaseline(ry, kRowH, 19), value, 19, kColAccent);
    }
    if (visible < count) {
        // More rows than fit: a bar at the right says where in the list we are.
        const int track_h = visible * (kRowH + kRowGap) - kRowGap;
        const int thumb_h = std::max(24, track_h * visible / count);
        const int thumb_y = y + (track_h - thumb_h) * first / (count - visible);
        c.FillRoundRect(x + w + 9, y, 4, track_h, 2, kColSurface);
        c.FillRoundRect(x + w + 9, thumb_y, 4, thumb_h, 2, kColTextDim);
    }
}

void App::DrawHelp(Canvas& c, int x, int y, int w, const std::string& text) {
    for (const std::string& line : WrapLines(font, text, 16, w)) {
        font.Draw(c, x, y, line, 16, kColTextDim);
        y += 21;
    }
}

void App::DrawHints(Canvas& c, const std::vector<std::pair<const char*, const char*>>& hints) {
    int total = 0;
    for (const auto& [hint, label] : hints) {
        total += PillWidth(font, hint, label) + 10;
    }
    int px = (c.Width() - (total - 10)) / 2;
    const int y = c.Height() - 22 - 30 - 17;
    for (const auto& [hint, label] : hints) {
        px += DrawPill(c, font, px, y, hint, label, false) + 10;
    }
}

void App::DrawGameHead(Canvas& c, int x, int y, int w, const Game& game, const char* section) {
    c.FillRoundRect(x, y, w, 96, 16, kColRail);
    c.FillRect(x, y + 80, w, 16, kColRail);
    c.FillRect(x, y + 96, w, 1, kColLine);
    // The game: its icon, its name, and what the file says of it (or which of its pages this is).
    int tx = x + 28;
    if (!game.icon.empty()) {
        c.BlitIcon(game.icon, game.icon_size, x + 24, y + 16, 64);
        tx = x + 24 + 64 + 18;
    }
    font.Draw(c, tx, y + 44, font.Truncate(game.title, 26, x + w - 28 - tx), 26, kColText);
    std::string under = section ? section : game.publisher;
    if (!section) {
        for (const std::string& more : {game.version.empty() ? std::string{} : "v" + game.version, game.note}) {
            if (!more.empty()) {
                under += (under.empty() ? "" : "   ") + more;
            }
        }
    }
    font.Draw(c, tx, y + 72, font.Truncate(under, 17, x + w - 28 - tx), 17, section ? kColAccent : kColTextDim);
}

void App::DrawSheet(Canvas& c) {
    const Game* game = Selected();
    if (!game) {
        return;
    }
    Dim(c);
    const int W = c.Width();
    const int w = 860, x = (W - w) / 2, y = 40, h = 590;
    c.FillRoundRect(x, y, w, h, 16, kColBg);
    DrawGameHead(c, x, y, w, *game, nullptr);

    const std::vector<Row> rows = SheetRows();
    const int rows_y = y + 96 + 18;
    DrawRows(c, x + 28, rows_y, w - 56, rows, sheet_cursor);
    if (sheet_cursor < static_cast<int>(rows.size())) {
        DrawHelp(c, x + 34, rows_y + static_cast<int>(rows.size()) * (kRowH + kRowGap) + 12, w - 68, rows[sheet_cursor].help);
    }

    // Where the game's things are on the card.
    c.FillRect(x + 28, y + h - 160, w - 56, 1, kColLine);
    const std::string folder = game->Folder();
    const auto there = [&](const std::string& path, const char* yes, const char* no) {
        return path + "   " + (Exists(path) ? yes : no);
    };
    const std::pair<const char*, std::string> facts[] = {
        {"Folder", folder + "/"},
        {"Log", there(folder + "/log.txt", "", "(none yet)")},
        {"Crash report", there(folder + "/crash.txt", "(a run ended badly: the file to send with a report)", "(none)")},
        {"Saved data", there(SlotFolder(*game, SlotInUse()), "", "(none yet)")},
        {"File", game->path + "   " + HumanSize(game->size)},
    };
    int fy = y + h - 134;
    for (const auto& [label, value] : facts) {
        font.Draw(c, x + 34, fy, label, 16, kColTextDim);
        font.Draw(c, x + 34 + 130, fy, font.TruncateFront(value, 16, w - 68 - 130), 16, kColText);
        fy += 26;
    }
    const bool opens = sheet_cursor == kSheetSaves || sheet_cursor == kSheetAdvanced;
    DrawHints(c, {opens ? std::pair{"A", "Open"} : std::pair{"< >", "Change"}, {"B", "Back"}});
}

void App::DrawAdvanced(Canvas& c) {
    const Game* game = Selected();
    if (!game) {
        return;
    }
    Dim(c);
    const int W = c.Width();
    const int w = 860, x = (W - w) / 2, y = 40, h = 590;
    c.FillRoundRect(x, y, w, h, 16, kColBg);
    DrawGameHead(c, x, y, w, *game, "Advanced options");
    int top = y + 96 + 18;
    top += DrawBanner(c, font, x + 28, top, w - 56, "These can keep a game from starting",
                      "Change them only if you know what they do. A game that no longer starts is put right by setting both back to "
                      "automatic.") +
           18;
    const std::vector<Row> rows = AdvancedRows();
    DrawRows(c, x + 28, top, w - 56, rows, advanced_cursor);
    DrawHelp(c, x + 34, top + static_cast<int>(rows.size()) * (kRowH + kRowGap) + 12, w - 68, rows[advanced_cursor].help);
    font.Draw(c, x + 34, y + h - 24, font.TruncateFront("Written to " + game->Folder() + "/game.cfg", 16, w - 68), 16, kColTextDim);
    DrawHints(c, {{"< >", "Change"}, {"X", "Automatic"}, {"B", "Back"}});
}

void App::DrawSaves(Canvas& c) {
    const Game* game = Selected();
    if (!game) {
        return;
    }
    Dim(c);
    const int W = c.Width();
    const int w = 860, x = (W - w) / 2, y = 40, h = 590;
    c.FillRoundRect(x, y, w, h, 16, kColBg);
    DrawGameHead(c, x, y, w, *game, "Saved data");
    const std::vector<Row> rows = SaveRows();
    const int rows_y = y + 96 + 18;
    DrawRows(c, x + 28, rows_y, w - 56, rows, saves_cursor, kSavesVisible);
    const int shown = std::min(static_cast<int>(rows.size()), kSavesVisible);
    if (saves_cursor < static_cast<int>(rows.size())) {
        DrawHelp(c, x + 34, rows_y + shown * (kRowH + kRowGap) + 12, w - 68, rows[saves_cursor].help);
    }
    font.Draw(c, x + 34, y + h - 24, "The slot in use is the one the game reads and writes the next time it runs.", 16, kColTextDim);
    if (saves_cursor < static_cast<int>(save_slots.size())) {
        DrawHints(c, {{"A", "Use this slot"}, {"X", "Delete"}, {"B", "Back"}});
    } else {
        DrawHints(c, {{"A", "Make it"}, {"B", "Back"}});
    }
}

void App::DrawSettings(Canvas& c) {
    Dim(c);
    const int W = c.Width();
    const int w = 860, x = (W - w) / 2, y = 34, h = 590;
    c.FillRoundRect(x, y, w, h, 16, kColBg);
    font.Draw(c, x + 28, y + 46, "Settings", 26, kColText);
    const std::string version = std::string{kName} + " " + kVersion;
    font.Draw(c, x + w - 28 - font.Measure(version, 16), y + 44, version, 16, kColTextDim);
    c.FillRect(x + 28, y + 64, w - 56, 1, kColLine);

    const std::vector<Row> rows = SettingsRows();
    const int rows_y = y + 80;
    DrawRows(c, x + 28, rows_y, w - 56, rows, mode == Mode::Settings ? settings_cursor : -1, kSettingsVisible);
    if (settings_cursor < static_cast<int>(rows.size())) {
        DrawHelp(c, x + 34, rows_y + kSettingsVisible * (kRowH + kRowGap) + 16, w - 68, rows[settings_cursor].help);
    }
    const std::string loader = std::string{"Game loader   "} + kGameNro + (Exists(kGameNro) ? "" : "   (NOT THERE: games can't be started)");
    font.Draw(c, x + 34, y + h - 22, font.TruncateFront(loader, 16, w - 68), 16, Exists(kGameNro) ? kColTextDim : kColError);
    if (mode == Mode::Settings) {
        DrawHints(c, {{"< >", "Change"}, {"A", "Open"}, {"B", "Back"}});
    }
}

void App::DrawPicker(Canvas& c) {
    Dim(c);
    const int W = c.Width();
    const int w = 760, x = (W - w) / 2, y = 60, h = 540;
    c.FillRoundRect(x, y, w, h, 16, kColBg);
    font.Draw(c, x + 28, y + 44, "Games folder", 24, kColText);
    const std::string here = picker_path.empty() ? "/" : picker_path;
    font.Draw(c, x + 28, y + 74, font.TruncateFront(here, 18, w - 56), 18, kColAccent);
    const std::string found = std::to_string(picker_nups) + (picker_nups == 1 ? " NUP file here" : " NUP files here");
    font.Draw(c, x + w - 28 - font.Measure(found, 16), y + 44, found, 16, picker_nups ? kColText : kColTextDim);
    c.FillRect(x + 28, y + 90, w - 56, 1, kColLine);

    const int count = static_cast<int>(picker_folders.size());
    const int first = std::clamp(picker_cursor - kPickerRows / 2, 0, std::max(0, count - kPickerRows));
    const int row_h = 44;
    for (int k = 0; k < kPickerRows && first + k < count; ++k) {
        const int index = first + k;
        const int ry = y + 104 + k * (row_h + 6);
        if (index == picker_cursor) {
            DrawFocusRing(c, x + 28, ry, w - 56, row_h, 9, time, kColSurfaceHi);
        } else {
            c.FillRoundRect(x + 28, ry, w - 56, row_h, 9, kColSurface);
        }
        font.Draw(c, x + 50, CenterBaseline(ry, row_h, 18), font.Truncate(picker_folders[index], 18, w - 120), 18, kColText);
    }
    if (count == 0) {
        font.Draw(c, x + 34, y + 140, "No folders in this one.", 18, kColTextDim);
    } else if (count > kPickerRows) {
        const std::string more = std::to_string(picker_cursor + 1) + " of " + std::to_string(count);
        font.Draw(c, x + w - 28 - font.Measure(more, 15), y + h - 18, more, 15, kColTextDim);
    }
    DrawHints(c, {{"A", "Open"}, {"X", "Use this folder"}, {"B", picker_path.empty() ? "Cancel" : "Up"}});
}

void App::DrawTransfer(Canvas& c) {
    const Transfer::State state = Transfer::Look();
    const int W = c.Width();
    const int w = 1140, x = (W - w) / 2, y = 30, h = 600;
    c.FillRoundRect(x, y, w, h, 16, kColBg);
    font.Draw(c, x + 28, y + 46, "Receive files from a computer", 26, kColText);
    const std::string version = std::string{kName} + " " + kVersion;
    font.Draw(c, x + w - 28 - font.Measure(version, 16), y + 44, version, 16, kColTextDim);
    c.FillRect(x + 28, y + 64, w - 56, 1, kColLine);

    // Where to send: the one thing the person at the computer has to read off this screen.
    if (address.empty()) {
        font.Draw(c, x + 28, y + 118, "This console is on no network", 30, kColError);
        font.Draw(c, x + 28, y + 150, "Connect it to one in the console's settings: its address then shows here.", 17, kColTextDim);
    } else {
        font.Draw(c, x + 28, y + 92, "Send to", 16, kColTextDim);
        font.Draw(c, x + 28, y + 140, address, 44, kColAccent);
        const int after = x + 28 + font.Measure(address, 44) + 18;
        font.Draw(c, after, y + 140, "port " + std::to_string(Transfer::kPort), 20, kColTextDim);
        const char* how[] = {"The program that sends to Toronja Transfer sends here:", "the same port, the same commands."};
        for (int k = 0; k < 2; ++k) {
            font.Draw(c, x + w - 28 - font.Measure(how[k], 16), y + 112 + k * 22, how[k], 16, kColTextDim);
        }
    }

    // What is happening.
    std::string doing = !state.running ? "Not listening" : !state.listening ? "Opening the port..." : "Waiting for a computer...";
    if (state.busy) {
        doing = state.current;
    } else if (state.connected) {
        doing = "A computer is connected";
    }
    font.Draw(c, x + 28, y + 196, font.TruncateFront(doing, 20, w - 56), 20, kColText);
    const int bar_x = x + 28, bar_y = y + 212, bar_w = w - 56, bar_h = 16;
    c.FillRoundRect(bar_x, bar_y, bar_w, bar_h, bar_h / 2, kColSurface);
    if (state.busy && state.total > 0) {
        const double part = std::min(1.0, static_cast<double>(state.done) / static_cast<double>(state.total));
        const int filled = std::max(bar_h, static_cast<int>(bar_w * part));
        c.FillRoundRect(bar_x, bar_y, filled, bar_h, bar_h / 2, kColAccent);
        const std::string amount = HumanSize(state.done) + " of " + HumanSize(state.total) + "   " + std::to_string(static_cast<int>(part * 100)) + "%";
        font.Draw(c, bar_x, bar_y + 42, amount, 17, kColText);
        const std::string pace = Megabytes(transfer_speed) + "/s";
        font.Draw(c, bar_x + bar_w - font.Measure(pace, 17), bar_y + 42, pace, 17, kColText);
        // How many blocks wait for the card: full means the card is what holds the pace.
        const std::string waiting = "Waiting for the card: " + std::to_string(state.queue_now) + " of " + std::to_string(state.queue_size) + " blocks";
        const int mid = bar_x + (bar_w - font.Measure(waiting, 16)) / 2;
        font.Draw(c, mid, bar_y + 42, waiting, 16, kColTextDim);
    }

    // The session, and how the receiver is set.
    const std::string session = "Since the menu was opened: " + Count(state.files, "file", "files") + ", " + HumanSize(state.bytes);
    font.Draw(c, x + 28, y + 292, session, 16, kColTextDim);
    const std::string set = "Blocks of " + std::to_string(transfer_block_kb) + " KB, a queue of " + std::to_string(transfer_queue) + ", " +
                            (transfer_kbps > 0 ? "at most " + CapText(transfer_kbps) : std::string{"no limit"});
    font.Draw(c, x + w - 28 - font.Measure(set, 16), y + 292, set, 16, kColTextDim);
    c.FillRect(x + 28, y + 306, w - 56, 1, kColLine);

    int ly = y + 334;
    for (const std::string& line : state.lines) {
        font.Draw(c, x + 28, ly, font.Truncate(line, 15, w - 56), 15, kColText);
        ly += 25;
    }
    font.Draw(c, x + 28, y + h - 20, "Every large file's figures are kept in /ns2LOAD/transfer.log; the receiver's numbers are in /ns2LOAD/menu.cfg.", 15,
              kColTextDim);
    DrawHints(c, {{"B", "Back"}});
}

void App::DrawAsk(Canvas& c) {
    Dim(c);
    const int W = c.Width(), H = c.Height();
    const int w = 680;
    const std::vector<std::string> lines = WrapLines(font, ask_text, 18, w - 64);
    const int h = 84 + static_cast<int>(lines.size()) * 25 + 76;
    const int x = (W - w) / 2, y = (H - h) / 2;
    c.FillRoundRect(x - 2, y - 2, w + 4, h + 4, 18, ask_warning ? kColWarnText : kColLine);
    c.FillRoundRect(x, y, w, h, 16, kColBg);
    int tx = x + 32;
    if (ask_warning) {
        DrawIcon(c, Icon::Warning, x + 46.0f, y + 44.0f, 28.0f, kColWarnText);
        tx += 38;
    }
    font.Draw(c, tx, y + 54, font.Truncate(ask_title, 24, x + w - 32 - tx), 24, ask_warning ? kColWarnText : kColText);
    int ty = y + 94;
    for (const std::string& line : lines) {
        font.Draw(c, x + 32, ty, line, 18, kColText);
        ty += 25;
    }
    const int total = PillWidth(font, "A", ask_yes) + 12 + PillWidth(font, "B", ask_no);
    int px = x + (w - total) / 2;
    px += DrawPill(c, font, px, y + h - 56, "A", ask_yes, true) + 12;
    DrawPill(c, font, px, y + h - 56, "B", ask_no, false);
}

void App::DrawBusy(Canvas& c) {
    Dim(c);
    const int size = 22;
    const std::string text = font.Truncate(busy, size, c.Width() - 200);
    const int w = font.Measure(text, size) + 80, h = 90;
    const int x = (c.Width() - w) / 2, y = (c.Height() - h) / 2;
    c.FillRoundRect(x, y, w, h, 16, kColBg);
    font.Draw(c, x + 40, CenterBaseline(y, h, size), text, size, kColText);
}

void App::DrawNotice(Canvas& c) {
    if (notice_left <= 0.0f || notice.empty()) {
        return;
    }
    const int size = 18;
    const std::string text = font.Truncate(notice, size, c.Width() - 160);
    const int w = font.Measure(text, size) + 44, h = 44;
    // Under the top line on the shelf; over the bar of buttons when a panel is up (which has the top).
    const int x = (c.Width() - w) / 2, y = mode == Mode::Shelf ? 66 : c.Height() - 22 - 30 - 22;
    c.FillRoundRect(x, y, w, h, h / 2, notice_error ? kColWarnBg : kColSurfaceHi);
    font.Draw(c, x + 22, CenterBaseline(y, h, size), text, size, notice_error ? kColWarnText : kColText);
}

} // namespace Flow
