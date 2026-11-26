// SPDX-License-Identifier: GPL-2.0-or-later

#include "library.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "images.h"
#include "nup.h"

namespace Flow {
namespace {

std::string g_root = "sdmc:";

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

bool EndsWith(const std::string& s, const char* tail) {
    const std::size_t n = std::strlen(tail);
    return s.size() >= n && Lower(s.substr(s.size() - n)) == tail;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    std::size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) {
        ++a;
    }
    return s.substr(a);
}

std::vector<std::string> Split(const std::string& line, char by) {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t next = line.find(by, at);
        out.push_back(line.substr(at, next == std::string::npos ? next : next - at));
        if (next == std::string::npos) {
            break;
        }
        at = next + 1;
    }
    return out;
}

/// A line's text with nothing in it that would end the line or a field.
std::string Flat(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return s;
}

std::string Hex16(std::uint64_t v) {
    char text[20];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}

} // namespace

const std::string& CardRoot() {
    return g_root;
}

void SetCardRoot(std::string root) {
    g_root = std::move(root);
}

std::string OnCard(const std::string& card_path) {
    return g_root + card_path;
}

bool Exists(const std::string& card_path) {
    struct stat st;
    return stat(OnCard(card_path).c_str(), &st) == 0;
}

bool IsFolder(const std::string& card_path) {
    struct stat st;
    return stat(OnCard(card_path.empty() ? "/" : card_path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void MakeFolders(const std::string& card_path) {
    for (std::size_t at = 1; at <= card_path.size(); ++at) {
        if (at == card_path.size() || card_path[at] == '/') {
            mkdir(OnCard(card_path.substr(0, at)).c_str(), 0777);
        }
    }
}

std::vector<std::string> ListFolders(const std::string& card_path, std::vector<std::string>* files) {
    std::vector<std::string> folders;
    const std::string base = OnCard(card_path.empty() ? "/" : card_path);
    DIR* dir = opendir(base.c_str());
    if (!dir) {
        return folders;
    }
    while (const dirent* e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        struct stat st;
        const std::string full = base + (base.back() == '/' ? "" : "/") + name;
        if (stat(full.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            folders.push_back(name);
        } else if (files) {
            files->push_back(name);
        }
    }
    closedir(dir);
    const auto by_name = [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); };
    std::sort(folders.begin(), folders.end(), by_name);
    if (files) {
        std::sort(files->begin(), files->end(), by_name);
    }
    return folders;
}

Settings ReadSettings(const std::string& card_path) {
    Settings out;
    std::FILE* f = std::fopen(OnCard(card_path).c_str(), "rb");
    if (!f) {
        return out;
    }
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        const std::string text = Trim(line);
        const std::size_t eq = text.find('=');
        if (text.empty() || text[0] == '#' || eq == std::string::npos) {
            continue;
        }
        out[Trim(text.substr(0, eq))] = Trim(text.substr(eq + 1));
    }
    std::fclose(f);
    return out;
}

bool WriteSettings(const std::string& card_path, const Settings& values) {
    const std::size_t slash = card_path.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        MakeFolders(card_path.substr(0, slash));
    }
    std::FILE* f = std::fopen(OnCard(card_path).c_str(), "wb");
    if (!f) {
        return false;
    }
    for (const auto& [key, value] : values) {
        if (!value.empty()) {
            std::fprintf(f, "%s=%s\n", key.c_str(), value.c_str());
        }
    }
    return std::fclose(f) == 0;
}

std::string HumanSize(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) {
        std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    } else if (bytes >= (1ull << 20)) {
        std::snprintf(text, sizeof text, "%u MB", static_cast<unsigned>(bytes >> 20));
    } else {
        std::snprintf(text, sizeof text, "%u KB", static_cast<unsigned>((bytes + 1023) >> 10));
    }
    return text;
}

