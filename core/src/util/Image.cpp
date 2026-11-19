#include "zederia/util/Image.h"
#include "zederia/util/Compress.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace zed {
namespace {

constexpr int kMaxSide = 16384;
constexpr std::size_t kMaxPixels = 64u * 1024u * 1024u;

// --- Inflate -----------------------------------------------------------------------------------

struct BitReader {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    std::uint32_t buffer = 0;
    int count = 0;
    bool ok = true;

    std::uint32_t bits(int n) {
        while (count < n) {
            if (pos >= data.size()) {
                ok = false;
                return 0;
            }
            buffer |= static_cast<std::uint32_t>(data[pos++]) << count;
            count += 8;
        }
        const std::uint32_t value = buffer & ((1u << n) - 1u);
        buffer >>= n;
        count -= n;
        return value;
    }
    void alignToByte() {
        buffer = 0;
        count = 0;
    }
};

// A canonical Huffman code: how many codes of each length, and the symbols in code order.
struct Huffman {
    std::array<std::uint16_t, 16> counts{};
    std::array<std::uint16_t, 288> symbols{};

    bool build(const std::uint8_t* lengths, int n) {
        counts.fill(0);
        for (int i = 0; i < n; ++i)
            ++counts[lengths[i]];
        counts[0] = 0;
        int left = 1; // code space left: must never go negative
        for (int len = 1; len < 16; ++len) {
            left = (left << 1) - counts[static_cast<std::size_t>(len)];
            if (left < 0)
                return false;
        }
        std::array<std::uint16_t, 16> offsets{};
        for (int len = 1; len < 15; ++len)
            offsets[static_cast<std::size_t>(len + 1)] =
                static_cast<std::uint16_t>(offsets[static_cast<std::size_t>(len)] + counts[static_cast<std::size_t>(len)]);
        for (int i = 0; i < n; ++i)
            if (lengths[i] != 0)
                symbols[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
        return true;
    }

    int decode(BitReader& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(in.bits(1));
            if (!in.ok)
                return -1;
            const int count = counts[static_cast<std::size_t>(len)];
            if (code - count < first)
                return symbols[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

bool inflateBlock(BitReader& in, std::vector<std::uint8_t>& out, const Huffman& literals, const Huffman& distances,
                  std::size_t maxSize) {
    static const std::uint16_t lengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                                 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const std::uint8_t lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                                 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const std::uint16_t distanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                   33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const std::uint8_t distanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        const int symbol = literals.decode(in);
        if (symbol < 0)
            return false;
        if (symbol < 256) {
            if (out.size() >= maxSize)
                return false;
            out.push_back(static_cast<std::uint8_t>(symbol));
        } else if (symbol == 256) {
            return true;
        } else {
            const int lengthIndex = symbol - 257;
            if (lengthIndex >= 29)
                return false;
            const std::size_t length = lengthBase[lengthIndex] + in.bits(lengthExtra[lengthIndex]);
            const int distanceSymbol = distances.decode(in);
            if (distanceSymbol < 0 || distanceSymbol >= 30)
                return false;
            const std::size_t distance = distanceBase[distanceSymbol] + in.bits(distanceExtra[distanceSymbol]);
            if (!in.ok || distance > out.size() || out.size() + length > maxSize)
                return false;
            for (std::size_t i = 0; i < length; ++i)
                out.push_back(out[out.size() - distance]);
        }
    }
}

bool inflateRaw(std::span<const std::uint8_t> data, std::vector<std::uint8_t>& out, std::size_t maxSize) {
    BitReader in{data};
    for (;;) {
        const bool last = in.bits(1) != 0;
        const std::uint32_t type = in.bits(2);
        if (!in.ok)
            return false;
        if (type == 0) { // stored
            in.alignToByte();
            if (in.pos + 4 > data.size())
                return false;
            const std::size_t length = data[in.pos] | (static_cast<std::size_t>(data[in.pos + 1]) << 8);
            const std::size_t check = data[in.pos + 2] | (static_cast<std::size_t>(data[in.pos + 3]) << 8);
            in.pos += 4;
            if ((length ^ check) != 0xffffu || in.pos + length > data.size() || out.size() + length > maxSize)
                return false;
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(in.pos),
                       data.begin() + static_cast<std::ptrdiff_t>(in.pos + length));
            in.pos += length;
        } else if (type == 1) { // fixed codes
            std::uint8_t lengths[288];
            for (int i = 0; i < 288; ++i)
                lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            Huffman literals, distances;
            literals.build(lengths, 288);
            std::uint8_t five[30];
            std::fill(five, five + 30, std::uint8_t{5});
            distances.build(five, 30);
            if (!inflateBlock(in, out, literals, distances, maxSize))
                return false;
        } else if (type == 2) { // dynamic codes
            const int literalCount = static_cast<int>(in.bits(5)) + 257;
            const int distanceCount = static_cast<int>(in.bits(5)) + 1;
            const int codeCount = static_cast<int>(in.bits(4)) + 4;
            if (!in.ok || literalCount > 286 || distanceCount > 30)
                return false;
            static const std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t codeLengths[19] = {};
            for (int i = 0; i < codeCount; ++i)
                codeLengths[order[i]] = static_cast<std::uint8_t>(in.bits(3));
            Huffman codes;
            if (!in.ok || !codes.build(codeLengths, 19))
                return false;
            std::uint8_t lengths[286 + 30] = {};
            int index = 0;
            while (index < literalCount + distanceCount) {
                const int symbol = codes.decode(in);
                if (symbol < 0)
                    return false;
                if (symbol < 16) {
                    lengths[index++] = static_cast<std::uint8_t>(symbol);
                    continue;
                }
                std::uint8_t value = 0;
                int repeat = 0;
                if (symbol == 16) {
                    if (index == 0)
                        return false;
                    value = lengths[index - 1];
                    repeat = 3 + static_cast<int>(in.bits(2));
                } else if (symbol == 17) {
                    repeat = 3 + static_cast<int>(in.bits(3));
                } else {
                    repeat = 11 + static_cast<int>(in.bits(7));
                }
                if (!in.ok || index + repeat > literalCount + distanceCount)
                    return false;
                while (repeat-- > 0)
                    lengths[index++] = value;
            }
            if (lengths[256] == 0)
                return false;
            Huffman literals, distances;
            if (!literals.build(lengths, literalCount) || !distances.build(lengths + literalCount, distanceCount))
                return false;
            if (!inflateBlock(in, out, literals, distances, maxSize))
                return false;
        } else {
            return false;
        }
        if (last)
            return in.ok;
    }
}

// --- Pixels to brightness ----------------------------------------------------------------------

float luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
std::uint32_t le32(const std::uint8_t* p) {
    return p[0] | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

bool sizeOk(std::int64_t width, std::int64_t height, std::string& error) {
    if (width < 1 || height < 1 || width > kMaxSide || height > kMaxSide ||
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > kMaxPixels) {
        error = "the image is empty or larger than " + std::to_string(kMaxSide) + " pixels on a side";
        return false;
    }
    return true;
}

bool decodePng(std::span<const std::uint8_t> bytes, ImageData& out, std::string& error) {
    std::size_t pos = 8;
    int width = 0, height = 0, depth = 0, colour = 0, interlace = 0;
    bool haveHeader = false;
    std::vector<std::uint8_t> packed;
    std::array<std::array<std::uint8_t, 4>, 256> palette{};
    for (auto& entry : palette)
        entry = {0, 0, 0, 255};
    while (pos + 12 <= bytes.size()) {
        const std::size_t length = be32(bytes.data() + pos);
        const std::uint8_t* type = bytes.data() + pos + 4;
        if (length > bytes.size() - pos - 12)
            break;
        const std::uint8_t* data = bytes.data() + pos + 8;
        if (std::memcmp(type, "IHDR", 4) == 0 && length >= 13) {
            if (!sizeOk(be32(data), be32(data + 4), error))
                return false;
            width = static_cast<int>(be32(data));
            height = static_cast<int>(be32(data + 4));
            depth = data[8];
            colour = data[9];
            interlace = data[12];
            haveHeader = true;
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            for (std::size_t i = 0; i < 256 && i * 3 + 2 < length; ++i)
                palette[i] = {data[i * 3], data[i * 3 + 1], data[i * 3 + 2], palette[i][3]};
        } else if (std::memcmp(type, "tRNS", 4) == 0 && colour == 3) {
            for (std::size_t i = 0; i < 256 && i < length; ++i)
                palette[i][3] = data[i];
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            packed.insert(packed.end(), data, data + length);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += length + 12;
    }
    if (!haveHeader || packed.empty()) {
        error = "the PNG file is damaged (no header or no image data)";
        return false;
    }
    if (interlace != 0) {
        error = "interlaced PNG files are not supported; save the image without interlacing";
        return false;
    }
    const int channels = colour == 0 ? 1 : colour == 2 ? 3 : colour == 3 ? 1 : colour == 4 ? 2 : colour == 6 ? 4 : 0;
    const bool depthOk = colour == 3 ? depth == 8 : (depth == 8 || depth == 16);
    if (channels == 0 || !depthOk) {
        error = "this PNG variant is not supported (" + std::to_string(depth) + " bit, colour type " + std::to_string(colour) +
                "); save it as 8-bit greyscale or RGB";
        return false;
    }
    const std::size_t bytesPerPixel = static_cast<std::size_t>(channels) * static_cast<std::size_t>(depth / 8);
    const std::size_t stride = static_cast<std::size_t>(width) * bytesPerPixel;
    const std::size_t expected = (stride + 1) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> raw;
    raw.reserve(expected);
    if (!inflateZlib(packed, raw, expected) || raw.size() != expected) {
        error = "the PNG file is damaged (its image data cannot be unpacked)";
        return false;
    }
    // Undo the per-row filters in place.
    std::vector<std::uint8_t> previous(stride, 0);
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = raw.data() + static_cast<std::size_t>(y) * (stride + 1);
        const std::uint8_t filter = row[0];
        std::uint8_t* px = row + 1;
        for (std::size_t i = 0; i < stride; ++i) {
            const int left = i >= bytesPerPixel ? px[i - bytesPerPixel] : 0;
            const int up = previous[i];
            const int upLeft = i >= bytesPerPixel ? previous[i - bytesPerPixel] : 0;
            int predicted = 0;
            switch (filter) {
            case 0: break;
            case 1: predicted = left; break;
            case 2: predicted = up; break;
            case 3: predicted = (left + up) / 2; break;
            case 4: {
                const int p = left + up - upLeft;
                const int pa = std::abs(p - left), pb = std::abs(p - up), pc = std::abs(p - upLeft);
                predicted = pa <= pb && pa <= pc ? left : pb <= pc ? up : upLeft;
                break;
            }
            default:
                error = "the PNG file is damaged (unknown row filter)";
                return false;
            }
            px[i] = static_cast<std::uint8_t>(px[i] + predicted);
        }
        std::copy(px, px + stride, previous.begin());
    }
    out.width = width;
    out.height = height;
    out.brightness.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const auto sample = [&](const std::uint8_t* p, int channel) {
        return depth == 16 ? static_cast<float>((p[channel * 2] << 8) | p[channel * 2 + 1]) / 65535.0f
                           : static_cast<float>(p[channel]) / 255.0f;
    };
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = raw.data() + static_cast<std::size_t>(y) * (stride + 1) + 1;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* p = row + static_cast<std::size_t>(x) * bytesPerPixel;
            float value = 0.0f;
            switch (colour) {
            case 0: value = sample(p, 0); break;
            case 2: value = luma(sample(p, 0), sample(p, 1), sample(p, 2)); break;
            case 3: {
                const auto& c = palette[p[0]];
                value = luma(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f) * (c[3] / 255.0f);
                break;
            }
            case 4: value = sample(p, 0) * sample(p, 1); break;
            default: value = luma(sample(p, 0), sample(p, 1), sample(p, 2)) * sample(p, 3); break;
            }
            out.brightness[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = value;
        }
    }
    return true;
}

bool decodeBmp(std::span<const std::uint8_t> bytes, ImageData& out, std::string& error) {
    if (bytes.size() < 54) {
        error = "the BMP file is cut short";
        return false;
    }
    const std::size_t dataOffset = le32(bytes.data() + 10);
    const std::uint32_t headerSize = le32(bytes.data() + 14);
    const auto width = static_cast<std::int32_t>(le32(bytes.data() + 18));
    const auto signedHeight = static_cast<std::int32_t>(le32(bytes.data() + 22));
    const int bits = bytes[28] | (bytes[29] << 8);
    const std::uint32_t compression = le32(bytes.data() + 30);
    const bool topDown = signedHeight < 0;
    const std::int64_t height = topDown ? -static_cast<std::int64_t>(signedHeight) : signedHeight;
    if (!sizeOk(width, height, error))
        return false;
    if (headerSize < 40 || (bits != 8 && bits != 24 && bits != 32) || (compression != 0 && !(compression == 3 && bits == 32))) {
        error = "this BMP variant is not supported; save it as an uncompressed 24-bit image";
        return false;
    }
    const std::size_t rowBytes = (static_cast<std::size_t>(width) * static_cast<std::size_t>(bits) + 31) / 32 * 4;
    if (dataOffset > bytes.size() || rowBytes * static_cast<std::size_t>(height) > bytes.size() - dataOffset) {
        error = "the BMP file is cut short";
        return false;
    }
    const std::size_t paletteOffset = 14 + static_cast<std::size_t>(headerSize);
    out.width = width;
    out.height = static_cast<int>(height);
    out.brightness.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < out.height; ++y) {
        const int sourceRow = topDown ? y : out.height - 1 - y;
        const std::uint8_t* row = bytes.data() + dataOffset + static_cast<std::size_t>(sourceRow) * rowBytes;
        for (int x = 0; x < width; ++x) {
            float value = 0.0f;
            if (bits == 8) {
                const std::size_t entry = paletteOffset + static_cast<std::size_t>(row[x]) * 4;
                if (entry + 2 < bytes.size())
                    value = luma(bytes[entry + 2] / 255.0f, bytes[entry + 1] / 255.0f, bytes[entry] / 255.0f);
            } else {
                const std::uint8_t* p = row + static_cast<std::size_t>(x) * static_cast<std::size_t>(bits / 8);
                value = luma(p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f);
            }
            out.brightness[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = value;
        }
    }
    return true;
}

bool decodePnm(std::span<const std::uint8_t> bytes, ImageData& out, std::string& error) {
    const bool colour = bytes[1] == '6';
    std::size_t pos = 2;
    const auto number = [&](std::int64_t& value) {
        for (;;) { // white space and comments
            while (pos < bytes.size() && (bytes[pos] == ' ' || bytes[pos] == '\n' || bytes[pos] == '\r' || bytes[pos] == '\t'))
                ++pos;
            if (pos < bytes.size() && bytes[pos] == '#')
                while (pos < bytes.size() && bytes[pos] != '\n')
                    ++pos;
            else
                break;
        }
        if (pos >= bytes.size() || bytes[pos] < '0' || bytes[pos] > '9')
            return false;
        value = 0;
        while (pos < bytes.size() && bytes[pos] >= '0' && bytes[pos] <= '9' && value < 1000000)
            value = value * 10 + (bytes[pos++] - '0');
        return true;
    };
    std::int64_t width = 0, height = 0, maximum = 0;
    if (!number(width) || !number(height) || !number(maximum) || maximum < 1 || maximum > 65535 || pos >= bytes.size()) {
        error = "the PGM/PPM header cannot be read";
        return false;
    }
    ++pos; // the single white-space byte after the header
    if (!sizeOk(width, height, error))
        return false;
    const std::size_t bytesPerSample = maximum > 255 ? 2 : 1;
    const std::size_t channels = colour ? 3 : 1;
    const std::size_t needed = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * channels * bytesPerSample;
    if (needed > bytes.size() - pos) {
        error = "the PGM/PPM file is cut short";
        return false;
    }
    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.brightness.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    const std::uint8_t* p = bytes.data() + pos;
    const auto sample = [&]() {
        const float v = bytesPerSample == 2 ? static_cast<float>((p[0] << 8) | p[1]) : static_cast<float>(p[0]);
        p += bytesPerSample;
        return std::min(v / static_cast<float>(maximum), 1.0f);
    };
    for (float& value : out.brightness) {
        if (colour) {
            const float r = sample(), g = sample(), b = sample();
            value = luma(r, g, b);
        } else {
            value = sample();
        }
    }
    return true;
}

} // namespace

bool inflateZlib(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t maxSize) {
    out.clear();
    if (in.size() < 6 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20) != 0)
        return false;
    return inflateRaw(in.subspan(2), out, maxSize);
}

bool decodeImage(std::span<const std::uint8_t> bytes, ImageData& out, std::string& error) {
    out = ImageData{};
    static const std::uint8_t pngSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    bool ok = false;
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), pngSignature, 8) == 0)
        ok = decodePng(bytes, out, error);
    else if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M')
        ok = decodeBmp(bytes, out, error);
    else if (bytes.size() >= 3 && bytes[0] == 'P' && (bytes[1] == '5' || bytes[1] == '6'))
        ok = decodePnm(bytes, out, error);
    else
        error = "this is not a PNG, BMP, PGM or PPM image";
    if (!ok)
        out = ImageData{};
    return ok;
}

