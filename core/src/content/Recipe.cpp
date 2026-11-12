#include "zederia/content/Recipe.h"

#include "zederia/dsp/Fft.h"
#include "zederia/engine/OscKernel.h"
#include "zederia/params/Registry.h"
#include "zederia/table/Formula.h"
#include "zederia/table/Resynth.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace zed {
namespace {

using J = nlohmann::json;

double numberOr(const J& j, const char* key, double fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}
int intOr(const J& j, const char* key, int fallback) {
    const double v = numberOr(j, key, fallback);
    return std::isfinite(v) ? static_cast<int>(std::clamp(v, -1.0e9, 1.0e9)) : fallback;
}
std::string stringOr(const J& j, const char* key, const std::string& fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}
bool boolOr(const J& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

FrameSelection selectionOf(const J& step) {
    const auto it = step.find("frames");
    if (it != step.end() && it->is_array() && it->size() == 2 && (*it)[0].is_number_integer() && (*it)[1].is_number_integer())
        return {(*it)[0].get<int>(), (*it)[1].get<int>()};
    return allFrames();
}

bool drawShapeFromName(const std::string& name, DrawShape& out) {
    static const std::pair<const char*, DrawShape> names[] = {
        {"flat", DrawShape::Flat},
        {"ramp_up", DrawShape::RampUp},
        {"ramp_down", DrawShape::RampDown},
        {"line", DrawShape::Line},
        {"sine", DrawShape::Sine},
        {"half_sine_up", DrawShape::HalfSineUp},
        {"half_sine_down", DrawShape::HalfSineDown},
        {"quarter_in", DrawShape::QuarterIn},
        {"quarter_out", DrawShape::QuarterOut},
        {"curve", DrawShape::Curve},
    };
    for (const auto& [text, shape] : names) {
        if (name == text) {
            out = shape;
            return true;
        }
    }
    return false;
}

bool tableStep(const J& step, Wavetable& table, std::string& error) {
    const std::string op = stringOr(step, "op", "");
    const FrameSelection selection = selectionOf(step);
    const int first = std::max(selection.first, 0);
    const int last = selection.last < 0 ? table.size() - 1 : std::min(selection.last, table.size() - 1);
    const auto refuse = [&](const std::string& what) {
        error = what;
        return false;
    };

    if (op == "frames") {
        const int count = intOr(step, "count", 0);
        if (count < 1 || count > kMaxFrames)
            return refuse("\"count\" must be from 1 to " + std::to_string(kMaxFrames));
        table.frames.resize(static_cast<std::size_t>(count), Frame{});
        return true;
    }
    if (op == "formula") {
        FormulaRequest request;
        const std::string domain = stringOr(step, "domain", "wave");
        if (domain != "wave" && domain != "harmonics")
            return refuse("\"domain\" must be \"wave\" or \"harmonics\"");
        request.domain = domain == "wave" ? FormulaDomain::Wave : FormulaDomain::Harmonics;
        request.expression = stringOr(step, "expr", "");
        request.phaseExpression = stringOr(step, "phase", "");
        request.scope = selection;
        request.selectedFrame = intOr(step, "selected", 0);
        request.seed = static_cast<std::uint64_t>(std::max(intOr(step, "seed", 1), 0));
        request.normalize = boolOr(step, "normalize", false);
        return applyFormula(table, request, error);
    }
    if (op == "process") {
        ProcessOp process;
        if (!processOpFromName(stringOr(step, "name", ""), process))
            return refuse("unknown process \"" + stringOr(step, "name", "") + "\"");
        processFrames(table, selection, process, intOr(step, "n", 0));
        return true;
    }
    if (op == "harmonic") {
        HarmonicOp harmonic;
        if (!harmonicOpFromName(stringOr(step, "name", ""), harmonic))
            return refuse("unknown harmonic operation \"" + stringOr(step, "name", "") + "\"");
        for (int f = first; f <= last; ++f)
            applyHarmonicOp(table.frames[static_cast<std::size_t>(f)], harmonic, intOr(step, "n", 0), numberOr(step, "amount", 1.0),
                            static_cast<std::uint64_t>(std::max(intOr(step, "seed", 1), 0)) + static_cast<std::uint64_t>(f));
        return true;
    }
    if (op == "draw") {
        DrawShape shape;
        if (!drawShapeFromName(stringOr(step, "shape", ""), shape))
            return refuse("unknown shape \"" + stringOr(step, "shape", "") + "\"");
        for (int f = first; f <= last; ++f)
            drawSegment(table.frames[static_cast<std::size_t>(f)], numberOr(step, "from", 0.0), numberOr(step, "to", 1.0), shape,
                        static_cast<float>(numberOr(step, "a", 0.0)), static_cast<float>(numberOr(step, "b", 1.0)));
        return true;
    }
    if (op == "noise") {
        for (int f = first; f <= last; ++f)
            addNoise(table.frames[static_cast<std::size_t>(f)], numberOr(step, "from", 0.0), numberOr(step, "to", 1.0),
                     static_cast<float>(numberOr(step, "amount", 0.1)),
                     static_cast<std::uint64_t>(std::max(intOr(step, "seed", 1), 0)) + static_cast<std::uint64_t>(f));
        return true;
    }
    if (op == "morph") {
        if (!morphModeFromName(stringOr(step, "mode", ""), table.morph))
            return refuse("unknown morph mode \"" + stringOr(step, "mode", "") + "\"");
        return true;
    }
    if (op == "bake_morph") {
        bakeMorph(table, intOr(step, "frames", kMaxFrames));
        return true;
    }
    if (op == "sort") {
        SortKey key;
        if (!sortKeyFromName(stringOr(step, "by", ""), key))
            return refuse("unknown sort key \"" + stringOr(step, "by", "") + "\"");
        sortFrames(table, key, boolOr(step, "descending", false));
        return true;
    }
    if (op == "reverse") {
        reverseFrames(table);
        return true;
    }
    if (op == "thin")
        return thinFrames(table, intOr(step, "keep_every", 2)) || refuse("\"keep_every\" must be 1 or more");
    if (op == "crop")
        return cropFrames(table, selection) || refuse("the frames to keep are outside the table");
    if (op == "duplicate")
        return duplicateFrames(table, selection) || refuse("the copies would not fit in " + std::to_string(kMaxFrames) + " frames");
    if (op == "delete")
        return deleteFrames(table, selection) || refuse("at least one frame must remain");
    if (op == "move")
        return moveFrame(table, intOr(step, "from", -1), intOr(step, "to", -1)) || refuse("\"from\" or \"to\" is outside the table");
    if (op == "split")
        return splitFrames(table) || refuse("the table would have more than " + std::to_string(kMaxFrames) + " frames");
    if (op == "join") {
        joinFramePairs(table);
        return true;
    }
    if (op == "pulse_width_sweep") {
        const int source = intOr(step, "frame", 0);
        if (source < 0 || source >= table.size())
            return refuse("\"frame\" is outside the table");
        const Wavetable swept = pulseWidthSweep(table.frames[static_cast<std::size_t>(source)], intOr(step, "count", 32),
                                                numberOr(step, "narrowest", 0.05));
        table.frames = swept.frames;
        return true;
    }
    if (op == "copy_spectrum") {
        copySpectrumToAll(table, intOr(step, "source", 0));
        return true;
    }
    if (op == "copy_phases") {
        copyPhasesToAll(table, intOr(step, "source", 0));
        return true;
    }
    if (op == "bake_warp") {
        const auto& modes = Registry::get().find("osc1.warp_a.mode")->choices;
        const std::string name = stringOr(step, "mode", "");
        const auto it = std::find(modes.begin(), modes.end(), name);
        if (it == modes.end() || name == "off")
            return refuse("unknown warp mode \"" + name + "\"");
        const std::string keep = table.name;
        table = bakeWarp(table, numberOr(step, "position", 0.0), static_cast<osc::WarpMode>(it - modes.begin()),
                         intOr(step, "count", 64));
        table.name = keep;
        return true;
    }
    if (op == "trim_silent") {
        trimSilentFrames(table, numberOr(step, "threshold_db", -60.0));
        return true;
    }
    if (op == "stepped") {
        table.stepped = boolOr(step, "value", true);
        return true;
    }
    return refuse(op.empty() ? "the step has no \"op\"" : "unknown step \"" + op + "\"");
}

// --- Textures ----------------------------------------------------------------------------------

struct Stereo {
    std::vector<float> left, right;
};

// Noise with a spectrum that falls by `slope` decibels per octave (0 white, -3 pink, -6 brown),
// optionally band-limited; shaped exactly, in the frequency domain.
std::vector<float> shapedNoise(std::size_t length, double sampleRate, double slopeDb, double lowpass, double highpass, Rng& rng) {
    const int size = dsp::nextPowerOfTwo(static_cast<int>(std::max<std::size_t>(length, 2)));
    std::vector<dsp::Fft::Complex> work(static_cast<std::size_t>(size));
    for (auto& v : work)
        v = rng.uniform() * 2.0 - 1.0;
    const dsp::Fft& fft = dsp::Fft::forSize(size);
    fft.forward(work.data());
    const double exponent = slopeDb / 6.0206; // amplitude ~ f^exponent
    work[0] = 0.0;
    for (int k = 1; k <= size / 2; ++k) {
        const double f = k * sampleRate / size;
        double gain = std::pow(std::max(f, 20.0) / 1000.0, exponent);
        if (lowpass > 0.0)
            gain /= std::sqrt(1.0 + std::pow(f / lowpass, 4.0));
        if (highpass > 0.0)
            gain /= std::sqrt(1.0 + std::pow(highpass / f, 4.0));
        work[static_cast<std::size_t>(k)] *= gain;
        if (k < size / 2)
            work[static_cast<std::size_t>(size - k)] *= gain;
    }
    fft.inverse(work.data());
    std::vector<float> out(length);
    double peak = 1e-12;
    for (std::size_t i = 0; i < length; ++i)
        peak = std::max(peak, std::abs(work[i].real()));
    for (std::size_t i = 0; i < length; ++i)
        out[i] = static_cast<float>(work[i].real() / peak);
    return out;
}

double slopeOf(const std::string& colour) { return colour == "pink" ? -3.0103 : colour == "brown" ? -6.0206 : 0.0; }

bool textureLayer(const J& layer, Stereo& mix, double sampleRate, Rng& rng, std::string& error) {
    const std::string type = stringOr(layer, "type", "");
    const auto level = static_cast<float>(numberOr(layer, "level", 1.0));
    const std::size_t length = mix.left.size();
    const double seconds = static_cast<double>(length) / sampleRate;

    if (type == "noise") {
        const std::string colour = stringOr(layer, "color", "white");
        if (colour != "white" && colour != "pink" && colour != "brown") {
            error = "\"color\" must be white, pink or brown";
            return false;
        }
        const double lowpass = numberOr(layer, "lowpass", 0.0), highpass = numberOr(layer, "highpass", 0.0);
        const std::vector<float> a = shapedNoise(length, sampleRate, slopeOf(colour), lowpass, highpass, rng);
        const std::vector<float> b = shapedNoise(length, sampleRate, slopeOf(colour), lowpass, highpass, rng);
        const double angle = std::clamp(numberOr(layer, "width", 1.0), 0.0, 1.0) * kPi * 0.5;
        const auto same = static_cast<float>(std::cos(angle)), other = static_cast<float>(std::sin(angle));
        for (std::size_t i = 0; i < length; ++i) {
            mix.left[i] += level * a[i];
            mix.right[i] += level * (same * a[i] + other * b[i]);
        }
        return true;
    }
    if (type == "crackle") {
        const double density = std::clamp(numberOr(layer, "density", 10.0), 0.01, 2000.0);
        const double decay = std::exp(-1.0 / (std::max(numberOr(layer, "decay_ms", 2.0), 0.02) * 0.001 * sampleRate));
        const double smooth = 1.0 - std::exp(-2.0 * kPi * std::clamp(numberOr(layer, "lowpass", 6000.0), 20.0, sampleRate * 0.45) / sampleRate);
        const int events = static_cast<int>(density * seconds);
        std::vector<float> impulsesL(length, 0.0f), impulsesR(length, 0.0f);
        for (int e = 0; e < events; ++e) {
            const auto at = static_cast<std::size_t>(rng.uniform() * static_cast<double>(length));
            const double size = (0.25 + 0.75 * rng.uniform() * rng.uniform()) * (rng.uniform() < 0.5 ? -1.0 : 1.0);
            const double pan = rng.uniform();
            impulsesL[std::min(at, length - 1)] += static_cast<float>(size * std::sqrt(1.0 - pan));
            impulsesR[std::min(at, length - 1)] += static_cast<float>(size * std::sqrt(pan));
        }
        double envL = 0.0, envR = 0.0, outL = 0.0, outR = 0.0;
        for (std::size_t i = 0; i < length; ++i) {
            envL = envL * decay + impulsesL[i];
            envR = envR * decay + impulsesR[i];
            outL += (envL - outL) * smooth;
            outR += (envR - outR) * smooth;
            mix.left[i] += level * static_cast<float>(outL);
            mix.right[i] += level * static_cast<float>(outR);
        }
        return true;
    }
    if (type == "wind") {
        const double centre = std::clamp(numberOr(layer, "center_hz", 500.0), 30.0, sampleRate * 0.12);
        const double sweep = std::clamp(numberOr(layer, "sweep_octaves", 1.0), 0.0, 3.0);
        const double rate = std::clamp(numberOr(layer, "rate_hz", 0.2), 0.01, 20.0);
        const double damping = 1.0 / std::clamp(numberOr(layer, "q", 2.0), 0.3, 30.0);
        // Slow movement: three sines at unrelated rates with random phases, for pitch and for level.
        double phases[6];
        for (double& ph : phases)
            ph = rng.uniform();
        const double ratios[3] = {1.0, 1.618, 2.414};
        double lowL = 0.0, bandL = 0.0, lowR = 0.0, bandR = 0.0;
        for (std::size_t i = 0; i < length; ++i) {
            const double t = static_cast<double>(i) / sampleRate;
            double pitch = 0.0, loudness = 0.0;
            for (int k = 0; k < 3; ++k) {
                pitch += std::sin(2.0 * kPi * (rate * ratios[k] * t + phases[k])) / 3.0;
                loudness += std::sin(2.0 * kPi * (rate * 0.7 * ratios[k] * t + phases[k + 3])) / 3.0;
            }
            const double f = 2.0 * std::sin(kPi * std::min(centre * std::exp2(sweep * pitch), sampleRate * 0.15) / sampleRate);
            const double gain = level * (0.65 + 0.35 * loudness);
            const double inL = rng.uniform() * 2.0 - 1.0, inR = rng.uniform() * 2.0 - 1.0;
            // A two-pole state-variable band-pass, once per side.
            lowL += f * bandL;
            bandL += f * (inL - lowL - damping * bandL);
            lowR += f * bandR;
            bandR += f * (inR - lowR - damping * bandR);
            mix.left[i] += static_cast<float>(gain * bandL * damping);
            mix.right[i] += static_cast<float>(gain * bandR * damping);
        }
        return true;
    }
    if (type == "grit") {
        const double hold = std::clamp(numberOr(layer, "hold_hz", 4000.0), 10.0, sampleRate);
        const int levels = 1 << std::clamp(intOr(layer, "bits", 3), 1, 12);
        double phase = 1.0;
        float heldL = 0.0f, heldR = 0.0f;
        for (std::size_t i = 0; i < length; ++i) {
            phase += hold / sampleRate;
            if (phase >= 1.0) {
                phase -= 1.0;
                const auto quantize = [&](double v) {
                    return static_cast<float>((std::floor(v * levels) + 0.5) / levels * 2.0 - 1.0);
                };
                heldL = quantize(rng.uniform());
                heldR = quantize(rng.uniform());
            }
            mix.left[i] += level * heldL;
            mix.right[i] += level * heldR;
        }
        return true;
    }
    if (type == "burst" || type == "tone") {
        const auto start = static_cast<std::size_t>(std::clamp(numberOr(layer, "at", 0.0), 0.0, seconds) * sampleRate);
        const double decaySamples = std::max(numberOr(layer, "decay_ms", 30.0), 0.05) * 0.001 * sampleRate;
        if (type == "tone") {
            const double hz = std::clamp(numberOr(layer, "hz", 200.0), 5.0, sampleRate * 0.45);
            const double drop = numberOr(layer, "drop_octaves", 0.0);
            double phase = 0.0;
            for (std::size_t i = start; i < length; ++i) {
                const double age = static_cast<double>(i - start) / decaySamples;
                if (age > 14.0)
                    break;
                const double envelope = std::exp(-age);
                phase += hz * std::exp2(-drop * (1.0 - envelope)) / sampleRate;
                const auto value = static_cast<float>(level * envelope * std::sin(2.0 * kPi * phase));
                mix.left[i] += value;
                mix.right[i] += value;
            }
            return true;
        }
        const std::size_t span = std::min(length - start, static_cast<std::size_t>(decaySamples * 14.0) + 1);
        const std::string colour = stringOr(layer, "color", "white");
        const std::vector<float> a = shapedNoise(span, sampleRate, slopeOf(colour), numberOr(layer, "lowpass", 0.0),
                                                 numberOr(layer, "highpass", 0.0), rng);
        const std::vector<float> b = shapedNoise(span, sampleRate, slopeOf(colour), numberOr(layer, "lowpass", 0.0),
                                                 numberOr(layer, "highpass", 0.0), rng);
        for (std::size_t i = 0; i < span; ++i) {
            const auto envelope = static_cast<float>(level * std::exp(-static_cast<double>(i) / decaySamples));
            mix.left[start + i] += envelope * a[i];
            mix.right[start + i] += envelope * 0.5f * (a[i] + b[i]);
        }
        return true;
    }
    error = type.empty() ? "the layer has no \"type\"" : "unknown layer type \"" + type + "\"";
    return false;
}

} // namespace

std::string recipeKind(const J& recipe) {
    if (!recipe.is_object())
        return {};
    const std::string kind = stringOr(recipe, "kind", "");
    return kind == "table" || kind == "texture" ? kind : std::string();
}

std::string recipeName(const J& recipe) { return recipe.is_object() ? stringOr(recipe, "name", "") : std::string(); }
std::string recipeCategory(const J& recipe) { return recipe.is_object() ? stringOr(recipe, "category", "") : std::string(); }

bool buildTableRecipe(const J& recipe, Wavetable& out, std::string& error) {
    out = Wavetable{};
    if (recipeKind(recipe) != "table") {
        error = "this is not a table recipe (\"kind\" must be \"table\")";
        return false;
    }
    const int frames = intOr(recipe, "frames", 1);
    if (frames < 1 || frames > kMaxFrames) {
        error = "\"frames\" must be from 1 to " + std::to_string(kMaxFrames);
        return false;
    }
    out = Wavetable::silent(frames);
    out.name = recipeName(recipe);
    const auto steps = recipe.find("steps");
    if (steps == recipe.end() || !steps->is_array() || steps->empty()) {
        error = "the recipe has no \"steps\"";
        return false;
    }
    int number = 0;
    for (const J& step : *steps) {
        ++number;
        std::string problem;
        if (!step.is_object() || !tableStep(step, out, problem)) {
            error = "step " + std::to_string(number) + (step.is_object() ? " (" + stringOr(step, "op", "?") + ")" : "") + ": " +
                    (step.is_object() ? problem : "a step must be an object");
            out = Wavetable{};
            return false;
        }
    }
    // The table's own settings are applied last, so no step can leave them in doubt.
    if (recipe.contains("morph") && !morphModeFromName(stringOr(recipe, "morph", ""), out.morph)) {
        error = "unknown morph mode \"" + stringOr(recipe, "morph", "") + "\"";
        return false;
    }
    if (recipe.contains("stepped"))
        out.stepped = boolOr(recipe, "stepped", false);
    for (Frame& frame : out.frames)
        for (float& s : frame)
            if (!std::isfinite(s))
                s = 0.0f;
    return true;
}

bool buildTextureRecipe(const J& recipe, WavData& out, std::string& error) {
    out = WavData{};
    if (recipeKind(recipe) != "texture") {
        error = "this is not a texture recipe (\"kind\" must be \"texture\")";
        return false;
    }
    const double seconds = numberOr(recipe, "seconds", 2.0);
    const double sampleRate = numberOr(recipe, "rate", 48000.0);
    if (!(seconds >= 0.01 && seconds <= 30.0) || !(sampleRate >= 8000.0 && sampleRate <= 192000.0)) {
        error = "\"seconds\" must be from 0.01 to 30 and \"rate\" from 8000 to 192000";
        return false;
    }
    const bool loop = boolOr(recipe, "loop", true);
    const auto length = static_cast<std::size_t>(seconds * sampleRate);
    const std::size_t overlap = loop ? std::min<std::size_t>(length / 4, static_cast<std::size_t>(0.25 * sampleRate)) : 0;
    Stereo mix;
    mix.left.assign(length + overlap, 0.0f);
    mix.right.assign(length + overlap, 0.0f);
    Rng rng(static_cast<std::uint64_t>(std::max(intOr(recipe, "seed", 1), 0)) * 0x9E3779B97F4A7C15ull + 17u);
    const auto layers = recipe.find("layers");
    if (layers == recipe.end() || !layers->is_array() || layers->empty()) {
        error = "the recipe has no \"layers\"";
        return false;
    }
    int number = 0;
    for (const J& layer : *layers) {
        ++number;
        std::string problem;
        if (!layer.is_object() || !textureLayer(layer, mix, sampleRate, rng, problem)) {
            error = "layer " + std::to_string(number) + ": " + (layer.is_object() ? problem : "a layer must be an object");
            return false;
        }
    }
    if (overlap > 0) {
        // What would follow the end is folded into the start, so the loop point is seamless.
        for (std::size_t i = 0; i < overlap; ++i) {
            const double w = static_cast<double>(i) / static_cast<double>(overlap);
            const auto in = static_cast<float>(std::sin(w * kPi * 0.5)), fromEnd = static_cast<float>(std::cos(w * kPi * 0.5));
            mix.left[i] = mix.left[i] * in + mix.left[length + i] * fromEnd;
            mix.right[i] = mix.right[i] * in + mix.right[length + i] * fromEnd;
        }
        mix.left.resize(length);
        mix.right.resize(length);
    } else if (!loop) {
        // A one-shot ends in silence: a short fade guards against a cut-off tail.
        const std::size_t fade = std::min<std::size_t>(length, static_cast<std::size_t>(0.005 * sampleRate));
        for (std::size_t i = 0; i < fade; ++i) {
            const float gain = static_cast<float>(i) / static_cast<float>(fade);
            mix.left[length - 1 - i] *= gain;
            mix.right[length - 1 - i] *= gain;
        }
    }
    float peak = 0.0f;
    for (std::size_t i = 0; i < length; ++i) {
        if (!std::isfinite(mix.left[i]) || !std::isfinite(mix.right[i]))
            mix.left[i] = mix.right[i] = 0.0f;
        peak = std::max({peak, std::abs(mix.left[i]), std::abs(mix.right[i])});
    }
    if (peak > 0.0f) {
        const auto gain = static_cast<float>(std::pow(10.0, std::clamp(numberOr(recipe, "peak_db", -3.0), -60.0, 0.0) / 20.0) / peak);
        for (std::size_t i = 0; i < length; ++i) {
            mix.left[i] *= gain;
            mix.right[i] *= gain;
        }
    }
    out.sampleRate = sampleRate;
    out.channels.push_back(std::move(mix.left));
    out.channels.push_back(std::move(mix.right));
    out.hasLoop = loop;
    out.loopStart = 0;
    out.loopEnd = static_cast<std::int64_t>(length);
    return true;
}

} // namespace zed
