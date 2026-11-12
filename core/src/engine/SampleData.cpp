#include "zederia/engine/SampleData.h"

#include "zederia/util/Math.h"

#include <algorithm>

namespace zed {

std::shared_ptr<const SampleData> SampleData::fromStereo(std::vector<float> left, std::vector<float> right,
                                                         double sampleRate, std::string name) {
    auto out = std::make_shared<SampleData>();
    out->name = std::move(name);
    out->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    out->left = std::move(left);
    out->right = std::move(right);
    if (out->right.size() != out->left.size())
        out->right = out->left;
    out->frames = static_cast<int>(std::min<std::size_t>(out->left.size(), 1u << 30));
    out->loopEnd = out->frames;
    return out;
}

std::shared_ptr<const SampleData> SampleData::fromWav(const WavData& wav, std::string name) {
    if (wav.channels.empty())
        return fromStereo({}, {}, wav.sampleRate, std::move(name));
    auto built = fromStereo(wav.channels[0], wav.channels.size() > 1 ? wav.channels[1] : wav.channels[0], wav.sampleRate,
                            std::move(name));
    auto out = std::const_pointer_cast<SampleData>(built);
    if (wav.hasLoop && wav.loopEnd - wav.loopStart >= 2 && wav.loopEnd <= out->frames) {
        out->hasLoop = true;
        out->loopStart = static_cast<int>(wav.loopStart);
        out->loopEnd = static_cast<int>(wav.loopEnd);
    }
    return out;
}

std::shared_ptr<const SampleData> SampleData::builtin() {
    static const std::shared_ptr<const SampleData> noise = [] {
        const int frames = 96000;
        std::vector<float> left(frames), right(frames);
        Rng rng(0x7e47u);
        for (int i = 0; i < frames; ++i) {
            left[static_cast<std::size_t>(i)] = static_cast<float>(rng.uniform() * 2.0 - 1.0) * 0.5f;
            right[static_cast<std::size_t>(i)] = static_cast<float>(rng.uniform() * 2.0 - 1.0) * 0.5f;
        }
        return fromStereo(std::move(left), std::move(right), 48000.0, "White Noise");
    }();
    return noise;
}

} // namespace zed
