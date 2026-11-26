// SPDX-License-Identifier: GPL-2.0-or-later

// Files between a computer and the card over the local network: what Toronja Transfer does
// (toronja's tools/transfer-nx), as a part of the menu. The same port (28302) and the same
// protocol ("TTX1": PING, LIST, PUT, GET, DELETE, HASH, MKDIR), so the program that sends to
// one sends to the other.
//
// What is different is how a file is received, so that it comes in at one steady pace instead
// of in bursts with seconds of nothing between them:
//   - the network thread and the card thread have a queue of a fixed number of blocks between
//     them, and when it is full the network thread stops reading the socket: the sender is held
//     back by the connection itself, and nothing piles up in memory;
//   - the card is written in blocks of one fixed size, each a single write, into a file that
//     was given its whole size first;
//   - the socket's own buffer is small, so that "held back" reaches the sender at once;
//   - the pace can be capped (KB a second), for a card that takes long over a write now and
//     then: a pace it always keeps is quicker than one it chokes on.
// Each file's log line says what there is to tune by: seconds, MB a second, the longest time
// with nothing written, and how full the queue got.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Transfer {

inline constexpr int kPort = 28302;

/// What can be tuned (menu.cfg: transfer_block_kb, transfer_queue, transfer_kbps,
/// transfer_rcvbuf_kb). Out-of-range values are brought into range by Start.
struct Tuning {
    int block_kb{1024};  // One write to the card: 64 to 4096.
    int queue{8};        // Blocks between the network and the card: 2 to 64.
    int kbps{0};         // The most taken from the network, KB a second; 0: no cap.
    int rcvbuf_kb{256};  // The socket's own buffer: 64 to 2048.
};

struct State {
    bool running{};
    bool listening{};
    bool connected{};
    bool busy{};
    std::string current;          // "Receiving /ns2LOAD/games/x.nup"
    std::uint64_t done{}, total{};
    int queue_now{}, queue_size{};
    std::uint32_t files{};        // This session.
    std::uint64_t bytes{};
    std::vector<std::string> lines; // The last few things that happened.
};

/// Starts listening, on a thread of its own. Nothing happens if it already is.
void Start(const Tuning& tuning);
/// Stops (a transfer under way is cut) and waits for the thread.
void Stop();
State Look();

} // namespace Transfer
