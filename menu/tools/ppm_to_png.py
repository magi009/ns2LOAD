#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The previews' pictures (binary PPM, as preview/main.cpp writes them) made into PNGs, and the PPMs removed.

    python tools/ppm_to_png.py <folder>          (needs Pillow)
"""
import glob
import os
import sys

from PIL import Image

for path in sorted(glob.glob(os.path.join(sys.argv[1], '*.ppm'))):
    Image.open(path).save(path[:-4] + '.png')
    os.remove(path)
    print(path[:-4] + '.png')
