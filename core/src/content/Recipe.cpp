#include "zederia/content/Recipe.h"

#include "zederia/dsp/Fft.h"
#include "zederia/engine/OscKernel.h"
#include "zederia/midi/MidiFile.h"
#include "zederia/music/Theory.h"
#include "zederia/params/Registry.h"
#include "zederia/patch/Modules.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/table/Formula.h"
#include "zederia/table/Resynth.h"
#include "zederia/table/Toolkit.h"
#include "zederia/util/Math.h"

#include "zederia/render/OfflineRenderer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <system_error>

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
        ProcessOp process = ProcessOp::Clear;
        if (!processOpFromName(stringOr(step, "name", ""), process))
            return refuse("unknown process \"" + stringOr(step, "name", "") + "\"");
        processFrames(table, selection, process, intOr(step, "n", 0));
        return true;
    }
    if (op == "harmonic") {
        HarmonicOp harmonic = HarmonicOp::ClearAll;
        if (!harmonicOpFromName(stringOr(step, "name", ""), harmonic))
            return refuse("unknown harmonic operation \"" + stringOr(step, "name", "") + "\"");
        for (int f = first; f <= last; ++f)
            applyHarmonicOp(table.frames[static_cast<std::size_t>(f)], harmonic, intOr(step, "n", 0), numberOr(step, "amount", 1.0),
                            static_cast<std::uint64_t>(std::max(intOr(step, "seed", 1), 0)) + static_cast<std::uint64_t>(f));
        return true;
    }
    if (op == "draw") {
        DrawShape shape = DrawShape::Flat;
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
        SortKey key = SortKey::Brightness;
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

// Reads a list of 1 to 32 numbers; false when the field is missing or holds anything else.
bool numberList(const J& layer, const char* key, std::vector<double>& out) {
    out.clear();
    const auto it = layer.find(key);
    if (it == layer.end() || !it->is_array() || it->empty() || it->size() > 32)
        return false;
    for (const J& v : *it) {
        if (!v.is_number() || !std::isfinite(v.get<double>()))
            return false;
        out.push_back(v.get<double>());
    }
    return true;
}

// `loopLength` is the length of one pass of a looped texture in samples (what follows it is folded
// into its start), 0 for a one-shot.
bool textureLayer(const J& layer, Stereo& mix, double sampleRate, std::size_t loopLength, Rng& rng, std::string& error) {
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
    if (type == "partials") {
        // Struck partials: sines at hz times each ratio, started at `at` and decaying; a higher
        // partial decays faster, by its ratio to the power -decay_tilt. Levels default to
        // ratio^-0.5. spread_cents detunes the two sides apart.
        std::vector<double> ratios, levels;
        if (!numberList(layer, "ratios", ratios)) {
            error = "\"ratios\" must be a list of 1 to 32 numbers";
            return false;
        }
        const bool haveLevels = layer.contains("levels");
        if (haveLevels && (!numberList(layer, "levels", levels) || levels.size() != ratios.size())) {
            error = "\"levels\" must be a list of numbers, one for each ratio";
            return false;
        }
        const auto start = static_cast<std::size_t>(std::clamp(numberOr(layer, "at", 0.0), 0.0, seconds) * sampleRate);
        const double hz = std::clamp(numberOr(layer, "hz", 440.0), 5.0, sampleRate * 0.45);
        const double decay = std::max(numberOr(layer, "decay_ms", 500.0), 0.05) * 0.001 * sampleRate;
        const double tilt = std::clamp(numberOr(layer, "decay_tilt", 0.5), 0.0, 4.0);
        const double spread = std::clamp(numberOr(layer, "spread_cents", 0.0), 0.0, 100.0);
        for (std::size_t k = 0; k < ratios.size(); ++k) {
            const double ratio = ratios[k];
            const double phase0 = rng.uniform(); // drawn for every partial, so the sequence never depends on the rate
            const double f = hz * ratio;
            if (!(ratio > 0.0) || f >= sampleRate * 0.45)
                continue;
            const double amplitude = level * (haveLevels ? levels[k] : std::pow(ratio, -0.5));
            const double partialDecay = decay * std::pow(ratio, -tilt);
            const double stepL = f * std::exp2(-spread / 2400.0) / sampleRate, stepR = f * std::exp2(spread / 2400.0) / sampleRate;
            double phaseL = phase0, phaseR = phase0;
            for (std::size_t i = start; i < length; ++i) {
                const double age = static_cast<double>(i - start) / partialDecay;
                if (age > 14.0)
                    break;
                const double envelope = amplitude * std::exp(-age);
                mix.left[i] += static_cast<float>(envelope * std::sin(2.0 * kPi * phaseL));
                mix.right[i] += static_cast<float>(envelope * std::sin(2.0 * kPi * phaseR));
                phaseL += stepL;
                phaseR += stepR;
            }
        }
        return true;
    }
    if (type == "hum") {
        // A steady tone with `harmonics` partials falling by tilt_db per octave. In a looped texture
        // the pitch is nudged so a whole number of cycles fits the loop, which keeps the loop seamless.
        double hz = std::clamp(numberOr(layer, "hz", 60.0), 5.0, sampleRate * 0.45);
        const int harmonics = std::clamp(intOr(layer, "harmonics", 1), 1, 64);
        const double tiltDb = std::clamp(numberOr(layer, "tilt_db", -6.0), -48.0, 12.0);
        if (loopLength > 0) {
            const double cycles = std::max(1.0, std::round(hz * static_cast<double>(loopLength) / sampleRate));
            hz = cycles * sampleRate / static_cast<double>(loopLength);
        }
        for (int k = 1; k <= harmonics; ++k) {
            const double f = hz * k;
            if (f >= sampleRate * 0.45)
                break;
            const double amplitude = level * std::pow(10.0, tiltDb * std::log2(static_cast<double>(k)) / 20.0);
            const double phaseL = rng.uniform(), phaseR = rng.uniform();
            for (std::size_t i = 0; i < length; ++i) {
                const double cycle = f * static_cast<double>(i) / sampleRate;
                mix.left[i] += static_cast<float>(amplitude * std::sin(2.0 * kPi * (cycle + phaseL)));
                mix.right[i] += static_cast<float>(amplitude * std::sin(2.0 * kPi * (cycle + phaseR)));
            }
        }
        return true;
    }
    if (type == "chirp") {
        // A sine (or, with "noise": true, a band of noise) gliding from from_hz to to_hz between
        // `at` and `at` + `duration`, evenly in pitch, with 5 ms fades at both ends.
        const double from = std::clamp(numberOr(layer, "from_hz", 100.0), 5.0, sampleRate * 0.45);
        const double to = std::clamp(numberOr(layer, "to_hz", 1000.0), 5.0, sampleRate * 0.45);
        const auto start = static_cast<std::size_t>(std::clamp(numberOr(layer, "at", 0.0), 0.0, seconds) * sampleRate);
        const double duration = std::clamp(numberOr(layer, "duration", seconds), 0.01, 30.0) * sampleRate;
        const bool noise = boolOr(layer, "noise", false);
        const double damping = 1.0 / std::clamp(numberOr(layer, "q", 4.0), 0.3, 30.0);
        const double edge = 0.005 * sampleRate;
        double phase = 0.0, lowL = 0.0, bandL = 0.0, lowR = 0.0, bandR = 0.0;
        for (std::size_t i = start; i < length; ++i) {
            const double age = static_cast<double>(i - start);
            if (age >= duration)
                break;
            const double u = age / duration;
            const double f = from * std::pow(to / from, u);
            const double fade = std::min({1.0, age / edge, (duration - age) / edge});
            if (noise) {
                const double g = 2.0 * std::sin(kPi * std::min(f, sampleRate * 0.15) / sampleRate);
                const double inL = rng.uniform() * 2.0 - 1.0, inR = rng.uniform() * 2.0 - 1.0;
                lowL += g * bandL;
                bandL += g * (inL - lowL - damping * bandL);
                lowR += g * bandR;
                bandR += g * (inR - lowR - damping * bandR);
                mix.left[i] += static_cast<float>(level * fade * bandL * damping);
                mix.right[i] += static_cast<float>(level * fade * bandR * damping);
            } else {
                phase += f / sampleRate;
                const auto value = static_cast<float>(level * fade * std::sin(2.0 * kPi * phase));
                mix.left[i] += value;
                mix.right[i] += value;
            }
        }
        return true;
    }
    error = type.empty() ? "the layer has no \"type\"" : "unknown layer type \"" + type + "\"";
    return false;
}

// A layer with "fade_in" or "fade_out" (seconds) is made apart and faded in from the start or out
// towards the end of the texture's own length (`length`, not the folded-in overlap).
bool addTextureLayer(const J& layer, Stereo& mix, double sampleRate, std::size_t length, std::size_t loopLength, Rng& rng,
                     std::string& error) {
    const double fadeIn = std::max(0.0, numberOr(layer, "fade_in", 0.0)) * sampleRate;
    const double fadeOut = std::max(0.0, numberOr(layer, "fade_out", 0.0)) * sampleRate;
    if (fadeIn <= 0.0 && fadeOut <= 0.0)
        return textureLayer(layer, mix, sampleRate, loopLength, rng, error);
    Stereo own;
    own.left.assign(mix.left.size(), 0.0f);
    own.right.assign(mix.right.size(), 0.0f);
    if (!textureLayer(layer, own, sampleRate, loopLength, rng, error))
        return false;
    const auto raised = [](double u) { return 0.5 - 0.5 * std::cos(kPi * std::clamp(u, 0.0, 1.0)); };
    for (std::size_t i = 0; i < own.left.size(); ++i) {
        const double at = static_cast<double>(i);
        double gain = fadeIn > 0.0 ? raised(at / fadeIn) : 1.0;
        if (fadeOut > 0.0)
            gain *= raised((static_cast<double>(length) - at) / fadeOut);
        mix.left[i] += static_cast<float>(gain) * own.left[i];
        mix.right[i] += static_cast<float>(gain) * own.right[i];
    }
    return true;
}

} // namespace

