// SPDX-License-Identifier: GPL-2.0-or-later

#include "covers.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "images.h"
#include "platform.h"

namespace Covers {
namespace {

std::unordered_map<std::string, std::shared_ptr<const Art>> g_cache; // By title id; null: none.
Flow::Settings g_codes;
bool g_codes_read = false;

std::mutex g_status_mutex;
std::string g_status;
std::atomic<bool> g_downloading{false};
std::atomic<bool> g_changed{false};

std::string Upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return s;
}

void SetStatus(std::string text) {
    std::lock_guard lock{g_status_mutex};
    g_status = std::move(text);
}

/// The names a game's cover may have, the likeliest first.
std::vector<std::string> NamesFor(const Flow::Game& game) {
    std::vector<std::string> names;
    if (const std::string code = CodeFor(game); !code.empty()) {
        names.push_back(code);
    }
    if (!game.id.empty()) {
        names.push_back(game.id);
        names.push_back(Upper(game.id));
    }
    return names;
}

/// The file of a game's cover as a path of the card, and whether it is a whole wrap.
bool FindFile(const Flow::Game& game, std::string& path, bool& wrap) {
    for (const bool full : {true, false}) {
        for (const std::string& name : NamesFor(game)) {
            for (const char* extension : {".jpg", ".png", ".jpeg"}) {
                const std::string candidate = std::string{Flow::kCovers} + (full ? "/full/" : "/") + name + extension;
                if (Flow::Exists(candidate)) {
                    path = candidate;
                    wrap = full;
                    return true;
                }
            }
        }
    }
    return false;
}

/// GameTDB files its art by region; the last letter of the code says which to try first.
std::vector<const char*> RegionsFor(const std::string& code) {
    switch (code.empty() ? ' ' : code.back()) {
    case 'J':
        return {"JA", "US", "EN"};
    case 'P':
    case 'X':
    case 'Y':
    case 'Z':
    case 'D':
    case 'F':
    case 'S':
    case 'I':
        return {"EN", "US", "JA"};
    case 'K':
        return {"KO", "US", "EN"};
    default:
        return {"US", "EN", "JA"};
    }
}

struct Request {
    std::string code;
};

void DownloadThread(std::vector<Request> requests) {
    Flow::MakeFolders(std::string{Flow::kCovers} + "/full");
    struct Kind {
        const char* folder; // On GameTDB.
        bool wrap;
    };
    // The whole wrap if GameTDB has it, the front otherwise.
    const Kind kinds[] = {{"coverfull", true}, {"cover", false}};
    int fetched = 0, missing = 0;
    std::string last_error;
    for (std::size_t k = 0; k < requests.size(); ++k) {
        const std::string& code = requests[k].code;
        SetStatus("Downloading covers: " + std::to_string(k + 1) + " of " + std::to_string(requests.size()) + " (" + code + ")");
        bool got = false;
        for (const Kind& kind : kinds) {
            for (const char* region : RegionsFor(code)) {
                // (plainly: the site gives its pictures that way too, and the console then needs no secure connection)
                const std::string url = std::string{"http://art.gametdb.com/switch/"} + kind.folder + "/" + region + "/" + code + ".jpg";
                const std::string path = std::string{Flow::kCovers} + (kind.wrap ? "/full/" : "/") + code + ".jpg";
                std::string error;
                if (Platform::Download(url, Flow::OnCard(path), error)) {
                    got = true;
                    break;
                }
                if (!error.empty()) {
                    last_error = error;
                }
            }
            if (got) {
                break;
            }
        }
        got ? ++fetched : ++missing;
    }
    std::string summary = std::to_string(fetched) + (fetched == 1 ? " cover" : " covers") + " downloaded";
    if (missing != 0) {
        summary += ", " + std::to_string(missing) + " not found";
        if (fetched == 0 && !last_error.empty()) {
            summary += " (" + last_error + ")";
        }
    }
    SetStatus(summary);
    if (fetched != 0) {
        g_changed = true;
    }
    g_downloading = false;
}

} // namespace

