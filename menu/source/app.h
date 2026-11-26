// SPDX-License-Identifier: GPL-2.0-or-later

// The menu itself: the shelf of games, a game's options and its slots of saved data, the menu's
// settings, and the screen that brings files to the card from a computer. It draws on a plain canvas and is told what was pressed, so the same
// code runs on the console and renders the previews on a PC.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "library.h"

namespace Flow {

inline constexpr const char* kVersion = "0.2.2";

/// What was pressed since the last frame (a direction held repeats: the caller sees to it).
struct Input {
    bool a{}, b{}, x{}, y{};
    bool l{}, r{}, zl{}, zr{};
    bool plus{}, minus{};
    bool up{}, down{}, left{}, right{};
};

/// What the menu asks of its caller after a frame.
struct Request {
    enum class Kind { None, Exit, Launch } kind{Kind::None};
    std::string nup;  // Launch: the container, as a path of the card.
    std::string room; // Launch: game.cfg's "room" (MB), which the loader is told as an argument; empty when unset.
};

class App {
public:
    void Start();
    Request Update(const Input& in, float dt);
    void Draw(SwitchFrontend::Ui::Canvas& c);
    /// The settings as they stand, written if they changed (the caller is about to leave).
    void Finish();
    /// Seconds the caller should rest after showing a frame. Not zero while a file is moving:
    /// drawing takes the processor from the threads that move it.
    float FramePause() const;

private:
    enum class Mode { Shelf, Sheet, Advanced, Saves, Settings, Picker, Transfer };
    /// A question put to the user over whatever is on screen, and what a yes does.
    enum class Ask { None, OpenAdvanced, DeleteSlot, MakeSlot, LeaveTransfer };

    struct Row {
        std::string label;
        std::string value;
        std::string help;
        bool arrows{}; // Left and right change it.
        bool check{};  // Marked as the one in use.
    };

    void LoadSettings();
    void SaveSettings();
    void Rescan();
    void Notice(std::string text, bool error = false);
    Game* Selected();

    Request UpdateShelf(const Input& in);
    void UpdateSheet(const Input& in);
    void UpdateAdvanced(const Input& in);
    void UpdateSaves(const Input& in);
    void UpdateSettings(const Input& in);
    void UpdatePicker(const Input& in);
    void UpdateTransfer(const Input& in, float dt);
    void UpdateAsk(const Input& in);

    void OpenSheet();
    void OpenPicker();
    void PickerLook();
    void OpenTransfer();
    void LeaveTransfer();
    void PutQuestion(Ask what, std::string title, std::string text, std::string yes, std::string no, bool warning = false);

    std::vector<Row> SheetRows();
    std::vector<Row> AdvancedRows();
    std::vector<Row> SaveRows();
    std::vector<Row> SettingsRows();
    void ChangeSheet(int row, int dir, bool press);
    void ChangeAdvanced(int row, int dir);
    void ChangeSetting(int row, int dir, bool press);
    bool WriteGameCfg();

    // Slots of saved data.
    void LookAtSlots();
    std::string SlotInUse() const;
    std::string NextSlotName() const;
    void BeginNewSlot(bool copy);
    void MakeSlot(const std::string& name, bool copy);
    void FinishCopy();

    void DrawShelfScreen(SwitchFrontend::Ui::Canvas& c);
    void DrawBackdrop(SwitchFrontend::Ui::Canvas& c);
    void DrawGameHead(SwitchFrontend::Ui::Canvas& c, int x, int y, int w, const Game& game, const char* section);
    void DrawSheet(SwitchFrontend::Ui::Canvas& c);
    void DrawAdvanced(SwitchFrontend::Ui::Canvas& c);
    void DrawSaves(SwitchFrontend::Ui::Canvas& c);
    void DrawSettings(SwitchFrontend::Ui::Canvas& c);
    void DrawPicker(SwitchFrontend::Ui::Canvas& c);
    void DrawTransfer(SwitchFrontend::Ui::Canvas& c);
    void DrawAsk(SwitchFrontend::Ui::Canvas& c);
    void DrawBusy(SwitchFrontend::Ui::Canvas& c);
    /// `visible`: how many rows fit; more than that scroll, the focused one kept in sight.
    void DrawRows(SwitchFrontend::Ui::Canvas& c, int x, int y, int w, const std::vector<Row>& rows, int cursor, int visible = 0);
    void DrawHelp(SwitchFrontend::Ui::Canvas& c, int x, int y, int w, const std::string& text);
    void DrawHints(SwitchFrontend::Ui::Canvas& c, const std::vector<std::pair<const char*, const char*>>& hints);
    void DrawNotice(SwitchFrontend::Ui::Canvas& c);
    const SwitchFrontend::Ui::Image* MadeCover(Game& game);

    SwitchFrontend::Ui::Font font;
    Mode mode{Mode::Shelf};
    std::vector<Game> games;
    // A front made here for a game that has no cover (or when covers are hidden): its icon and
    // its name on a case's shape, by the container's path.
    std::unordered_map<std::string, SwitchFrontend::Ui::Image> made;
    float time{};

    // The settings (menu.cfg).
    std::string games_folder{kDefaultGames};
    int style{};
    bool covers_shown{true};
    bool cases{true};
    bool light{};
    int transfer_kbps{};
    int transfer_block_kb{1024};
    int transfer_queue{8};
    int transfer_rcvbuf_kb{256};
    bool settings_changed{};

    // The shelf.
    int selected{};
    float position{};
    bool flipped{};
    float flip{};

    // A game's options, its advanced ones, and its slots of saved data.
    int sheet_cursor{};
    int advanced_cursor{};
    Settings game_cfg;
    std::vector<std::string> mod_sets;
    std::vector<std::string> save_slots; // "default" first.
    int saves_cursor{};
    // A copy of saved data that is about to be made: done on the frame after the one that
    // says so, since it can take a while and nothing is drawn meanwhile.
    std::string copy_from, copy_to, copy_name;
    std::string busy;

    // The question on screen, if any.
    Ask ask{Ask::None};
    std::string ask_title, ask_text, ask_yes, ask_no;
    bool ask_warning{};
    std::string ask_name; // The slot it is about.
    bool ask_copy{};

    // The settings page and its folder picker.
    int settings_cursor{};
    std::string picker_path;
    std::vector<std::string> picker_folders;
    int picker_nups{};
    int picker_cursor{};

    // Files from a computer.
    Mode transfer_back{Mode::Shelf};
    std::string address;
    float address_age{};
    std::uint32_t transfer_files_before{};
    std::uint64_t transfer_seen{};
    float transfer_speed{}; // Bytes a second, smoothed.
    float transfer_since{};

    std::string notice;
    bool notice_error{};
    float notice_left{};
};

} // namespace Flow
