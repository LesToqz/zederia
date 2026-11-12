#include "zederia/audio/WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace zed {
namespace {

constexpr std::size_t kMaxFrames = 512u * 1024u * 1024u / 4u; // 512 MB of float data per file
constexpr int kMaxChannels = 64;

std::uint32_t u16at(std::span<const std::uint8_t> b, std::size_t p) {
    return static_cast<std::uint32_t>(b[p]) | (static_cast<std::uint32_t>(b[p + 1]) << 8);
}
std::uint32_t u32at(std::span<const std::uint8_t> b, std::size_t p) { return u16at(b, p) | (u16at(b, p + 2) << 16); }

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xffffu);
    put16(out, v >> 16);
}
void putTag(std::vector<std::uint8_t>& out, const char* tag) { out.insert(out.end(), tag, tag + 4); }

} // namespace

bool decodeWav(std::span<const std::uint8_t> bytes, WavData& out, std::string& error) {
    out = WavData{};
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        error = "this is not a WAV file (it does not start with RIFF/WAVE)";
        return false;
    }
    std::uint32_t format = 0, channels = 0, bits = 0, blockAlign = 0;
    double sampleRate = 0.0;
    bool haveFormat = false;
    std::span<const std::uint8_t> data;
    bool haveData = false;

    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::string id(reinterpret_cast<const char*>(bytes.data() + pos), 4);
        std::size_t length = u32at(bytes, pos + 4);
        pos += 8;
        if (length > bytes.size() - pos) {
            if (id != "data") { // a cut-short trailing chunk other than the audio is ignored
                break;
            }
            length = bytes.size() - pos; // cut-short audio: take what is there
        }
        const auto chunk = bytes.subspan(pos, length);
        if (id == "fmt ") {
            if (length < 16) {
                error = "the WAV file's format chunk is too short";
                return false;
            }
            format = u16at(chunk, 0);
            channels = u16at(chunk, 2);
            sampleRate = u32at(chunk, 4);
            blockAlign = u16at(chunk, 12);
            bits = u16at(chunk, 14);
            if (format == 0xfffe && length >= 26) // extensible: the real format is in the sub-format
                format = u16at(chunk, 24);
            haveFormat = true;
        } else if (id == "data") {
            if (!haveData) {
                data = chunk;
                haveData = true;
            }
        } else if (id == "smpl") {
            if (length >= 36 + 24 && u32at(chunk, 28) >= 1) { // the first loop
                out.hasLoop = true;
                out.loopStart = u32at(chunk, 36 + 8);
                out.loopEnd = static_cast<std::int64_t>(u32at(chunk, 36 + 12)) + 1; // stored inclusive
            }
        } else if (length <= 1024u * 1024u) {
            out.chunks[id].assign(chunk.begin(), chunk.end());
        }
        pos += length + (length & 1u);
    }
    if (!haveFormat || !haveData) {
        error = "the WAV file has no format or no audio data";
        return false;
    }
    const bool isFloat = format == 3;
    if (format != 1 && format != 3) {
        error = "only uncompressed WAV files (PCM or float) are supported";
        return false;
    }
    const bool validBits = isFloat ? (bits == 32 || bits == 64) : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!validBits || channels == 0 || channels > static_cast<std::uint32_t>(kMaxChannels) || sampleRate < 1.0 ||
        sampleRate > 1.0e6) {
        error = "the WAV file has a format Zederia does not read (" + std::to_string(bits) + " bit, " +
                std::to_string(channels) + " channel(s))";
        return false;
    }
    const std::size_t bytesPerSample = bits / 8;
    const std::size_t frameBytes = std::max<std::size_t>(blockAlign, bytesPerSample * channels);
    if (frameBytes < bytesPerSample * channels) {
        error = "the WAV file's format chunk is inconsistent";
        return false;
    }
    const std::size_t frames = std::min(data.size() / frameBytes, kMaxFrames / channels);
    out.sampleRate = sampleRate;
    out.bits = static_cast<int>(bits);
    out.isFloat = isFloat;
    out.channels.assign(channels, std::vector<float>(frames));
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const std::uint8_t* p = data.data() + f * frameBytes + c * bytesPerSample;
            double v = 0.0;
            if (isFloat && bits == 32) {
                float x;
                std::memcpy(&x, p, 4);
                v = x;
            } else if (isFloat) {
                std::memcpy(&v, p, 8);
            } else if (bits == 8) {
                v = (static_cast<int>(p[0]) - 128) / 128.0;
            } else if (bits == 16) {
                v = static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8))) / 32768.0;
            } else if (bits == 24) {
                std::int32_t x = p[0] | (p[1] << 8) | (p[2] << 16);
                if (x & 0x800000)
                    x -= 0x1000000;
                v = x / 8388608.0;
            } else {
                std::uint32_t x;
                std::memcpy(&x, p, 4);
                v = static_cast<std::int32_t>(x) / 2147483648.0;
            }
            if (!std::isfinite(v))
                v = 0.0;
            out.channels[c][f] = static_cast<float>(std::clamp(v, -64.0, 64.0));
        }
    }
    if (out.hasLoop) {
        out.loopStart = std::clamp<std::int64_t>(out.loopStart, 0, static_cast<std::int64_t>(frames));
        out.loopEnd = std::clamp<std::int64_t>(out.loopEnd, 0, static_cast<std::int64_t>(frames));
        if (out.loopEnd - out.loopStart < 2)
            out.hasLoop = false;
    }
    return true;
}

