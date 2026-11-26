// SPDX-License-Identifier: GPL-2.0-or-later

// After toronja's tools/transfer-nx/source/main.cpp: the protocol and its commands are that
// program's, unchanged; how a file is received is described in transfer.h. Nothing here is the
// console's own (the sockets are the C library's), so the same code runs on a PC, where it was
// tried against the program that sends.

#include "transfer.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "app.h"
#include "library.h"

namespace Transfer {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;

constexpr u32 kMagic = 0x31585454; // "TTX1"
constexpr const char* kLogFile = "/ns2LOAD/transfer.log";

enum Op : u8 { OpPing = 0, OpList = 1, OpPut = 2, OpGet = 3, OpDelete = 4, OpHash = 5, OpMkdir = 6 };

double Seconds(Clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

// ----------------------------------------------------------------------------------- CRC-32

u32 g_crc_table[4][256];

void CrcInit() {
    static bool made = false;
    if (made) {
        return;
    }
    made = true;
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        g_crc_table[0][i] = c;
    }
    for (u32 i = 0; i < 256; ++i) {
        for (int t = 1; t < 4; ++t) {
            g_crc_table[t][i] = (g_crc_table[t - 1][i] >> 8) ^ g_crc_table[0][g_crc_table[t - 1][i] & 0xFF];
        }
    }
}

/// zlib's CRC-32, four bytes at a time: the sum the program that sends expects.
u32 Crc(u32 crc, const u8* p, std::size_t n) {
    crc = ~crc;
    while (n >= 4) {
        crc ^= static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[3]) << 24;
        crc = g_crc_table[3][crc & 0xFF] ^ g_crc_table[2][(crc >> 8) & 0xFF] ^ g_crc_table[1][(crc >> 16) & 0xFF] ^ g_crc_table[0][crc >> 24];
        p += 4;
        n -= 4;
    }
    while (n--) {
        crc = g_crc_table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// -------------------------------------------------------------------------------------- state

Tuning g_tuning;
std::size_t g_block = 1 << 20;
std::thread g_server;
std::atomic<bool> g_running{false};
std::atomic<bool> g_quit{false};
std::atomic<bool> g_listening{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_busy{false};
std::atomic<u64> g_done{0};
std::atomic<u64> g_total{0};
std::atomic<u64> g_session_bytes{0};
std::atomic<u32> g_session_files{0};
std::atomic<int> g_queue_now{0};
std::atomic<int> g_client{-1}; // The connection being served: Stop hangs it up.
std::mutex g_state_mutex;
std::string g_current;
std::vector<std::string> g_lines;

void AddLine(const char* text, bool to_file, bool to_screen) {
    std::lock_guard lock{g_state_mutex};
    if (to_file) {
        if (std::FILE* file = std::fopen(Flow::OnCard(kLogFile).c_str(), "a")) {
            std::fprintf(file, "%s\n", text);
            std::fclose(file);
        }
    }
    if (to_screen) {
        g_lines.emplace_back(text);
        if (g_lines.size() > 9) {
            g_lines.erase(g_lines.begin());
        }
    }
}

/// To the screen and to the log file.
__attribute__((format(printf, 1, 2))) void Log(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    AddLine(text, true, true);
}

/// To the log file only: the figures of a file, which are longer than a line of the screen.
__attribute__((format(printf, 1, 2))) void Note(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    AddLine(text, true, false);
}

/// To the screen only: a line for each small file would cost a write to the card for each.
__attribute__((format(printf, 1, 2))) void Say(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    AddLine(text, false, true);
}

void SetCurrent(const char* verb, const std::string& path, u64 total) {
    std::lock_guard lock{g_state_mutex};
    g_current = std::string{verb} + " " + path;
    g_done = 0;
    g_total = total;
    g_busy = true;
}

// ------------------------------------------------------------------------------------- blocks

/// The blocks that travel between the network thread and the card thread: a fixed number of
/// them, so that whoever is ahead waits for the other.
struct Block {
    u8* data{};
    std::size_t size{};
};

class Pipe {
public:
    bool Init(int count, std::size_t block_size) {
        for (u8* memory : storage) {
            std::free(memory);
        }
        storage.clear();
        for (int i = 0; i < count; ++i) {
            u8* memory = static_cast<u8*>(std::aligned_alloc(0x1000, block_size));
            if (memory == nullptr) {
                return false;
            }
            storage.push_back(memory);
        }
        return true;
    }

    void Reset() {
        std::lock_guard lock{mutex};
        free_blocks = storage;
        filled.clear();
        closed = false;
        aborted = false;
        most = 0;
        g_queue_now = 0;
    }

    /// An empty block to fill. Null once the other side has given up.
    u8* Acquire() {
        std::unique_lock lock{mutex};
        cv.wait(lock, [this] { return !free_blocks.empty() || aborted; });
        if (aborted) {
            return nullptr;
        }
        u8* block = free_blocks.back();
        free_blocks.pop_back();
        return block;
    }

    void Push(u8* data, std::size_t size) {
        {
            std::lock_guard lock{mutex};
            filled.push_back({data, size});
            most = std::max(most, static_cast<int>(filled.size()));
            g_queue_now = static_cast<int>(filled.size());
        }
        cv.notify_all();
    }

    void Close() {
        {
            std::lock_guard lock{mutex};
            closed = true;
        }
        cv.notify_all();
    }

    void Abort() {
        {
            std::lock_guard lock{mutex};
            aborted = true;
        }
        cv.notify_all();
    }

    bool Aborted() {
        std::lock_guard lock{mutex};
        return aborted;
    }

    /// The next filled block. False at the end, or when aborted.
    bool Pop(Block& out) {
        std::unique_lock lock{mutex};
        cv.wait(lock, [this] { return !filled.empty() || closed || aborted; });
        if (aborted || filled.empty()) {
            return false;
        }
        out = filled.front();
        filled.erase(filled.begin());
        g_queue_now = static_cast<int>(filled.size());
        return true;
    }

    void Release(u8* data) {
        {
            std::lock_guard lock{mutex};
            free_blocks.push_back(data);
        }
        cv.notify_all();
    }

    /// The most blocks that waited for the card at once since Reset.
    int Most() {
        std::lock_guard lock{mutex};
        return most;
    }

    int Count() const {
        return static_cast<int>(storage.size());
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<u8*> storage;
    std::vector<u8*> free_blocks;
    std::vector<Block> filled;
    bool closed{};
    bool aborted{};
    int most{};
};

Pipe g_pipe;

// ------------------------------------------------------------------------------------ sockets

bool SendAll(int fd, const void* data, std::size_t size) {
    const u8* p = static_cast<const u8*>(data);
    while (size > 0) {
        const ssize_t sent = send(fd, p, size, 0);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool RecvAll(int fd, void* data, std::size_t size) {
    u8* p = static_cast<u8*>(data);
    while (size > 0) {
        const ssize_t got = recv(fd, p, size, 0);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

void TuneSocket(int fd) {
    // A small buffer on purpose: what the card hasn't taken is to wait in the sender, not here.
    int size = g_tuning.rcvbuf_kb * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    size = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
}

void SetTimeouts(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

bool SendError(int fd, const char* text) {
    const u16 length = static_cast<u16>(std::strlen(text));
    const u8 head[3] = {1, static_cast<u8>(length), static_cast<u8>(length >> 8)};
    return SendAll(fd, head, 3) && SendAll(fd, text, length);
}

/// A reply: status 0 and nothing else, or a status and the reason as text.
bool SendStatus(int fd, int error, const char* what = "") {
    if (error == 0) {
        const u8 ok = 0;
        return SendAll(fd, &ok, 1);
    }
    char text[256];
    std::snprintf(text, sizeof text, "%s%s%s", what, *what ? ": " : "", std::strerror(error));
    return SendError(fd, text);
}

bool SendU32(int fd, u32 v) {
    const u8 b[4] = {static_cast<u8>(v), static_cast<u8>(v >> 8), static_cast<u8>(v >> 16), static_cast<u8>(v >> 24)};
    return SendAll(fd, b, 4);
}

bool SendU64(int fd, u64 v) {
    return SendU32(fd, static_cast<u32>(v)) && SendU32(fd, static_cast<u32>(v >> 32));
}

// -------------------------------------------------------------------------------------- files

/// "/switch/a.nro" as the client names it, as the C library takes it. Empty when not allowed.
std::string CardPath(const std::string& path) {
    if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos) {
        return {};
    }
    std::string card = path;
    while (card.size() > 1 && card.back() == '/') {
        card.pop_back();
    }
    return Flow::OnCard(card);
}

/// A card path's folder part, for making the folders on the way.
std::string FolderOf(const std::string& shown) {
    const std::size_t slash = shown.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string{} : shown.substr(0, slash);
}

bool WriteAll(int file, const u8* data, std::size_t size) {
    while (size > 0) {
        const ssize_t done = write(file, data, size);
        if (done <= 0) {
            return false;
        }
        data += done;
        size -= static_cast<std::size_t>(done);
    }
    return true;
}

// ----------------------------------------------------------------------------------- commands

bool DoPing(int fd) {
    u64 free_bytes = 0, total_bytes = 0;
    struct statvfs info {};
    if (statvfs(Flow::OnCard("/").c_str(), &info) == 0) {
        free_bytes = static_cast<u64>(info.f_bavail) * info.f_frsize;
        total_bytes = static_cast<u64>(info.f_blocks) * info.f_frsize;
    }
    char text[128];
    const int length = std::snprintf(text, sizeof text, "%s %s switch2", Flow::kName, Flow::kVersion);
    const u8 size[2] = {static_cast<u8>(length), static_cast<u8>(length >> 8)};
    return SendStatus(fd, 0) && SendU64(fd, free_bytes) && SendU64(fd, total_bytes) && SendAll(fd, size, 2) &&
           SendAll(fd, text, static_cast<std::size_t>(length));
}

bool DoList(int fd, const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return SendStatus(fd, errno, "opendir");
    }
    std::vector<u8> out;
    u32 count = 0;
    while (const dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        u8 type = 0;
        u64 size = 0, mtime = 0;
        struct stat info {};
        const std::string full = (path.back() == '/' ? path : path + "/") + name;
        if (stat(full.c_str(), &info) == 0) {
            type = S_ISDIR(info.st_mode) ? 1 : 0;
            size = static_cast<u64>(info.st_size);
            mtime = static_cast<u64>(info.st_mtime);
        }
        const u16 length = static_cast<u16>(name.size());
        out.push_back(type);
        for (int k = 0; k < 8; ++k) {
            out.push_back(static_cast<u8>(size >> (8 * k)));
        }
        for (int k = 0; k < 8; ++k) {
            out.push_back(static_cast<u8>(mtime >> (8 * k)));
        }
        out.push_back(static_cast<u8>(length));
        out.push_back(static_cast<u8>(length >> 8));
        out.insert(out.end(), name.begin(), name.begin() + length);
        ++count;
    }
    closedir(dir);
    return SendStatus(fd, 0) && SendU32(fd, count) && SendU32(fd, static_cast<u32>(out.size())) && SendAll(fd, out.data(), out.size());
}

/// Computer -> card. The network fills blocks on this thread; another thread writes them.
bool DoPut(int fd, const std::string& path, const std::string& shown) {
    u8 raw[8];
    if (!RecvAll(fd, raw, 8)) {
        return false;
    }
    u64 size = 0;
    for (int k = 7; k >= 0; --k) {
        size = size << 8 | raw[k];
    }
    Flow::MakeFolders(FolderOf(shown));
    const std::string part = path + ".ttpart";
    const int file = open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (file < 0) {
        SendStatus(fd, errno, "create");
        return false; // The data that follows cannot be told from a command: hang up.
    }
    // The whole size at once: a file that grows block by block is what makes a card slow.
    const Clock::time_point reserve_started = Clock::now();
    if (size > 0 && ftruncate(file, static_cast<off_t>(size)) != 0) {
        const int error = errno;
        close(file);
        unlink(part.c_str());
        SendStatus(fd, error, "reserve space");
        return false;
    }
    if (!SendStatus(fd, 0)) {
        close(file);
        unlink(part.c_str());
        return false;
    }

    SetCurrent("Receiving", shown, size);
    const Clock::time_point started = Clock::now();
    const double reserve_s = Seconds(started - reserve_started);
    g_pipe.Reset();
    u32 crc = 0;
    int write_error = 0;
    // What the card thread saw: how long the writes took, and the longest time that went by
    // with nothing written (waiting for the network counts: to whoever watches, it is a pause).
    struct Timing {
        double write_s{}, starved_s{}, slowest_s{}, longest_gap_s{}, gap_at_s{};
        u32 blocks{}, over_100ms{}, over_500ms{}, over_2s{};
    } timing;
    std::thread writer{[&] {
        Block block;
        Clock::time_point last_written = started;
        for (;;) {
            const Clock::time_point t0 = Clock::now();
            if (!g_pipe.Pop(block)) {
                break;
            }
            const Clock::time_point t1 = Clock::now();
            crc = Crc(crc, block.data, block.size);
            const Clock::time_point t2 = Clock::now();
            if (!WriteAll(file, block.data, block.size)) {
                write_error = errno != 0 ? errno : EIO;
                g_pipe.Abort();
                return;
            }
            const Clock::time_point t3 = Clock::now();
            const double took = Seconds(t3 - t2), gap = Seconds(t3 - last_written);
            timing.starved_s += Seconds(t1 - t0);
            timing.write_s += took;
            timing.slowest_s = std::max(timing.slowest_s, took);
            if (gap > timing.longest_gap_s) {
                timing.longest_gap_s = gap;
                timing.gap_at_s = Seconds(t3 - started);
            }
            last_written = t3;
            ++timing.blocks;
            timing.over_100ms += took >= 0.1;
            timing.over_500ms += took >= 0.5;
            timing.over_2s += took >= 2.0;
            g_done += block.size;
            g_pipe.Release(block.data);
        }
    }};

    // The network's side. With a cap, it is held to that pace from the first byte: what has
    // come in is never more than the time gone by allows, give or take a block.
    const double cap = g_tuning.kbps > 0 ? g_tuning.kbps * 1024.0 : 0.0;
    bool network_ok = true;
    double blocked_s = 0, recv_s = 0, paced_s = 0;
    u64 left = size, taken = 0;
    while (left > 0) {
        const Clock::time_point t0 = Clock::now();
        u8* block = g_pipe.Acquire();
        if (block == nullptr) {
            break;
        }
        const Clock::time_point t1 = Clock::now();
        blocked_s += Seconds(t1 - t0);
        const std::size_t want = static_cast<std::size_t>(std::min<u64>(left, g_block));
        std::size_t have = 0;
        while (have < want && network_ok) {
            if (cap > 0) {
                const double ahead = static_cast<double>(taken) / cap - Seconds(Clock::now() - started);
                if (ahead > 0.002) {
                    const double wait = std::min(ahead, 0.25);
                    std::this_thread::sleep_for(std::chrono::duration<double>(wait));
                    paced_s += wait;
                    if (g_quit) {
                        network_ok = false;
                    }
                    continue;
                }
            }
            // (with a cap, in pieces small enough for the pace to be even; without, as it comes)
            const std::size_t piece = cap > 0 ? std::min<std::size_t>(want - have, 64 * 1024) : want - have;
            const ssize_t got = recv(fd, block + have, piece, 0);
            if (got <= 0) {
                if (got < 0 && errno == EINTR) {
                    continue;
                }
                network_ok = false;
                break;
            }
            have += static_cast<std::size_t>(got);
            taken += static_cast<u64>(got);
        }
        recv_s += Seconds(Clock::now() - t1);
        if (!network_ok) {
            g_pipe.Abort();
            break;
        }
        g_pipe.Push(block, want);
        left -= want;
    }
    g_pipe.Close();
    writer.join();
    const bool failed = !network_ok || write_error != 0 || g_pipe.Aborted();
    const Clock::time_point close_started = Clock::now();
    if (close(file) != 0 && write_error == 0 && !failed) {
        write_error = errno != 0 ? errno : EIO;
    }
    const double close_s = Seconds(Clock::now() - close_started);
    g_busy = false;

    if (failed || write_error != 0) {
        unlink(part.c_str());
        if (write_error != 0) {
            Log("PUT %s: write failed (%s)", shown.c_str(), std::strerror(write_error));
            SendStatus(fd, write_error, "write");
        } else {
            Log("PUT %s: connection lost", shown.c_str());
        }
        return false;
    }
    unlink(path.c_str());
    if (rename(part.c_str(), path.c_str()) != 0) {
        const int error = errno;
        unlink(part.c_str());
        Log("PUT %s: rename failed (%s)", shown.c_str(), std::strerror(error));
        return SendStatus(fd, error, "rename");
    }
    const double seconds = Seconds(Clock::now() - started);
    const double mib = size / 1048576.0;
    const double mbps = seconds > 0 ? mib / seconds : 0;
    if (size >= 8u * 1024 * 1024) {
        // The lines to tune by: the short one on the screen, the whole of it in the file.
        Say("PUT %s: %.1f MiB in %.1f s, %.1f MB/s; longest pause %.2f s; queue at most %d of %d", shown.c_str(), mib, seconds, mbps,
            timing.longest_gap_s, g_pipe.Most(), g_pipe.Count());
        Note("PUT %s: %.1f MiB in %.1f s, %.1f MB/s; longest time with nothing written %.2f s (at %.1f s); queue at most %d of %d",
             shown.c_str(), mib, seconds, mbps, timing.longest_gap_s, timing.gap_at_s, g_pipe.Most(), g_pipe.Count());
        Note("  blocks of %d KB: %u writes, slowest %.0f ms, 100 ms or more: %u, 500 ms or more: %u, 2 s or more: %u; writing %.1f s (%.1f MB/s "
             "while writing), waiting for the network %.1f s",
             static_cast<int>(g_block / 1024), timing.blocks, timing.slowest_s * 1000, timing.over_100ms, timing.over_500ms, timing.over_2s,
             timing.write_s, timing.write_s > 0 ? mib / timing.write_s : 0, timing.starved_s);
        Note("  network: receiving %.1f s, held back by the queue %.1f s, held back by the cap %.1f s (%d KB/s); socket buffer %d KB; "
             "reserve %.0f ms, close %.0f ms",
             recv_s - paced_s, blocked_s, paced_s, g_tuning.kbps, g_tuning.rcvbuf_kb, reserve_s * 1000, close_s * 1000);
    } else {
        Say("PUT %s: %llu bytes, %.1f MB/s", shown.c_str(), static_cast<unsigned long long>(size), mbps);
    }
    g_session_bytes += size;
    ++g_session_files;
    return SendStatus(fd, 0) && SendU32(fd, crc);
}

/// Card -> computer. A thread reads the card into blocks; this one sends them.
bool DoGet(int fd, const std::string& path, const std::string& shown, bool send_data) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        return SendStatus(fd, errno, "stat");
    }
    if (S_ISDIR(info.st_mode)) {
        return SendError(fd, "is a folder");
    }
    const int file = open(path.c_str(), O_RDONLY);
    if (file < 0) {
        return SendStatus(fd, errno, "open");
    }
    const u64 size = static_cast<u64>(info.st_size);
    if (!SendStatus(fd, 0) || !SendU64(fd, size)) {
        close(file);
        return false;
    }
    SetCurrent(send_data ? "Sending" : "Checking", shown, size);
    const Clock::time_point started = Clock::now();
    g_pipe.Reset();
    u32 crc = 0;
    bool read_ok = true;
    std::thread reader{[&] {
        u64 left = size;
        while (left > 0) {
            u8* block = g_pipe.Acquire();
            if (block == nullptr) {
                return;
            }
            const std::size_t want = static_cast<std::size_t>(std::min<u64>(left, g_block));
            std::size_t have = 0;
            while (have < want) {
                const ssize_t got = read(file, block + have, want - have);
                if (got <= 0) {
                    read_ok = false;
                    g_pipe.Abort();
                    return;
                }
                have += static_cast<std::size_t>(got);
            }
            crc = Crc(crc, block, want);
            g_pipe.Push(block, want);
            left -= want;
        }
        g_pipe.Close();
    }};
    bool network_ok = true;
    Block block;
    while (g_pipe.Pop(block)) {
        if (send_data && !SendAll(fd, block.data, block.size)) {
            network_ok = false;
            g_pipe.Abort();
            break;
        }
        g_done += block.size;
        g_pipe.Release(block.data);
    }
    reader.join();
    close(file);
    g_busy = false;
    if (!network_ok || !read_ok) {
        Log("%s %s: %s", send_data ? "GET" : "HASH", shown.c_str(), read_ok ? "connection lost" : "read failed");
        return false; // The client was promised `size` bytes: all that is left is to hang up.
    }
    if (send_data) {
        const double seconds = Seconds(Clock::now() - started);
        Say("GET %s: %llu bytes, %.1f MB/s", shown.c_str(), static_cast<unsigned long long>(size), seconds > 0 ? size / 1048576.0 / seconds : 0);
        g_session_bytes += size;
        ++g_session_files;
    }
    return SendU32(fd, crc);
}

bool DoDelete(int fd, const std::string& path, const std::string& shown) {
    if (shown == "/") {
        return SendError(fd, "not the whole card");
    }
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) {
        return SendStatus(fd, errno, "stat");
    }
    const int result = S_ISDIR(info.st_mode) ? rmdir(path.c_str()) : unlink(path.c_str());
    if (result != 0) {
        return SendStatus(fd, errno, S_ISDIR(info.st_mode) ? "rmdir" : "delete");
    }
    Say("DELETE %s", shown.c_str());
    return SendStatus(fd, 0);
}

/// One client, command after command, until it hangs up.
void Serve(int fd) {
    for (;;) {
        pollfd waiting{fd, POLLIN, 0};
        const int ready = poll(&waiting, 1, 200);
        if (g_quit) {
            return;
        }
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
            return;
        }
        u8 head[7];
        if (!RecvAll(fd, head, sizeof head)) {
            return;
        }
        const u32 magic = static_cast<u32>(head[0]) | static_cast<u32>(head[1]) << 8 | static_cast<u32>(head[2]) << 16 | static_cast<u32>(head[3]) << 24;
        const u8 op = head[4];
        const u16 path_length = static_cast<u16>(head[5] | (head[6] << 8));
        if (magic != kMagic || path_length > 1024) {
            Log("Bad request, hanging up");
            return;
        }
        std::string shown(path_length, '\0');
        if (path_length > 0 && !RecvAll(fd, shown.data(), path_length)) {
            return;
        }
        if (op == OpPing) {
            if (!DoPing(fd)) {
                return;
            }
            continue;
        }
        const std::string path = CardPath(shown);
        if (path.empty()) {
            SendError(fd, "bad path");
            return; // A PUT's data may follow: hang up rather than read it as commands.
        }
        bool ok = false;
        switch (op) {
        case OpList:
            ok = DoList(fd, path);
            break;
        case OpPut:
            ok = DoPut(fd, path, shown);
            break;
        case OpGet:
            ok = DoGet(fd, path, shown, true);
            break;
        case OpHash:
            ok = DoGet(fd, path, shown, false);
            break;
        case OpDelete:
            ok = DoDelete(fd, path, shown);
            break;
        case OpMkdir: {
            std::string folder = shown;
            while (folder.size() > 1 && folder.back() == '/') {
                folder.pop_back();
            }
            Flow::MakeFolders(folder);
            ok = SendStatus(fd, 0);
            break;
        }
        default:
            SendError(fd, "unknown command");
            break;
        }
        g_busy = false;
        if (!ok) {
            return;
        }
    }
}

void ServerThread() {
    int listener = -1;
    bool complained = false; // (once: this is tried again every second)
    while (!g_quit) {
        if (listener < 0) {
            listener = socket(AF_INET, SOCK_STREAM, 0);
            if (listener < 0) {
                if (!complained) {
                    Log("No socket: %s", std::strerror(errno));
                }
                complained = true;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            int yes = 1;
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
            TuneSocket(listener); // Accepted sockets inherit the buffer sizes.
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(kPort);
            addr.sin_addr.s_addr = INADDR_ANY;
            if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(listener, 2) != 0) {
                if (!complained) {
                    Log("The port couldn't be opened: %s", std::strerror(errno));
                }
                complained = true;
                close(listener);
                listener = -1;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            g_listening = true;
            complained = false;
            Log("Listening on port %d (blocks of %d KB, a queue of %d, %s)", kPort, g_tuning.block_kb, g_tuning.queue,
                g_tuning.kbps > 0 ? ("at most " + std::to_string(g_tuning.kbps) + " KB/s").c_str() : "no cap");
        }
        pollfd waiting{listener, POLLIN, 0};
        const int ready = poll(&waiting, 1, 200);
        if (ready < 0 || (ready > 0 && (waiting.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
            // The network went away under us (cable pulled, sleep): start over.
            close(listener);
            listener = -1;
            g_listening = false;
            continue;
        }
        if (ready == 0) {
            continue;
        }
        sockaddr_in peer{};
        socklen_t peer_size = sizeof peer;
        const int fd = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (fd < 0) {
            continue;
        }
        TuneSocket(fd);
        SetTimeouts(fd, 30);
        Log("%s connected", inet_ntoa(peer.sin_addr));
        g_connected = true;
        g_client = fd;
        Serve(fd);
        g_client = -1;
        g_connected = false;
        g_busy = false;
        close(fd);
        Log("Disconnected");
    }
    if (listener >= 0) {
        close(listener);
    }
    g_listening = false;
}

} // namespace

void Start(const Tuning& tuning) {
    if (g_running) {
        return;
    }
    Stop(); // (joins a thread that has finished)
    g_tuning = tuning;
    g_tuning.block_kb = std::clamp(g_tuning.block_kb, 64, 4096);
    g_tuning.queue = std::clamp(g_tuning.queue, 2, 64);
    g_tuning.kbps = std::max(0, g_tuning.kbps);
    g_tuning.rcvbuf_kb = std::clamp(g_tuning.rcvbuf_kb, 64, 2048);
    g_block = static_cast<std::size_t>(g_tuning.block_kb) * 1024;
    CrcInit();
    {
        std::lock_guard lock{g_state_mutex};
        g_lines.clear();
    }
    if (!g_pipe.Init(g_tuning.queue, g_block)) {
        Log("No memory for %d blocks of %d KB", g_tuning.queue, g_tuning.block_kb);
        return;
    }
    g_quit = false;
    g_running = true;
    g_server = std::thread{ServerThread};
}

void Stop() {
    g_quit = true;
    g_pipe.Abort();
    // (a thread waiting on the connection would wait out its time limit otherwise)
    if (const int fd = g_client; fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
    if (g_server.joinable()) {
        g_server.join();
    }
    g_running = false;
    g_busy = false;
    g_connected = false;
}

State Look() {
    State s;
    s.running = g_running;
    s.listening = g_listening;
    s.connected = g_connected;
    s.busy = g_busy;
    s.done = g_done;
    s.total = g_total;
    s.queue_now = g_queue_now;
    s.queue_size = g_pipe.Count();
    s.files = g_session_files;
    s.bytes = g_session_bytes;
    std::lock_guard lock{g_state_mutex};
    s.current = g_current;
    s.lines = g_lines;
    return s;
}

} // namespace Transfer
