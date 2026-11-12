// A sample for the Texture player (plan 3.2): immutable stereo audio with optional loop points,
// built off the audio thread and handed over by pointer.
#pragma once

#include "zederia/audio/WavFile.h"

#include <memory>
#include <string>
#include <vector>

namespace zed {

struct SampleData {
    std::string name;
    double sampleRate = 48000.0;
    int frames = 0;
    bool hasLoop = false;          // loop points came with the file
    int loopStart = 0, loopEnd = 0; // the loop plays [loopStart, loopEnd)
    std::vector<float> left, right; // `frames` samples each; mono files are copied to both

    // Mono files play on both sides; files with more than two channels use the first two.
    static std::shared_ptr<const SampleData> fromWav(const WavData& wav, std::string name);
    static std::shared_ptr<const SampleData> fromStereo(std::vector<float> left, std::vector<float> right,
                                                        double sampleRate, std::string name);
    // What the Texture player uses when the patch names no sample: two seconds of white noise.
    static std::shared_ptr<const SampleData> builtin();
};

} // namespace zed