std::vector<std::uint8_t> encodeWavData(const WavData& data, WavSampleFormat format) {
    const auto channels = static_cast<std::uint32_t>(std::max(1, data.channelCount()));
    const std::size_t frames = data.frames();
    const std::uint32_t bytesPerSample = format == WavSampleFormat::Pcm16 ? 2 : format == WavSampleFormat::Pcm24 ? 3 : 4;
    const bool isFloat = format == WavSampleFormat::Float32;
    const auto dataBytes = static_cast<std::uint32_t>(frames * channels * bytesPerSample);
    const auto rate = static_cast<std::uint32_t>(std::lround(data.sampleRate));

    std::vector<std::uint8_t> out;
    out.reserve(dataBytes + 256);
    putTag(out, "RIFF");
    put32(out, 0); // patched at the end
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    put32(out, 16);
    put16(out, isFloat ? 3 : 1);
    put16(out, channels);
    put32(out, rate);
    put32(out, rate * channels * bytesPerSample);
    put16(out, channels * bytesPerSample);
    put16(out, bytesPerSample * 8);
    if (isFloat) {
        putTag(out, "fact");
        put32(out, 4);
        put32(out, static_cast<std::uint32_t>(frames));
    }
    for (const auto& [id, bytes] : data.chunks) { // metadata before the audio, so a reader finds it early
        if (id.size() != 4 || id == "fmt " || id == "data" || id == "fact" || id == "smpl")
            continue;
        putTag(out, id.c_str());
        put32(out, static_cast<std::uint32_t>(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
        if (bytes.size() & 1u)
            out.push_back(0);
    }
    if (data.hasLoop) {
        putTag(out, "smpl");
        put32(out, 36 + 24);
        put32(out, 0); // manufacturer
        put32(out, 0); // product
        put32(out, rate > 0 ? static_cast<std::uint32_t>(1.0e9 / rate) : 0); // sample period in ns
        put32(out, 60); // unity note
        put32(out, 0);  // pitch fraction
        put32(out, 0);  // SMPTE format
        put32(out, 0);  // SMPTE offset
        put32(out, 1);  // one loop
        put32(out, 0);  // sampler data
        put32(out, 0);  // cue id
        put32(out, 0);  // forward loop
        put32(out, static_cast<std::uint32_t>(data.loopStart));
        put32(out, static_cast<std::uint32_t>(std::max<std::int64_t>(data.loopEnd - 1, 0)));
        put32(out, 0); // fraction
        put32(out, 0); // play count: endless
    }
    putTag(out, "data");
    put32(out, dataBytes);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const float sample = c < data.channels.size() ? data.channels[c][f] : 0.0f;
            if (isFloat) {
                std::uint32_t bitsValue = 0;
                std::memcpy(&bitsValue, &sample, 4);
                put32(out, bitsValue);
                continue;
            }
            const double clipped = std::clamp(static_cast<double>(sample), -1.0, 1.0);
            if (format == WavSampleFormat::Pcm16) {
                put16(out, static_cast<std::uint32_t>(std::lround(clipped * 32767.0)) & 0xffffu);
            } else {
                const auto v = static_cast<std::uint32_t>(std::lround(clipped * 8388607.0));
                out.push_back(static_cast<std::uint8_t>(v));
                out.push_back(static_cast<std::uint8_t>(v >> 8));
                out.push_back(static_cast<std::uint8_t>(v >> 16));
            }
        }
    }
    if (dataBytes & 1u)
        out.push_back(0);
    const auto riffSize = static_cast<std::uint32_t>(out.size() - 8);
    out[4] = static_cast<std::uint8_t>(riffSize);
    out[5] = static_cast<std::uint8_t>(riffSize >> 8);
    out[6] = static_cast<std::uint8_t>(riffSize >> 16);
    out[7] = static_cast<std::uint8_t>(riffSize >> 24);
    return out;
}

} // namespace zed
