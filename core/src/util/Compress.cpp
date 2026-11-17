#include "zederia/util/Compress.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace zed {
namespace {

// Writes bits least significant first, as deflate wants.
class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void bits(std::uint32_t value, int count) {
        acc_ |= static_cast<std::uint64_t>(value) << fill_;
        fill_ += count;
        while (fill_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(acc_));
            acc_ >>= 8;
            fill_ -= 8;
        }
    }
    // Huffman codes are defined most significant bit first.
    void code(std::uint32_t code, int length) {
        std::uint32_t reversed = 0;
        for (int i = 0; i < length; ++i)
            reversed |= ((code >> i) & 1u) << (length - 1 - i);
        bits(reversed, length);
    }
    void flush() {
        if (fill_ > 0)
            out_.push_back(static_cast<std::uint8_t>(acc_));
        acc_ = 0;
        fill_ = 0;
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t acc_ = 0;
    int fill_ = 0;
};

constexpr std::array<int, 29> kLengthBase = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                             35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<int, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                              3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<int, 30> kDistBase = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<int, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// The fixed literal/length code of deflate (RFC 1951, 3.2.6).
void putSymbol(BitWriter& w, int symbol) {
    if (symbol <= 143)
        w.code(0x30u + static_cast<std::uint32_t>(symbol), 8);
    else if (symbol <= 255)
        w.code(0x190u + static_cast<std::uint32_t>(symbol - 144), 9);
    else if (symbol <= 279)
        w.code(static_cast<std::uint32_t>(symbol - 256), 7);
    else
        w.code(0xC0u + static_cast<std::uint32_t>(symbol - 280), 8);
}

void putMatch(BitWriter& w, int length, int distance) {
    int li = 28;
    while (kLengthBase[static_cast<std::size_t>(li)] > length)
        --li;
    putSymbol(w, 257 + li);
    if (kLengthExtra[static_cast<std::size_t>(li)] > 0)
        w.bits(static_cast<std::uint32_t>(length - kLengthBase[static_cast<std::size_t>(li)]), kLengthExtra[static_cast<std::size_t>(li)]);
    int di = 29;
    while (kDistBase[static_cast<std::size_t>(di)] > distance)
        --di;
    w.code(static_cast<std::uint32_t>(di), 5);
    if (kDistExtra[static_cast<std::size_t>(di)] > 0)
        w.bits(static_cast<std::uint32_t>(distance - kDistBase[static_cast<std::size_t>(di)]), kDistExtra[static_cast<std::size_t>(di)]);
}

constexpr int kWindow = 32768;
constexpr int kMinMatch = 3, kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kMaxChain = 48;

std::uint32_t adler32(std::span<const std::uint8_t> data) {
    std::uint32_t a = 1, b = 0;
    std::size_t i = 0;
    while (i < data.size()) {
        const std::size_t end = std::min(data.size(), i + 5552);
        for (; i < end; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::vector<std::uint8_t> deflateZlib(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out;
    out.reserve(data.size() / 2 + 64);
    out.push_back(0x78);
    out.push_back(0x01);
    BitWriter w(out);
    w.bits(1, 1); // final block
    w.bits(1, 2); // fixed Huffman codes

    const int n = static_cast<int>(data.size());
    std::vector<int> head(std::size_t{1} << kHashBits, -1);
    std::vector<int> prev(static_cast<std::size_t>(std::max(n, 1)), -1);
    const auto hashAt = [&](int i) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[static_cast<std::size_t>(i)]) |
                                (static_cast<std::uint32_t>(data[static_cast<std::size_t>(i + 1)]) << 8) |
                                (static_cast<std::uint32_t>(data[static_cast<std::size_t>(i + 2)]) << 16);
        return static_cast<int>((v * 2654435761u) >> (32 - kHashBits));
    };
    const auto insert = [&](int i) {
        if (i + kMinMatch > n)
            return;
        const int h = hashAt(i);
        prev[static_cast<std::size_t>(i)] = head[static_cast<std::size_t>(h)];
        head[static_cast<std::size_t>(h)] = i;
    };

    int i = 0;
    while (i < n) {
        int bestLength = 0, bestDistance = 0;
        if (i + kMinMatch <= n) {
            int candidate = head[static_cast<std::size_t>(hashAt(i))];
            const int limit = std::min(kMaxMatch, n - i);
            for (int chain = 0; candidate >= 0 && i - candidate <= kWindow && chain < kMaxChain; ++chain) {
                int length = 0;
                while (length < limit && data[static_cast<std::size_t>(candidate + length)] == data[static_cast<std::size_t>(i + length)])
                    ++length;
                if (length > bestLength) {
                    bestLength = length;
                    bestDistance = i - candidate;
                    if (length == limit)
                        break;
                }
                candidate = prev[static_cast<std::size_t>(candidate)];
            }
        }
        if (bestLength >= kMinMatch) {
            putMatch(w, bestLength, bestDistance);
            for (int k = 0; k < bestLength; ++k)
                insert(i + k);
            i += bestLength;
        } else {
            putSymbol(w, data[static_cast<std::size_t>(i)]);
            insert(i);
            ++i;
        }
    }
    putSymbol(w, 256); // end of block
    w.flush();
    const std::uint32_t check = adler32(data);
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(check >> shift));
    return out;
}

std::string base64Encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i + 1 == data.size()) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool base64Decode(std::string_view text, std::vector<std::uint8_t>& out) {
    std::array<int, 256> value{};
    value.fill(-1);
    for (int k = 0; k < 64; ++k)
        value[static_cast<unsigned char>(kAlphabet[k])] = k;
    out.clear();
    out.reserve(text.size() / 4 * 3);
    std::uint32_t acc = 0;
    int count = 0, padding = 0;
    for (const char c : text) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        if (c == '=') {
            ++padding;
            acc <<= 6;
            ++count;
        } else {
            if (padding > 0)
                return false; // data after padding
            const int v = value[static_cast<unsigned char>(c)];
            if (v < 0)
                return false;
            acc = (acc << 6) | static_cast<std::uint32_t>(v);
            ++count;
        }
        if (count == 4) {
            out.push_back(static_cast<std::uint8_t>(acc >> 16));
            if (padding < 2)
                out.push_back(static_cast<std::uint8_t>(acc >> 8));
            if (padding < 1)
                out.push_back(static_cast<std::uint8_t>(acc));
            acc = 0;
            count = 0;
        }
    }
    return count == 0 && padding <= 2;
}

std::vector<std::uint8_t> shuffleFloats(std::span<const float> values) {
    const std::size_t n = values.size();
    std::vector<std::uint8_t> out(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &values[i], 4);
        for (std::size_t b = 0; b < 4; ++b)
            out[b * n + i] = static_cast<std::uint8_t>(bits >> (8 * b));
    }
    return out;
}

bool unshuffleFloats(std::span<const std::uint8_t> bytes, std::vector<float>& out) {
    if (bytes.size() % 4 != 0)
        return false;
    const std::size_t n = bytes.size() / 4;
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t bits = 0;
        for (std::size_t b = 0; b < 4; ++b)
            bits |= static_cast<std::uint32_t>(bytes[b * n + i]) << (8 * b);
        std::memcpy(&out[i], &bits, 4);
    }
    return true;
}

} // namespace zed