namespace {

std::uint32_t crc32Of(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0xFFFFFFFFu) {
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

void pngChunk(std::vector<std::uint8_t>& out, const char* type, std::span<const std::uint8_t> data) {
    const auto put32 = [&](std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            out.push_back(static_cast<std::uint8_t>(v >> s));
    };
    put32(static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(crc32Of(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}

} // namespace

std::vector<std::uint8_t> encodePngRgb(int width, int height, std::span<const std::uint8_t> rgb) {
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (width <= 0 || height <= 0 || rgb.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u)
        return {};
    std::vector<std::uint8_t> header;
    for (const int v : {width, height})
        for (int s = 24; s >= 0; s -= 8)
            header.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> s));
    header.insert(header.end(), {8, 2, 0, 0, 0}); // 8 bits, RGB, deflate, no filter choice, no interlace
    pngChunk(out, "IHDR", header);
    std::vector<std::uint8_t> raw;
    const std::size_t row = static_cast<std::size_t>(width) * 3u;
    raw.reserve((row + 1) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        const auto* line = rgb.data() + static_cast<std::size_t>(y) * row;
        raw.insert(raw.end(), line, line + row);
    }
    pngChunk(out, "IDAT", deflateZlib(raw));
    pngChunk(out, "IEND", {});
    return out;
}

} // namespace zed
