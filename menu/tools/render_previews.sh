#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The previews, step by step, for a PC where "make preview" can't do all of it in one shell (on Windows the PC compiler is in
# devkitPro's own MSYS2 shell, and the Python that has Pillow may be another): run from menu/, in any shell that has python.
#
#   sh tools/render_previews.sh
#
# Out: out/preview/*.png
set -e
DKP_SHELL=${DKP_SHELL:-C:/devkitPro/msys2/usr/bin}
HERE=$(pwd)
if [ -x "$DKP_SHELL/bash.exe" ]; then
    "$DKP_SHELL/bash.exe" -lc "cd '$(cygpath -u "$HERE" 2>/dev/null || echo "$HERE")' && make out/preview/preview"
    PATH="$(cygpath -u "$DKP_SHELL" 2>/dev/null || echo "$DKP_SHELL"):$PATH"
else
    make out/preview/preview
fi
rm -rf out/preview/card out/preview/*.png out/preview/*.ppm
python tools/make_fake_card.py out/preview/card
out/preview/preview out/preview/card out/preview
python tools/ppm_to_png.py out/preview
