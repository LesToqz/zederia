// Runs one effect on its own, the way the rack runs it: in control intervals, with the parameter
// values in the effect's own units, flush-to-zero on.
#pragma once

#include "Analysis.h"
#include "TestUtil.h"
#include "zederia/dsp/HalfBand.h"
#include "zederia/fx/Effect.h"
#include "zederia/params/Registry.h"
#include "zederia/rt/Denormals.h"
#include "zederia/util/Math.h"

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace zedtest {

struct FxRig {
    std::unique_ptr<zed::fx::Effect> effect;
    int type = -1;
    std::vector<double> values;
    zed::fx::Context ctx;

    explicit FxRig(const char* id, double rate = 48000.0, int oversample = 2) {
        const auto& reg = zed::Registry::get();
        type = reg.effectIndex(id);
        effect = zed::fx::makeEffect(type, rate);
        for (const auto& p : reg.effectTypes()[static_cast<std::size_t>(type)].params)
            values.push_back(p.def);
        ctx.sampleRate = rate;
        setQuality(oversample);
        effect->set(values.data(), ctx, true);
    }
    void setQuality(int oversample) {
        ctx.oversample = oversample;
        ctx.draft = oversample == 1;
        ctx.halfBands = &zed::dsp::decimatorDesign(oversample >= 4);
    }
    int param(const char* name) const {
        const auto& params = zed::Registry::get().effectTypes()[static_cast<std::size_t>(type)].params;
        for (std::size_t i = 0; i < params.size(); ++i)
            if (params[i].id.substr(params[i].id.rfind('.') + 1) == name)
                return static_cast<int>(i);
        throw std::runtime_error(std::string("no such effect parameter: ") + name);
    }
    void set(const char* name, double value, bool snap = true) {
        values[static_cast<std::size_t>(param(name))] = value;
        effect->set(values.data(), ctx, snap);
    }
    void choose(const char* name, const char* token, bool snap = true) {
        const auto& params = zed::Registry::get().effectTypes()[static_cast<std::size_t>(type)].params;
        set(name, params[static_cast<std::size_t>(param(name))].choiceIndex(token), snap);
    }
    void apply(bool snap = false) { effect->set(values.data(), ctx, snap); }

    // Processes in control intervals; the wet signal replaces the input.
    void run(std::vector<float>& left, std::vector<float>& right) {
        const zed::rt::ScopedNoDenormals noDenormals;
        for (std::size_t at = 0; at < left.size(); at += zed::fx::kMaxFrames) {
            const int frames = static_cast<int>(std::min<std::size_t>(zed::fx::kMaxFrames, left.size() - at));
            effect->process(left.data() + at, right.data() + at, frames);
        }
    }
    // Mono input on both sides.
    std::pair<std::vector<float>, std::vector<float>> runMono(const std::vector<float>& in) {
        std::vector<float> l = in, r = in;
        run(l, r);
        return {std::move(l), std::move(r)};
    }
};

inline std::vector<float> sine(double hz, double seconds, double level = 0.5, double rate = 48000.0) {
    std::vector<float> x(static_cast<std::size_t>(seconds * rate));
    for (std::size_t n = 0; n < x.size(); ++n)
        x[n] = static_cast<float>(level * std::sin(2.0 * zed::kPi * hz * static_cast<double>(n) / rate));
    return x;
}

inline std::vector<float> noise(double seconds, double level = 0.25, std::uint64_t seed = 7, double rate = 48000.0) {
    std::vector<float> x(static_cast<std::size_t>(seconds * rate));
    zed::Rng rng(seed);
    for (float& s : x)
        s = static_cast<float>(level * (2.0 * rng.uniform() - 1.0) * std::sqrt(3.0));
    return x;
}

inline std::vector<float> withSilence(std::vector<float> x, double seconds, double rate = 48000.0) {
    x.resize(x.size() + static_cast<std::size_t>(seconds * rate), 0.0f);
    return x;
}

inline double db(double gain) { return 20.0 * std::log10(std::max(gain, 1e-30)); }

inline double correlation(const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t to) {
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = from; i < to && i < a.size() && i < b.size(); ++i) {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt(aa * bb) : 1.0;
}

} // namespace zedtest
