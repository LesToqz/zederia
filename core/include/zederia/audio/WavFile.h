// Reading and writing WAV files with any channel count, loop points and extra chunks. Used for
// wavetables (with the Zederia metadata chunk), textures and imports. No input can crash a read.
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace zed {

struct WavData {
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels; // one vector per channel, equal lengths
    int bits = 32;                            // of the file that was read
    bool isFloat = true;
    bool hasLoop = false; // from the file's sampler chunk
    std::int64_t loopStart = 0, loopEnd = 0; // frames; the loop plays [loopStart, loopEnd)
    std::map<std::string, std::vector<std::uint8_t>> chunks; // other chunks by four-character id

    int channelCount() const { return static_cast<int>(channels.size()); }
    std::size_t frames() const { return channels.empty() ? 0 : channels.front().size(); }
};

// PCM 8, 16, 24 and 32 bit, float 32 and 64 bit, plain and extensible headers.
bool decodeWav(std::span<const std::uint8_t> bytes, WavData& out, std::string& error);

enum class WavSampleFormat { Pcm16, Pcm24, Float32 };
// Writes the channels, the loop (when hasLoop) and every entry of `chunks`.
std::vector<std::uint8_t> encodeWavData(const WavData& data, WavSampleFormat format);

} // namespace zed