std::string CodeFor(const Flow::Game& game) {
    if (!g_codes_read) {
        g_codes_read = true;
        g_codes = Flow::ReadSettings(std::string{Flow::kCovers} + "/ids.txt");
    }
    for (const std::string& key : {game.id, Upper(game.id)}) {
        if (const auto it = g_codes.find(key); it != g_codes.end()) {
            return it->second;
        }
    }
    return {};
}

bool HasFile(const Flow::Game& game) {
    std::string path;
    bool wrap = false;
    return FindFile(game, path, wrap);
}

std::shared_ptr<const Art> Get(const Flow::Game& game, int& budget) {
    if (game.id.empty()) {
        return nullptr;
    }
    if (const auto it = g_cache.find(game.id); it != g_cache.end()) {
        return it->second;
    }
    if (budget <= 0) {
        return nullptr;
    }
    std::string path;
    bool wrap = false;
    std::shared_ptr<Art> art;
    if (FindFile(game, path, wrap)) {
        --budget;
        art = std::make_shared<Art>();
        art->full_wrap = wrap;
        // (a case on the shelf is some 400 pixels tall at its nearest: twice that is plenty)
        if (!Images::LoadFile(Flow::OnCard(path), art->image, wrap ? 1100 : 640)) {
            art.reset();
        }
    }
    g_cache[game.id] = art;
    return art;
}

void EnsureFolders() {
    Flow::MakeFolders(std::string{Flow::kCovers} + "/full");
    const std::string note = std::string{Flow::kCovers} + "/readme.txt";
    if (Flow::Exists(note)) {
        return;
    }
    if (std::FILE* f = std::fopen(Flow::OnCard(note).c_str(), "wb")) {
        std::fputs("Cover artwork for the games on the shelf.\n\n"
                   "  full/<ID>.jpg or .png   the whole wrap: back, spine and front.\n"
                   "  <ID>.jpg or .png        the front only.\n\n"
                   "<ID> is the game's title id (16 hex digits; the name of its folder beside this one),\n"
                   "or the code GameTDB files the game under. A NUP file has the title id and not that\n"
                   "code: ids.txt ties them, one line a game:\n\n"
                   "  <title id>=<GameTDB code>\n\n"
                   "The menu downloads the covers of the games listed there when the console has internet\n"
                   "access (Settings, Download covers), from art.gametdb.com/switch/coverfull/<region>/<code>.jpg\n"
                   "and .../cover/... A game with no cover shows its own icon.\n",
                   f);
        std::fclose(f);
    }
}

void Reset() {
    g_cache.clear();
    g_codes.clear();
    g_codes_read = false;
}

bool StartDownload(const std::vector<Flow::Game>& games) {
    if (g_downloading) {
        return false;
    }
    if (!Platform::HasNetwork()) {
        SetStatus("No internet access here: covers can be fetched on a computer (tools/fetch_covers.py)");
        return false;
    }
    std::vector<Request> requests;
    int unknown = 0;
    for (const Flow::Game& game : games) {
        if (game.id.empty() || HasFile(game)) {
            continue;
        }
        const std::string code = CodeFor(game);
        if (code.empty()) {
            ++unknown;
        } else {
            requests.push_back({code});
        }
    }
    if (requests.empty()) {
        SetStatus(unknown ? std::to_string(unknown) + (unknown == 1 ? " game has" : " games have") +
                                " no GameTDB code in covers/ids.txt: nothing to ask for"
                          : "Every game has its cover");
        return false;
    }
    g_downloading = true;
    std::thread{DownloadThread, std::move(requests)}.detach();
    return true;
}

bool Downloading() {
    return g_downloading;
}

std::string Status() {
    std::lock_guard lock{g_status_mutex};
    return g_status;
}

bool TakeChanged() {
    return g_changed.exchange(false);
}

} // namespace Covers
