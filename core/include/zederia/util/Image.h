// Reading still images as brightness maps, for the image-to-table import (plan 3.10). Written
// for the core so the command-line tool and the agent server need no plug-in framework:
// PNG (8 and 16 bit, any colour type, not interlaced), BMP (8, 24 and 32 bit, uncompressed) and
// binary PGM/PPM. No input can crash a read.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace zed {

struct ImageData {
    int width = 0, height = 0;
    std::vector<float> brightness; // width * height values in 0..1, top row first; transparent pixels are dark
    float at(int x, int y) const { return brightness[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
};

bool decodeImage(std::span<const std::uint8_t> bytes, ImageData& out, std::string& error);

// Expands a zlib stream (the "deflate" method with its two-byte header). Returns false on
// damaged data or when the result would exceed `maxSize`.
bool inflateZlib(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t maxSize);

} // namespace zed