std::string recipeKind(const J& recipe) {
    if (!recipe.is_object())
        return {};
    const std::string kind = stringOr(recipe, "kind", "");
    return kind == "table" || kind == "texture" || kind == "clip" ? kind : std::string();
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
        out = Wavetable{};
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
        out = Wavetable{};
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

bool applyTableSteps(const J& steps, Wavetable& table, std::string& error) {
    if (!steps.is_array() || steps.empty()) {
        error = "\"operations\" must be a list of steps";
        return false;
    }
    Wavetable work = table;
    int number = 0;
    for (const J& step : steps) {
        ++number;
        std::string problem;
        if (!step.is_object() || !tableStep(step, work, problem)) {
            error = "operation " + std::to_string(number) + (step.is_object() ? " (" + stringOr(step, "op", "?") + ")" : "") + ": " +
                    (step.is_object() ? problem : "an operation must be an object such as {\"op\": \"process\", \"name\": \"normalize\"}");
            return false;
        }
    }
    for (Frame& frame : work.frames)
        for (float& s : frame)
            if (!std::isfinite(s))
                s = 0.0f;
    table = std::move(work);
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
        if (!layer.is_object() || !addTextureLayer(layer, mix, sampleRate, length, loop ? length : 0, rng, problem)) {
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

// --- Clips -------------------------------------------------------------------------------------

namespace {

// "0:1.5:C5:0.8 1.5:0.5:Ab4" (commas also separate entries): start:length:key[:velocity] in beats.
bool parseClipNotes(const std::string& text, std::vector<ClipNote>& out, std::string& error) {
    std::size_t at = 0;
    int number = 0;
    while (at < text.size()) {
        const std::size_t end = std::min(text.find_first_of(" ,\t\r\n", at), text.size());
        const std::string entry = text.substr(at, end - at);
        at = end + 1;
        if (entry.empty())
            continue;
        ++number;
        std::vector<std::string> fields;
        std::size_t from = 0;
        while (from <= entry.size()) {
            const std::size_t colon = std::min(entry.find(':', from), entry.size());
            fields.push_back(entry.substr(from, colon - from));
            from = colon + 1;
        }
        const auto bad = [&](const std::string& what) {
            error = "note " + std::to_string(number) + " (\"" + entry + "\"): " + what;
            return false;
        };
        if (fields.size() < 3 || fields.size() > 4)
            return bad("write start:length:key or start:length:key:velocity");
        ClipNote note;
        try {
            std::size_t used = 0;
            note.start = std::stod(fields[0], &used);
            if (used != fields[0].size())
                return bad("the start is not a number");
            note.length = std::stod(fields[1], &used);
            if (used != fields[1].size() || !(note.length > 0.0))
                return bad("the length must be a number above 0");
            if (fields.size() == 4) {
                double velocity = std::stod(fields[3], &used);
                if (used != fields[3].size() || !(velocity > 0.0) || velocity > 127.0)
                    return bad("the velocity must be from 0 to 1, or 1 to 127");
                note.velocity = velocity > 1.0 ? velocity / 127.0 : velocity;
            }
        } catch (const std::exception&) {
            return bad("a number cannot be read");
        }
        const std::string& key = fields[2];
        int midi = parseNoteName(key);
        if (midi < 0 && !key.empty() && key.size() <= 3 && key.find_first_not_of("0123456789") == std::string::npos)
            midi = std::stoi(key);
        if (midi < 0 || midi > 127)
            return bad("the key must be a note name such as C4 or Ab3, or a MIDI number");
        note.key = midi;
        out.push_back(note);
    }
    return true;
}

} // namespace

bool buildClipRecipe(const J& recipe, Clip& out, std::string& error) {
    out = Clip{};
    if (recipeKind(recipe) != "clip") {
        error = "this is not a clip recipe (\"kind\" must be \"clip\")";
        return false;
    }
    out.name = recipeName(recipe);
    out.tempo = std::clamp(numberOr(recipe, "tempo", 120.0), 20.0, 999.0);
    out.beatsPerBar = std::clamp(intOr(recipe, "beats_per_bar", 4), 1, 16);
    out.bars = numberOr(recipe, "bars", 1.0);
    if (!(out.bars >= 1.0 && out.bars <= music::kMaxClipBars)) {
        error = "\"bars\" must be from 1 to " + std::to_string(static_cast<int>(music::kMaxClipBars));
        return false;
    }
    out.key = stringOr(recipe, "key", "");
    music::Key key;
    if (!out.key.empty() && !music::parseKey(out.key, key)) {
        error = "the key \"" + out.key + "\" cannot be read; write it as \"F minor\" or \"D major\"";
        return false;
    }
    if (key.valid()) // the tonic nearest middle C plays the clip as written
        out.root = 60 + (key.tonic + 6) % 12 - 6;
    const double total = out.lengthBeats();
    const auto parts = recipe.find("parts");
    if (parts == recipe.end() || !parts->is_array() || parts->empty()) {
        error = "the recipe has no \"parts\"";
        return false;
    }
    int number = 0;
    for (const J& part : *parts) {
        ++number;
        const std::string where = "part " + std::to_string(number) + ": ";
        if (!part.is_object()) {
            error = where + "a part must be an object";
            return false;
        }
        if (part.contains("chords")) {
            std::vector<music::Chord> chords;
            for (const J& symbol : part["chords"]) {
                music::Chord chord;
                std::string problem;
                if (!symbol.is_string() || !music::parseChordOrRoman(symbol.get<std::string>(), key.valid() ? &key : nullptr, chord, problem)) {
                    error = where + (symbol.is_string() ? "chord \"" + symbol.get<std::string>() + "\": " + problem : "chords must be text");
                    return false;
                }
                chords.push_back(chord);
            }
            const double span = numberOr(part, "beats_per_chord", static_cast<double>(out.beatsPerBar));
            if (chords.empty() || !(span > 0.0)) {
                error = where + "a chord part needs \"chords\" and a \"beats_per_chord\" above 0";
                return false;
            }
            // The chords repeat until the clip is full; each one is voice-led from the one before.
            std::vector<music::Chord> sequence;
            for (double at = 0.0; at < total - 1e-9; at += span)
                sequence.push_back(chords[sequence.size() % chords.size()]);
            const auto voicings = music::voiceLead(sequence, intOr(part, "centre", 60), intOr(part, "low", 52),
                                                   intOr(part, "high", 76), boolOr(part, "bass", false));
            std::vector<std::pair<double, double>> hits;
            if (const auto h = part.find("hits"); h != part.end() && h->is_array()) {
                for (const J& hit : *h)
                    if (hit.is_array() && hit.size() == 2 && hit[0].is_number() && hit[1].is_number())
                        hits.emplace_back(hit[0].get<double>(), hit[1].get<double>());
            }
            if (hits.empty())
                hits.emplace_back(0.0, span);
            const double velocity = std::clamp(numberOr(part, "velocity", 0.7), 0.01, 1.0);
            const double bassVelocity = std::clamp(numberOr(part, "bass_velocity", velocity), 0.01, 1.0);
            for (std::size_t c = 0; c < voicings.size(); ++c) {
                const double start = static_cast<double>(c) * span;
                for (const auto& [offset, length] : hits) {
                    for (const int n : voicings[c].notes)
                        out.notes.push_back({start + offset, length, n, velocity});
                    if (voicings[c].bass >= 0)
                        out.notes.push_back({start + offset, length, voicings[c].bass, bassVelocity});
                }
            }
        } else if (part.contains("notes")) {
            std::vector<ClipNote> notes;
            std::string problem;
            if (!part["notes"].is_string() || !parseClipNotes(part["notes"].get<std::string>(), notes, problem)) {
                error = where + (part["notes"].is_string() ? problem : "\"notes\" must be text such as \"0:1:C4 1:1:E4\"");
                return false;
            }
            const int repeat = std::clamp(intOr(part, "repeat", 1), 1, 256);
            const double every = numberOr(part, "every", total);
            for (int r = 0; r < repeat; ++r)
                for (ClipNote note : notes) {
                    note.start += r * every;
                    out.notes.push_back(note);
                }
        } else {
            error = where + "a part needs \"chords\" or \"notes\"";
            return false;
        }
    }
    std::stable_sort(out.notes.begin(), out.notes.end(),
                     [](const ClipNote& a, const ClipNote& b) { return a.start != b.start ? a.start < b.start : a.key < b.key; });
    const bool chromatic = boolOr(recipe, "chromatic", false);
    for (const music::ClipIssue& issue : music::validateClip(out, key)) {
        if (chromatic && !issue.error && issue.code == "outside_key")
            continue;
        error = issue.message + (issue.path.empty() ? "" : " (" + issue.path + ")") + "; " + issue.fix;
        out = Clip{};
        return false;
    }
    return true;
}

} // namespace zed

namespace zed {
namespace {

// A recipe's name as a file name: letters, digits, spaces, dashes and underscores.
std::string fileNameOf(const std::string& name) {
    std::string out;
    for (const char c : name)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')
            out += c;
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.substr(0, 80);
}


// Text with every line ending made LF, so copied files do not depend on how the folder was checked out.
std::vector<std::uint8_t> withLfEndings(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint8_t> out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i)
        if (!(bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n'))
            out.push_back(bytes[i]);
    return out;
}

// What is wrong with a factory file of this kind, or "" when it may ship. Factory files must load
// without errors and without warnings.
std::string checkFactoryFile(const std::string& extension, const std::string& text) {
    std::vector<Issue> issues;
    const auto first = [&](const char* what) {
        for (const Issue& i : issues)
            return std::string(what) + (i.severity == Severity::Error ? " error at " : " warning at ") +
                   (i.path.empty() ? std::string("(document)") : i.path) + ": " + i.message;
        return std::string(what) + " could not be read";
    };
    if (extension == ".zpreset") {
        const LoadResult loaded = loadPatch(text);
        issues = loaded.issues;
        if (!loaded.ok() || !issues.empty())
            return first("preset");
        if (loaded.patch.meta.name.empty())
            return "the preset has no name (meta.name)";
        return {};
    }
    if (extension == ".zshape") {
        Shape shape;
        if (!pasteShape(text, ShapeKind::Lfo, shape, issues) || !issues.empty())
            return first("shape");
        if (shape.points.size() < 2)
            return "a shape needs at least two points";
        return {};
    }
    if (extension == ".zrack") {
        Patch patch;
        if (!loadRackPreset(text, patch, issues) || !issues.empty())
            return first("rack preset");
        return {};
    }
    if (extension == ".zfx") {
        Patch patch;
        if (!loadSlotPreset(text, patch, 0, 0, nullptr, issues) || !issues.empty())
            return first("effect preset");
        return {};
    }
    return "unknown kind of file";
}

} // namespace

ContentBuild buildContentFolder(const std::string& recipesDir, const std::string& outDir) {
    namespace fs = std::filesystem;
    ContentBuild result;
    std::error_code ec;
    const fs::path root(recipesDir);
    std::vector<fs::path> recipes;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".json")
            recipes.push_back(it->path());
    if (ec || recipes.empty()) {
        result.errors.push_back("no recipes (*.json) were found in " + recipesDir);
        return result;
    }
    std::sort(recipes.begin(), recipes.end());
    const auto write = [&](const fs::path& relative, const std::vector<std::uint8_t>& bytes, const std::string& shown) {
        const fs::path target = fs::path(outDir) / relative;
        std::error_code made;
        fs::create_directories(target.parent_path(), made);
        std::string error;
        if (!writeFile(target.string(), bytes, error)) {
            result.errors.push_back(shown + ": " + error);
            return false;
        }
        result.written.push_back(relative.generic_string());
        return true;
    };
    for (const fs::path& file : recipes) {
        const std::string shown = file.filename().string();
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!readFile(file.string(), bytes, error)) {
            result.errors.push_back(shown + ": " + error);
            continue;
        }
        const J recipe = J::parse(bytes.begin(), bytes.end(), nullptr, false);
        const std::string kind = recipeKind(recipe);
        const std::string name = fileNameOf(recipeName(recipe));
        if (kind.empty() || name.empty()) {
            result.errors.push_back(shown + ": not a recipe (it needs \"kind\": \"table\", \"texture\" or \"clip\" and a \"name\")");
            continue;
        }
        if (kind == "table") {
            Wavetable table;
            if (!buildTableRecipe(recipe, table, error)) {
                result.errors.push_back(shown + ": " + error);
                continue;
            }
            const std::string category = fileNameOf(recipeCategory(recipe));
            if (write(fs::path("Wavetables") / (category.empty() ? fs::path() : fs::path(category)) / (name + ".wav"),
                      encodeWavetable(table), shown))
                ++result.tables;
        } else if (kind == "texture") {
            WavData texture;
            if (!buildTextureRecipe(recipe, texture, error)) {
                result.errors.push_back(shown + ": " + error);
                continue;
            }
            if (write(fs::path("Textures") / (name + ".wav"), encodeWavData(texture, WavSampleFormat::Pcm24), shown))
                ++result.textures;
        } else {
            Clip clip;
            if (!buildClipRecipe(recipe, clip, error)) {
                result.errors.push_back(shown + ": " + error);
                continue;
            }
            if (write(fs::path("Clips") / (name + ".mid"), midi::clipToFile(clip), shown))
                ++result.clips;
        }
    }

    // Presets, shapes and racks are checked and copied as they are, folders included.
    struct Kind {
        const char* from;
        const char* to;
        const char* extension;
        int ContentBuild::*count;
    };
    const Kind kinds[] = {{"presets", "Presets", ".zpreset", &ContentBuild::presets},
                          {"shapes", "Shapes", ".zshape", &ContentBuild::shapes},
                          {"racks", "Racks", ".zrack", &ContentBuild::racks},
                          {"racks", "Racks", ".zfx", &ContentBuild::racks}};
    for (const Kind& kind : kinds) {
        const fs::path base = root / kind.from;
        if (!fs::is_directory(base, ec))
            continue;
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == kind.extension)
                files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            const fs::path relative = fs::relative(file, base, ec);
            const std::string shown = (fs::path(kind.from) / relative).generic_string();
            std::vector<std::uint8_t> bytes;
            std::string error;
            if (ec || !readFile(file.string(), bytes, error)) {
                result.errors.push_back(shown + ": " + (error.empty() ? std::string("cannot be read") : error));
                continue;
            }
            const std::vector<std::uint8_t> text = withLfEndings(bytes);
            const std::string problem =
                checkFactoryFile(kind.extension, std::string(reinterpret_cast<const char*>(text.data()), text.size()));
            if (!problem.empty()) {
                result.errors.push_back(shown + ": " + problem);
                continue;
            }
            if (write(fs::path(kind.to) / relative, text, shown))
                ++(result.*(kind.count));
        }
    }
    return result;
}

} // namespace zed
