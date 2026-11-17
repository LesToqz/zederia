// Small codecs the patch format needs (plan 5.6): binary assets are stored inside a patch as
// base64 text of zlib-deflated data. The inflater lives with the image reader (util/Image.h); the
// deflater and base64 are here. Written for the core so it needs no plug-in framework.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

// A zlib stream (two-byte header, deflate data with fixed Huffman codes and LZ77 matches, Adler-32).
// Any zlib reader expands it; inflateZlib (util/Image.h) is the one the core uses.
std::vector<std::uint8_t> deflateZlib(std::span<const std::uint8_t> data);

std::string base64Encode(std::span<const std::uint8_t> data);
// False on characters outside the alphabet or a broken length. Whitespace is skipped.
bool base64Decode(std::string_view text, std::vector<std::uint8_t>& out);

// 32-bit floats stored as four byte planes (all first bytes, then all second bytes ...): the
// sign and exponent bytes of an audio signal repeat a lot, so the planes compress far better.
std::vector<std::uint8_t> shuffleFloats(std::span<const float> values);
bool unshuffleFloats(std::span<const std::uint8_t> bytes, std::vector<float>& out);

} // namespace zed
