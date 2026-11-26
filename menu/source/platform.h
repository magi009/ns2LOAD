// SPDX-License-Identifier: GPL-2.0-or-later

// What the menu asks of the machine it runs on: the console (main_switch.cpp) or a PC that
// renders previews (preview/main.cpp).

#pragma once

#include <string>

namespace Platform {

/// Whether the machine is on a network at all.
bool HasNetwork();

/// Fetches a URL into a file. False, with `error` in words when there is something to say,
/// when it couldn't (a 404 is no error to tell: the thing isn't there).
bool Download(const std::string& url, const std::string& to_file, std::string& error);

/// Makes the machine's sockets ready (the console's are a service to open). False when there are none.
bool StartNetwork();

/// The machine's address on the network it is on, as text; empty when it is on none.
std::string Address();

/// Keeps the console from going to sleep on its own while something long is under way.
void KeepAwake(bool on);

/// Asks the user for a line of text with the machine's own keyboard. False when nothing was
/// given: the user backed out, or there is no keyboard to show.
bool AskText(const std::string& what_for, const std::string& initial, std::string& out);

} // namespace Platform