std::vector<Game> ScanGames(const std::string& games_folder) {
    std::vector<Game> games;
    std::vector<std::string> files;
    ListFolders(games_folder, &files);

    // What was read before: a line for each container (name, size, time, then what it said).
    const std::string cache_path = std::string{kCache} + "/library.txt";
    std::map<std::string, std::vector<std::string>> known;
    if (std::FILE* f = std::fopen(OnCard(cache_path).c_str(), "rb")) {
        char line[2048];
        while (std::fgets(line, sizeof line, f)) {
            std::vector<std::string> fields = Split(Trim(line), '\t');
            if (fields.size() >= 9) {
                known[fields[0]] = std::move(fields);
            }
        }
        std::fclose(f);
    }

    bool changed = false;
    std::vector<std::vector<std::string>> lines;
    for (const std::string& file : files) {
        if (!EndsWith(file, ".nup")) {
            continue;
        }
        Game game;
        game.file = file;
        game.path = games_folder + "/" + file;
        struct stat st;
        if (stat(OnCard(game.path).c_str(), &st) != 0) {
            continue;
        }
        game.size = static_cast<std::uint64_t>(st.st_size);
        game.mtime = static_cast<std::int64_t>(st.st_mtime);
        const std::string size_text = std::to_string(game.size), time_text = std::to_string(game.mtime);
        const auto it = known.find(game.path);
        if (it != known.end() && it->second[1] == size_text && it->second[2] == time_text) {
            const std::vector<std::string>& k = it->second;
            game.id = k[3];
            game.title_id = std::strtoull(k[3].c_str(), nullptr, 16);
            game.title = k[4];
            game.publisher = k[5];
            game.version = k[6];
            game.note = k[7];
            game.error = k[8];
        } else {
            changed = true;
            Nup::Info info;
            std::string error;
            if (Nup::Read(OnCard(game.path), info, error)) {
                game.title_id = info.title_id;
                game.id = Hex16(info.title_id);
                game.title = Flat(info.title);
                game.publisher = Flat(info.publisher);
                game.version = Flat(info.display_version);
                if (info.updates != 0) {
                    game.note = std::to_string(info.updates) + (info.updates == 1 ? " update" : " updates");
                }
                if (info.addons != 0) {
                    game.note += (game.note.empty() ? "" : ", ") + std::to_string(info.addons) +
                                 (info.addons == 1 ? " added content" : " added contents");
                }
                if (!info.icon.empty()) {
                    MakeFolders(kCache);
                    if (std::FILE* out = std::fopen(OnCard(std::string{kCache} + "/" + game.id + ".jpg").c_str(), "wb")) {
                        std::fwrite(info.icon.data(), 1, info.icon.size(), out);
                        std::fclose(out);
                    }
                }
            } else {
                game.error = error;
            }
        }
        if (game.title.empty()) {
            game.title = file.substr(0, file.size() - 4); // (the file's own name, without ".nup")
        }
        lines.push_back({game.path, size_text, time_text, game.id, game.title, game.publisher, game.version, game.note, game.error});
        games.push_back(std::move(game));
    }
    if (changed || lines.size() != known.size()) {
        MakeFolders(kCache);
        if (std::FILE* f = std::fopen(OnCard(cache_path).c_str(), "wb")) {
            for (const auto& fields : lines) {
                for (std::size_t k = 0; k < fields.size(); ++k) {
                    std::fprintf(f, "%s%s", k ? "\t" : "", Flat(fields[k]).c_str());
                }
                std::fputc('\n', f);
            }
            std::fclose(f);
        }
    }
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        const std::string x = Lower(a.title), y = Lower(b.title);
        return x != y ? x < y : a.file < b.file;
    });
    return games;
}

void LoadIcon(Game& game, int& budget) {
    if (game.icon_tried || game.id.empty() || budget <= 0) {
        return;
    }
    game.icon_tried = true;
    --budget;
    SwitchFrontend::Ui::Image image;
    if (Images::LoadFile(OnCard(std::string{kCache} + "/" + game.id + ".jpg"), image, 256) && image.w == image.h) {
        game.icon_size = image.w;
        game.icon = std::move(image.px);
    }
}

std::vector<std::string> ModSets(const Game& game) {
    return ListFolders(game.Folder() + "/modsets");
}

std::vector<std::string> SaveSlots(const Game& game) {
    return ListFolders(game.Folder() + "/saves");
}

std::string SlotFolder(const Game& game, const std::string& slot) {
    return slot.empty() || slot == "default" ? game.Folder() + "/save" : game.Folder() + "/saves/" + slot;
}

std::string CleanSlotName(const std::string& typed) {
    std::string name;
    for (const char ch : typed) {
        const bool fine = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' ||
                          ch == '_' || ch == '.';
        if (!fine || name.size() >= 32 || (ch == ' ' && !name.empty() && name.back() == ' ')) {
            continue; // (what is left out may leave two spaces together: one is kept)
        }
        name += ch;
    }
    // (a folder's name neither begins nor ends with a space or a dot on this kind of card)
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    std::size_t first = 0;
    while (first < name.size() && (name[first] == ' ' || name[first] == '.')) {
        ++first;
    }
    name = name.substr(first);
    return Lower(name) == "default" ? std::string{} : name;
}

bool CopyFolder(const std::string& from_card_path, const std::string& to_card_path, std::string& error) {
    MakeFolders(to_card_path);
    std::vector<std::string> files;
    const std::vector<std::string> folders = ListFolders(from_card_path, &files);
    std::vector<char> block(1 << 20);
    for (const std::string& name : files) {
        std::FILE* in = std::fopen(OnCard(from_card_path + "/" + name).c_str(), "rb");
        if (!in) {
            error = name + " couldn't be read";
            return false;
        }
        std::FILE* out = std::fopen(OnCard(to_card_path + "/" + name).c_str(), "wb");
        if (!out) {
            std::fclose(in);
            error = name + " couldn't be written";
            return false;
        }
        bool ok = true;
        for (;;) {
            const std::size_t got = std::fread(block.data(), 1, block.size(), in);
            if (got == 0) {
                ok = std::ferror(in) == 0;
                break;
            }
            if (std::fwrite(block.data(), 1, got, out) != got) {
                ok = false;
                break;
            }
        }
        std::fclose(in);
        if (std::fclose(out) != 0 || !ok) {
            error = name + " couldn't be copied (is the card full?)";
            return false;
        }
    }
    for (const std::string& name : folders) {
        if (!CopyFolder(from_card_path + "/" + name, to_card_path + "/" + name, error)) {
            return false;
        }
    }
    return true;
}

bool RemoveFolder(const std::string& card_path) {
    std::vector<std::string> files;
    const std::vector<std::string> folders = ListFolders(card_path, &files);
    bool ok = true;
    for (const std::string& name : files) {
        ok = std::remove(OnCard(card_path + "/" + name).c_str()) == 0 && ok;
    }
    for (const std::string& name : folders) {
        ok = RemoveFolder(card_path + "/" + name) && ok;
    }
    return rmdir(OnCard(card_path).c_str()) == 0 && ok;
}

} // namespace Flow
