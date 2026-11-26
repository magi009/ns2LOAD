// SPDX-License-Identifier: GPL-2.0-or-later

#include "nup.h"

#include <cstdio>
#include <cstring>
#include <map>

namespace Nup {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

u16 Rd16(const u8* p) {
    return static_cast<u16>(p[0] | p[1] << 8);
}
u32 Rd32(const u8* p) {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}
u64 Rd64(const u8* p) {
    return static_cast<u64>(Rd32(p)) | static_cast<u64>(Rd32(p + 4)) << 32;
}

bool ReadAt(std::FILE* f, u64 at, void* to, std::size_t size) {
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (_fseeki64(f, static_cast<long long>(at), SEEK_SET) != 0) {
#else
    if (fseeko(f, static_cast<off_t>(at), SEEK_SET) != 0) {
#endif
        return false;
    }
    return std::fread(to, 1, size, f) == size;
}

// The facts of a package, in their order. A container numbers them from 0 or from 1 (two
// versions of the tool that makes them): which, is told by where an NPDM is.
enum Fact { kNpdm, kNacp, kIcon, kDisplayVersion, kTitle, kPublisher };

struct Package {
    u64 title{};
    u32 version{};
    int kind{};
    u64 meta_at{};
    u32 meta_size{};
    std::map<int, std::vector<u8>> meta; // By the number the container gives each.
};

std::string Text(const std::vector<u8>& v) {
    std::size_t n = 0;
    while (n < v.size() && v[n] != 0) {
        ++n;
    }
    return std::string{reinterpret_cast<const char*>(v.data()), n};
}

/// A NACP's own title and publisher (the first language that has one) and display version:
/// for a container that gives the NACP and not the texts.
void FromNacp(const std::vector<u8>& nacp, std::string& title, std::string& publisher,
              std::string& version) {
    if (nacp.size() < 0x3070) {
        return;
    }
    for (int lang = 0; lang < 16 && title.empty(); ++lang) {
        const u8* entry = nacp.data() + lang * 0x300;
        const std::size_t tl = strnlen(reinterpret_cast<const char*>(entry), 0x200);
        if (tl != 0) {
            title.assign(reinterpret_cast<const char*>(entry), tl);
            const std::size_t pl = strnlen(reinterpret_cast<const char*>(entry + 0x200), 0x100);
            publisher.assign(reinterpret_cast<const char*>(entry + 0x200), pl);
        }
    }
    if (version.empty()) {
        const std::size_t vl = strnlen(reinterpret_cast<const char*>(nacp.data() + 0x3060), 0x10);
        version.assign(reinterpret_cast<const char*>(nacp.data() + 0x3060), vl);
    }
}

} // namespace

bool Read(const std::string& path, Info& out, std::string& error) {
    out = Info{};
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "the file can't be opened";
        return false;
    }
    struct Closer {
        std::FILE* f;
        ~Closer() {
            std::fclose(f);
        }
    } closer{f};

    u8 head[0x80];
    if (!ReadAt(f, 0, head, sizeof head) || std::memcmp(head, "NUP\x01", 4) != 0) {
        error = "not a NUP container";
        return false;
    }
    const u64 toc_at = Rd64(head + 8), toc_size = Rd64(head + 16);
    const u32 count = Rd32(head + 0x48);
    if (count == 0 || count > 64 || toc_size < 0x30 + count * 0x40ull) {
        error = "its table of contents isn't one this reads";
        return false;
    }
    std::vector<u8> toc(0x30 + count * 0x40);
    if (!ReadAt(f, toc_at, toc.data(), toc.size()) || std::memcmp(toc.data(), "TOC\x01", 4) != 0) {
        error = "its table of contents isn't one this reads";
        return false;
    }
    std::vector<Package> packages(count);
    int first_fact = -1;
    for (u32 k = 0; k < count; ++k) {
        const u8* p = toc.data() + 0x30 + k * 0x40;
        Package& pk = packages[k];
        pk.title = Rd64(p);
        pk.version = Rd32(p + 0x14);
        pk.kind = p[0x18];
        pk.meta_at = Rd64(p + 0x20);
        pk.meta_size = Rd32(p + 0x28);
        if (pk.meta_size == 0 || pk.meta_size > (8u << 20)) {
            continue; // (no facts, or a block too large to be one)
        }
        std::vector<u8> block(pk.meta_size);
        if (!ReadAt(f, pk.meta_at, block.data(), block.size())) {
            continue;
        }
        for (std::size_t at = 0; at + 8 <= block.size();) {
            const int kind = Rd16(block.data() + at);
            const u32 size = Rd32(block.data() + at + 4);
            if (at + 8 + size > block.size()) {
                break;
            }
            pk.meta[kind].assign(block.begin() + at + 8, block.begin() + at + 8 + size);
            if (first_fact < 0 && size >= 4 && std::memcmp(block.data() + at + 8, "META", 4) == 0) {
                first_fact = kind;
            }
            at += 8 + size;
        }
    }
    if (first_fact < 0) {
        first_fact = 0;
    }
    const auto fact = [&](const Package& pk, Fact which) -> const std::vector<u8>* {
        const auto it = pk.meta.find(first_fact + which);
        return it == pk.meta.end() || it->second.empty() ? nullptr : &it->second;
    };

    const Package* game = nullptr;
    const Package* update = nullptr;
    out.packages = static_cast<int>(count);
    for (const Package& pk : packages) {
        if (pk.kind == 0 && !game) {
            game = &pk;
        } else if (pk.kind == 1) {
            ++out.updates;
            if (!update || pk.version > update->version) {
                update = &pk;
            }
        } else if (pk.kind == 2) {
            ++out.addons;
        }
    }
    if (!game && !update) {
        error = "it has no game in it (only added content)";
        return false;
    }
    const Package* newest = update ? update : game;
    const Package* base = game ? game : update;
    // (an update's title id is the game's with 0x800 added: the game's is what a game is known by)
    out.title_id = game ? game->title : update->title & ~0xFFFull;
    out.version = newest->version;
    for (const Package* pk : {newest, base}) {
        if (out.display_version.empty()) {
            if (const auto* v = fact(*pk, kDisplayVersion)) {
                out.display_version = Text(*v);
            }
        }
        if (out.title.empty()) {
            if (const auto* v = fact(*pk, kTitle)) {
                out.title = Text(*v);
            }
        }
        if (out.publisher.empty()) {
            if (const auto* v = fact(*pk, kPublisher)) {
                out.publisher = Text(*v);
            }
        }
        if (out.icon.empty()) {
            if (const auto* v = fact(*pk, kIcon); v && v->size() > 4 && (*v)[0] == 0xFF && (*v)[1] == 0xD8) {
                out.icon = *v;
            }
        }
        if (const auto* nacp = fact(*pk, kNacp)) {
            FromNacp(*nacp, out.title, out.publisher, out.display_version);
        }
    }
    return true;
}

} // namespace Nup
