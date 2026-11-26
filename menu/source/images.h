// SPDX-License-Identifier: GPL-2.0-or-later

// Pictures read from files (JPEG, PNG) into the canvas's own kind, made smaller on the way:
// the shelf draws in software, and a scan of a cover is far larger than it is ever shown.

#pragma once

#include <string>

#include "ui_views.h"

namespace Images {

/// Decodes a file. The picture is made so that its longer side is at most `max_side` pixels
/// (0: as it is). False when the file isn't there or isn't a picture this reads.
bool LoadFile(const std::string& path, SwitchFrontend::Ui::Image& out, int max_side);

} // namespace Images
