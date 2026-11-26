// SPDX-License-Identifier: GPL-2.0-or-later

#include "images.h"

#include <cstdio>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wshift-negative-value"
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace Images {

using SwitchFrontend::Ui::u32;
using SwitchFrontend::Ui::u8;

bool LoadFile(const std::string& path, SwitchFrontend::Ui::Image& out, int max_side) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::vector<u8> bytes;
    u8 block[0x4000];
    for (std::size_t n; (n = std::fread(block, 1, sizeof block, f)) > 0;) {
        bytes.insert(bytes.end(), block, block + n);
        if (bytes.size() > (48u << 20)) { // (no cover is this large: not a file to take in whole)
            std::fclose(f);
            return false;
        }
    }
    std::fclose(f);
    int w = 0, h = 0, channels = 0;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) {
            stbi_image_free(px);
        }
        return false;
    }
    // How many old pixels to a new one, each way: whole numbers, so that every new pixel is the
    // plain average of a block (covers are several times larger than shown).
    int step = 1;
    while (max_side > 0 && (w / step > max_side || h / step > max_side)) {
        ++step;
    }
    out.w = w / step;
    out.h = h / step;
    out.px.resize(static_cast<std::size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            u32 sum[4] = {0, 0, 0, 0};
            for (int yy = 0; yy < step; ++yy) {
                const stbi_uc* row = px + (static_cast<std::size_t>(y * step + yy) * w + x * step) * 4;
                for (int xx = 0; xx < step; ++xx) {
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += row[xx * 4 + c];
                    }
                }
            }
            const u32 n = static_cast<u32>(step * step);
            out.px[static_cast<std::size_t>(y) * out.w + x] =
                SwitchFrontend::Ui::MakeColor(static_cast<u8>(sum[0] / n), static_cast<u8>(sum[1] / n),
                                              static_cast<u8>(sum[2] / n), 0xFF);
        }
    }
    stbi_image_free(px);
    return true;
}

} // namespace Images
