// SPDX-License-Identifier: GPL-2.0-or-later

// The menu's file receiver (source/transfer.cpp) on a PC, with a folder for a card: to try it
// against the program that sends, with no console.
//
//   transfer_test <card folder> <seconds to run> [block KB] [queue] [KB a second] [socket buffer KB]
//
// It prints what the Transfer screen would list. The sender is then pointed at 127.0.0.1.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "library.h"
#include "transfer.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("transfer_test <card folder> <seconds> [block KB] [queue] [KB a second] [socket buffer KB]\n");
        return 1;
    }
    Flow::SetCardRoot(argv[1]);
    Flow::MakeFolders(Flow::kHome);
    Transfer::Tuning tuning;
    if (argc > 3) {
        tuning.block_kb = std::atoi(argv[3]);
    }
    if (argc > 4) {
        tuning.queue = std::atoi(argv[4]);
    }
    if (argc > 5) {
        tuning.kbps = std::atoi(argv[5]);
    }
    if (argc > 6) {
        tuning.rcvbuf_kb = std::atoi(argv[6]);
    }
    Transfer::Start(tuning);
    std::string last;
    const int ticks = std::atoi(argv[2]) * 10;
    for (int k = 0; k < ticks; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const Transfer::State state = Transfer::Look();
        // (only what is new: the screen keeps the last few lines, so print from the first unseen)
        std::size_t from = 0;
        for (std::size_t n = 0; n < state.lines.size(); ++n) {
            if (state.lines[n] == last) {
                from = n + 1;
            }
        }
        for (std::size_t n = from; n < state.lines.size(); ++n) {
            std::printf("%s\n", state.lines[n].c_str());
            last = state.lines[n];
        }
        std::fflush(stdout);
    }
    Transfer::Stop();
    const Transfer::State state = Transfer::Look();
    std::printf("%u files, %llu bytes\n", state.files, static_cast<unsigned long long>(state.bytes));
    return 0;
}
