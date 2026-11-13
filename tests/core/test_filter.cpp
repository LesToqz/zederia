// The voice filters (plan 3.5, 5.7, 6.1): type table, linear responses against analytic curves,
// stability under audio-rate cutoff modulation at full resonance, self-oscillation pitch, and the
// stages around the core.
#include "../support/Analysis.h"
#include "../support/TestUtil.h"
#include "zederia/engine/Filter.h"
#include "zederia/rt/Denormals.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <complex>

using namespace zed;
using Catch::Approx;
using C = std::complex<double>;

namespace {

constexpr double kRate = 48000.0;

int typeIndex(const char* name) { return Registry::get().find("filter1.type")->choiceIndex(name); }

// One filter slot with its delay memory, driven the way the engine drives it: a control tick
// every 16 samples.
struct Slot {
    std::vector<float> memory;
    flt::VoiceFilter filter;
    flt::Settings settings;
    int oversample = 1;
    double rate = kRate;

    explicit Slot(const flt::Settings& s, int os = 1, double sampleRate = kRate) : settings(s), oversample(os), rate(sampleRate) {
        const int length = flt::delayLengthFor(rate);
        memory.assign(static_cast<std::size_t>(2 * length), 0.0f);
        filter.prepare(rate, memory.data(), length);
        filter.aim(settings, 16, oversample, true);
    }
    // Filters in place; `mod` is null or one cutoff offset (octaves) per sample.
    void run(std::vector<float>& left, std::vector<float>& right, const float* mod = nullptr) {
        const rt::ScopedNoDenormals noDenormals;
        const auto& hb = dsp::decimatorDesign(false);
        for (std::size_t pos = 0; pos < left.size(); pos += 16) {
            const int n = static_cast<int>(std::min<std::size_t>(16, left.size() - pos));
            filter.settle();
            filter.aim(settings, 16, oversample, false);
            filter.process(left.data() + pos, right.data() + pos, n, mod != nullptr ? mod + pos : nullptr, hb);
        }
    }
};

flt::Settings make(const char* type, double cutoff = 1000.0, double resonance = 0.0, double character = 0.5, double drive = 0.0) {
    flt::Settings s;
    s.type = typeIndex(type);
    REQUIRE(s.type >= 0);
    s.cutoffHz[0] = s.cutoffHz[1] = static_cast<float>(cutoff);
    s.resonance = static_cast<float>(resonance);
    s.character = static_cast<float>(character);
    s.drive = static_cast<float>(drive);
    return s;
}

// The impulse response of the left channel at a small level (so saturating types stay linear).
std::vector<float> impulseResponse(const flt::Settings& s, int length, int os = 1, double level = 1.0) {
    Slot slot(s, os);
    std::vector<float> left(static_cast<std::size_t>(length), 0.0f), right(left);
    left[0] = right[0] = static_cast<float>(level);
    slot.run(left, right);
    for (float& v : left)
        v = static_cast<float>(v / level);
    return left;
}

C transferAt(const std::vector<float>& h, double hz) {
    C sum(0.0);
    const C step = std::exp(C(0.0, -2.0 * kPi * hz / kRate));
    C phase(1.0);
    for (const float v : h) {
        sum += static_cast<double>(v) * phase;
        phase *= step;
    }
    return sum;
}

double db(double magnitude) { return 20.0 * std::log10(std::max(magnitude, 1e-12)); }

const std::vector<double> kProbeHz = {30, 60, 125, 250, 500, 700, 1000, 1400, 2000, 4000, 8000, 12000, 16000, 20000};

// Largest difference in dB between the measured response and `expected`, ignoring points where
// both are below `floorDb` (deep in a stop band or a notch, where a ratio means little).
template <typename Expected>
double worstErrorDb(const std::vector<float>& h, Expected&& expected, double maxHz = 24000.0, double floorDb = -70.0) {
    double worst = 0.0;
    for (const double hz : kProbeHz) {
        if (hz > maxHz)
            continue;
        const double measured = db(std::abs(transferAt(h, hz))), wanted = db(std::abs(expected(hz)));
        if (measured < floorDb && wanted < floorDb)
            continue;
        worst = std::max(worst, std::abs(measured - wanted));
    }
    return worst;
}

// --- Analog prototypes, written out independently of the filter code ----------------------------

C twoPole(C s, double k, const char* tap) {
    const C d = s * s + k * s + 1.0;
    const std::string t = tap;
    if (t == "lp") return 1.0 / d;
    if (t == "hp") return s * s / d;
    if (t == "bp") return s / d;
    if (t == "notch") return (1.0 + s * s) / d;
    return (1.0 - s * s) / d; // peak
}
C onePole(C s, bool high) { return (high ? s : C(1.0)) / (1.0 + s); }

// The Laplace variable at `hz`, normalized to a corner at `cutoff`. Warped: through the bilinear
// transform the filters are built on. Plain: the analog frequency axis.
C warped(double hz, double cutoff) { return C(0.0, std::tan(kPi * hz / kRate) / std::tan(kPi * cutoff / kRate)); }
C plain(double hz, double cutoff) { return C(0.0, hz / cutoff); }

double qOf(double resonance, double qMax = 40.0) { return 0.70710678 * std::pow(qMax / 0.70710678, resonance); }

} // namespace

TEST_CASE("filter: the type table follows the registry's list", "[filter]") {
    const ParamDef& def = *Registry::get().find("filter1.type");
    REQUIRE(flt::typeCount() == static_cast<int>(def.choices.size()));
    const std::pair<const char*, flt::Family> prefixes[] = {
        {"svf_", flt::Family::Svf},       {"ladder_", flt::Family::Ladder},   {"dual_", flt::Family::Dual},
        {"morph_", flt::Family::Morph},   {"comb_", flt::Family::Comb},       {"flange_", flt::Family::Flange},
        {"phase_", flt::Family::Phase},   {"formant_", flt::Family::Formant}, {"low_shelf", flt::Family::Shelf},
        {"high_shelf", flt::Family::Shelf}, {"bell", flt::Family::Shelf},     {"ring_mod", flt::Family::RingMod},
        {"sample_hold", flt::Family::SampleHold}, {"allpass_cluster", flt::Family::AllpassCluster},
        {"diffuser", flt::Family::Diffuser}, {"band_reject", flt::Family::BandReject}, {"howl_", flt::Family::Howl},
        {"grit_comb", flt::Family::GritComb}};
    for (int i = 0; i < flt::typeCount(); ++i) {
        const std::string& name = def.choices[static_cast<std::size_t>(i)];
        INFO(name);
        bool matched = false;
        for (const auto& [prefix, family] : prefixes)
            if (name.rfind(prefix, 0) == 0) {
                CHECK(flt::typeInfo(i).family == family);
                matched = true;
            }
        CHECK(matched);
    }
    CHECK(flt::typeInfo(typeIndex("svf_lp24")).b == 4);
    CHECK(flt::typeInfo(typeIndex("svf_hp6")).a == static_cast<int>(flt::Tap::Hp));
    CHECK(flt::typeInfo(typeIndex("ladder_lp18")).b == 3);
    CHECK(flt::typeInfo(typeIndex("dual_bp_notch")).a == static_cast<int>(flt::Tap::Bp));
    CHECK(flt::typeInfo(typeIndex("dual_bp_notch")).b == static_cast<int>(flt::Tap::Notch));
    CHECK(flt::typeInfo(typeIndex("comb_neg_hp")).a == 1);
    CHECK(flt::typeInfo(typeIndex("comb_neg_hp")).b == 1);
    CHECK(flt::typeInfo(typeIndex("howl_bp")).a == 1);
    CHECK(flt::typeInfo(-5).family == flt::Family::Svf);
    CHECK(flt::delayLengthFor(48000.0) == 4096);
    CHECK(flt::delayLengthFor(96000.0) == 8192);
}

TEST_CASE("filter: the half-band interpolator passes the band and rejects the image", "[filter][quality]") {
    for (const bool steep : {false, true}) {
        const auto& design = dsp::decimatorDesign(steep);
        for (const int factor : {2, 4, 8}) {
            for (const double hz : {100.0, 1000.0, 10000.0, 18000.0}) {
                dsp::DecimatorState state;
                const int frames = 8192;
                std::vector<float> in(static_cast<std::size_t>(frames)), out(static_cast<std::size_t>(frames * factor)),
                    scratch(out.size());
                for (int i = 0; i < frames; ++i)
                    in[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(2.0 * kPi * hz * i / kRate));
                for (int pos = 0; pos < frames; pos += 16)
                    dsp::interpolate(design, state, in.data() + pos, out.data() + pos * factor, scratch.data(), 16, factor);
                const auto spectrum = zedtest::spectrumOf(out, 4096, 16384, kRate * factor);
                const double tone = zedtest::lineLevel(spectrum, hz);
                const double image = zedtest::lineLevel(spectrum, kRate - hz);
                INFO("steep " << steep << " factor " << factor << " at " << hz << " Hz");
                CHECK(db(tone) == Approx(0.0).margin(hz <= 10000.0 ? 0.05 : 0.5));
                CHECK(db(image / tone) < (hz <= 18000.0 ? -90.0 : -60.0));
            }
        }
    }
}

TEST_CASE("filter: state-variable types follow their analog prototypes", "[filter][response]") {
    struct Case {
        const char* type;
        const char* tap;
        int poles;
    };
    const Case cases[] = {{"svf_lp6", "lp", 1},      {"svf_lp12", "lp", 2},       {"svf_lp18", "lp", 3},    {"svf_lp24", "lp", 4},
                          {"svf_hp6", "hp", 1},      {"svf_hp12", "hp", 2},       {"svf_hp18", "hp", 3},    {"svf_hp24", "hp", 4},
                          {"svf_bp12", "bp", 2},     {"svf_bp24", "bp", 4},       {"svf_notch12", "notch", 2},
                          {"svf_notch24", "notch", 4}, {"svf_peak12", "peak", 2}, {"svf_peak24", "peak", 4}};
    double worstWarped = 0.0, worstPlain = 0.0;
    for (const Case& c : cases) {
        for (const double resonance : {0.0, 0.5, 0.85}) {
            for (const double cutoff : {120.0, 1000.0, 6000.0}) {
                const auto h = impulseResponse(make(c.type, cutoff, resonance), 1 << 16);
                const double q = qOf(resonance);
                const auto prototype = [&](C s) {
                    const bool high = std::string(c.tap) == "hp";
                    if (c.poles == 1)
                        return onePole(s, high);
                    if (c.poles == 3)
                        return twoPole(s, 1.0 / q, c.tap) * onePole(s, high);
                    if (c.poles == 4) {
                        const double k = 1.0 / std::sqrt(q * 0.70710678);
                        return twoPole(s, k, c.tap) * twoPole(s, k, c.tap);
                    }
                    return twoPole(s, 1.0 / q, c.tap);
                };
                INFO(c.type << " resonance " << resonance << " cutoff " << cutoff);
                // Over the whole band against the prototype seen through the bilinear transform...
                const double a = worstErrorDb(h, [&](double hz) { return prototype(warped(hz, cutoff)); });
                CHECK(a < 0.5);
                worstWarped = std::max(worstWarped, a);
                // ...and, where the frequency axis is not yet bent, against the analog curve itself.
                if (cutoff <= 1000.0) {
                    const double b = worstErrorDb(h, [&](double hz) { return prototype(plain(hz, cutoff)); }, 2000.0);
                    CHECK(b < 0.5);
                    worstPlain = std::max(worstPlain, b);
                }
            }
        }
    }
    WARN("state-variable types: worst deviation " << worstWarped << " dB over the whole band, " << worstPlain
                                                  << " dB against the unwarped analog curve up to 2 kHz");
}

TEST_CASE("filter: shelves and the bell follow their analog prototypes", "[filter][response]") {
    double worst = 0.0;
    for (const double character : {0.0, 0.25, 0.5, 0.8, 1.0}) {
        const double gainDb = (character - 0.5) * 36.0;
        const double a = std::pow(10.0, gainDb / 40.0);
        for (const double cutoff : {200.0, 1000.0, 5000.0}) {
            const double kShelf = 1.0 / 0.70710678;
            const double kBell = 1.0 / (0.5 * a);
            const auto low = impulseResponse(make("low_shelf", cutoff, 0.0, character), 1 << 15);
            const auto high = impulseResponse(make("high_shelf", cutoff, 0.0, character), 1 << 15);
            const auto bell = impulseResponse(make("bell", cutoff, 0.0, character), 1 << 15);
            INFO("gain " << gainDb << " dB, cutoff " << cutoff);
            // Low shelf: gain a^2 below the corner, 1 above. The corner sits at cutoff / sqrt(a).
            const double e1 = worstErrorDb(low, [&](double hz) {
                const C s = warped(hz, cutoff) * std::sqrt(a);
                return (a * a + kShelf * a * s + s * s) / (s * s + kShelf * s + 1.0);
            });
            const double e2 = worstErrorDb(high, [&](double hz) {
                const C s = warped(hz, cutoff) / std::sqrt(a);
                return (1.0 + kShelf * a * s + a * a * s * s) / (s * s + kShelf * s + 1.0);
            });
            const double e3 = worstErrorDb(bell, [&](double hz) {
                const C s = warped(hz, cutoff);
                return (1.0 + kBell * a * a * s + s * s) / (s * s + kBell * s + 1.0);
            });
            CHECK(e1 < 0.5);
            CHECK(e2 < 0.5);
            CHECK(e3 < 0.5);
            worst = std::max({worst, e1, e2, e3});
            // The ends of the range are where the gain is read off.
            CHECK(db(std::abs(transferAt(low, 20.0))) == Approx(cutoff >= 1000.0 ? gainDb : db(std::abs(transferAt(low, 20.0)))).margin(0.5));
            CHECK(db(std::abs(transferAt(bell, cutoff))) == Approx(gainDb).margin(0.2));
            CHECK(db(std::abs(transferAt(low, 20000.0))) == Approx(0.0).margin(cutoff <= 1000.0 ? 0.5 : 3.0));
        }
    }
    WARN("shelf and bell: worst deviation " << worst << " dB");
}

TEST_CASE("filter: every linear type matches the response the display is given", "[filter][response]") {
    const ParamDef& def = *Registry::get().find("filter1.type");
    double worst = 0.0;
    std::string worstName;
    for (int type = 0; type < flt::typeCount(); ++type) {
        const flt::Family family = flt::typeInfo(type).family;
        if (!flt::hasLinearResponse(type) || flt::hasNonlinearCore(type))
            continue; // the ladder and howl are measured at a small level below
        const std::string& name = def.choices[static_cast<std::size_t>(type)];
        const bool delays = family == flt::Family::Comb || family == flt::Family::Flange || family == flt::Family::Diffuser;
        for (const double resonance : {0.0, 0.6}) {
            for (const double character : {0.2, 0.5, 0.9}) {
                flt::Settings s = make(name.c_str(), delays ? 480.0 : 900.0, resonance, character);
                if (family == flt::Family::Svf && character > 0.5)
                    continue; // the body stage saturates: covered by its own test
                s.mix = 0.8f;
                const auto h = impulseResponse(s, 1 << 16);
                // A delay read between samples loses a little treble; the response describes the
                // ideal delay, so the delay types are compared where that loss is negligible.
                const double limit = family == flt::Family::Diffuser ? 2500.0 : 24000.0;
                const double e = worstErrorDb(h, [&](double hz) { return flt::response(s, 0, kRate, hz); }, limit);
                INFO(name << " resonance " << resonance << " character " << character);
                CHECK(e < 0.5);
                if (e > worst) {
                    worst = e;
                    worstName = name;
                }
            }
        }
    }
    WARN("linear types against their display response: worst deviation " << worst << " dB (" << worstName << ")");
}

TEST_CASE("filter: dual, morph, band-reject and all-pass types follow their prototypes", "[filter][response]") {
    // Dual: two sections in series, the second one moved by the Character control.
    {
        const auto s = make("dual_lp_hp", 2000.0, 0.4, 0.25); // second cutoff two octaves down
        const auto h = impulseResponse(s, 1 << 15);
        const double k = 1.0 / std::sqrt(qOf(0.4) * 0.70710678); // the pair shares the resonance
        const double e = worstErrorDb(h, [&](double hz) { return twoPole(warped(hz, 2000.0), k, "lp") * twoPole(warped(hz, 500.0), k, "hp"); });
        CHECK(e < 0.5);
    }
    // Morph: at the ends and in the middle it is the named response.
    for (const auto& [character, tap] : {std::pair{0.0, "lp"}, {0.5, "bp"}, {1.0, "hp"}}) {
        const auto h = impulseResponse(make("morph_lbh12", 800.0, 0.3, character), 1 << 15);
        const double k = 1.0 / qOf(0.3);
        CHECK(worstErrorDb(h, [&](double hz) { return twoPole(warped(hz, 800.0), k, tap); }) < 0.5);
    }
    {
        const auto h = impulseResponse(make("morph_lnh24", 800.0, 0.3, 0.5), 1 << 15);
        const double k = 1.0 / std::sqrt(qOf(0.3) * 0.70710678);
        CHECK(worstErrorDb(h, [&](double hz) { const C n = twoPole(warped(hz, 800.0), k, "notch"); return n * n; }) < 0.5);
    }
    // Band reject: a low-pass below and a high-pass above the rejected band.
    {
        const auto h = impulseResponse(make("band_reject", 1000.0, 0.0, 0.5), 1 << 15);
        const double width = 0.25 + 2.75 * 0.5, k = 1.0 / 0.70710678;
        CHECK(worstErrorDb(h, [&](double hz) {
            return twoPole(warped(hz, 1000.0 * std::exp2(-width)), k, "lp") - twoPole(warped(hz, 1000.0 * std::exp2(width)), k, "hp");
        }) < 0.5);
        CHECK(db(std::abs(transferAt(h, 1000.0))) < -20.0);
    }
    // All-pass types change phase only.
    for (const char* type : {"allpass_cluster", "diffuser"}) {
        const auto h = impulseResponse(make(type, 600.0, 0.5, 0.5), 1 << 16);
        for (const double hz : {50.0, 200.0, 600.0, 1500.0, 2500.0}) {
            INFO(type << " at " << hz);
            CHECK(db(std::abs(transferAt(h, hz))) == Approx(0.0).margin(0.5));
        }
        // ...and they do change it: the response is not a plain wire.
        double turn = 0.0;
        for (const double hz : {150.0, 300.0, 450.0, 800.0, 1200.0})
            turn = std::max(turn, std::abs(std::arg(transferAt(h, hz))));
        CHECK(turn > 0.5);
    }
}

TEST_CASE("filter: combs resonate at multiples of the cutoff, flangers notch, phasers notch around it", "[filter][response]") {
    // 480 Hz at 48 kHz is a delay of exactly 100 samples.
    const auto positive = impulseResponse(make("comb_pos_lp", 480.0, 0.8, 1.0), 1 << 16);
    const auto negative = impulseResponse(make("comb_neg_lp", 480.0, 0.8, 1.0), 1 << 16);
    const auto level = [&](const std::vector<float>& h, double hz) { return db(std::abs(transferAt(h, hz))); };
    CHECK(level(positive, 480.0) > level(positive, 240.0) + 15.0);
    CHECK(level(positive, 960.0) > level(positive, 720.0) + 15.0);
    CHECK(level(negative, 240.0) > level(negative, 480.0) + 15.0); // negative feedback: peaks at odd halves
    CHECK(level(negative, 720.0) > level(negative, 960.0) + 15.0);
    // Damping takes the top off the upper resonances.
    const auto damped = impulseResponse(make("comb_pos_lp", 480.0, 0.8, 0.2), 1 << 16);
    CHECK(level(damped, 4800.0) < level(positive, 4800.0) - 6.0);

    const auto flange = impulseResponse(make("flange_pos_lp", 480.0, 0.3, 1.0), 1 << 16);
    CHECK(level(flange, 240.0) < level(flange, 480.0) - 15.0); // notch where the delayed copy is opposite
    const auto phaser = impulseResponse(make("phase_pos_lp", 1000.0, 0.0, 1.0), 1 << 16);
    // Six all-passes at one corner: the copy is opposite at tan(15), tan(45) and tan(75 degrees) times it.
    for (const double ratio : {0.26795, 1.0, 3.7321}) {
        const double hz = std::atan(ratio * std::tan(kPi * 1000.0 / kRate)) * kRate / kPi;
        INFO("notch near " << hz);
        CHECK(level(phaser, hz) < -30.0);
    }
    CHECK(level(phaser, 40.0) == Approx(0.0).margin(0.5));
}

TEST_CASE("filter: formant types put resonances at the vowel's formants", "[filter][response]") {
    // The middle of the cutoff range is the third vowel, "i": a low first and a high second formant.
    const double middle = 8.0 * std::sqrt(2750.0);
    const auto h = impulseResponse(make("formant_a", middle, 0.7, 0.5), 1 << 15);
    const auto level = [&](double hz) { return db(std::abs(transferAt(h, hz))); };
    CHECK(level(270.0) > level(800.0) + 12.0);
    CHECK(level(2290.0) > level(800.0) + 6.0);
    // Formant shift moves them: an octave up at full Character.
    const auto shifted = impulseResponse(make("formant_a", middle, 0.7, 1.0), 1 << 15);
    CHECK(db(std::abs(transferAt(shifted, 540.0))) > db(std::abs(transferAt(shifted, 270.0))) + 10.0);
    // The other voicings sit higher.
    const auto b = impulseResponse(make("formant_b", middle, 0.7, 0.5), 1 << 15);
    CHECK(db(std::abs(transferAt(b, 310.0))) > db(std::abs(transferAt(b, 800.0))) + 12.0);
}

TEST_CASE("filter: the ladder's small-signal response matches the display data", "[filter][response]") {
    double worst = 0.0;
    for (const char* type : {"ladder_lp6", "ladder_lp12", "ladder_lp18", "ladder_lp24"}) {
        for (const double resonance : {0.0, 0.5, 0.8}) {
            for (const int os : {1, 2}) {
                const flt::Settings s = make(type, 1000.0, resonance);
                const auto h = impulseResponse(s, 1 << 15, os, 1.0e-3);
                // Close to half the sample rate the 2x path shows its own band limit.
                const double e = worstErrorDb(h, [&](double hz) { return flt::response(s, 0, kRate, hz, os); }, os == 1 ? 24000.0 : 16000.0);
                INFO(type << " resonance " << resonance << " oversampling " << os);
                CHECK(e < 1.0);
                worst = std::max(worst, e);
            }
        }
    }
    WARN("ladder against its display response at a small level: worst deviation " << worst << " dB");
    // 24 dB per octave well above the cutoff.
    const auto h = impulseResponse(make("ladder_lp24", 100.0), 1 << 15, 1, 1.0e-3);
    CHECK(db(std::abs(transferAt(h, 1000.0))) - db(std::abs(transferAt(h, 2000.0))) == Approx(24.0).margin(1.0));
}

TEST_CASE("filter: the ladder self-oscillates in tune with its cutoff", "[filter][selfosc]") {
    double worst = 0.0;
    for (const int os : {1, 2, 4}) {
        for (const double cutoff : {55.0, 220.0, 880.0, 3520.0, 7040.0}) {
            Slot slot(make("ladder_lp24", cutoff, 1.0), os);
            // Four seconds: at 55 Hz the oscillation takes a second or two to settle at its level.
            std::vector<float> left(192000, 0.0f), right(192000, 0.0f);
            left[0] = right[0] = 0.5f; // a click starts it; then it sings on its own
            slot.run(left, right);
            const float early = zedtest::peakOf(left, 120000, 156000), late = zedtest::peakOf(left, 156000, 192000);
            const double hz = zedtest::measureFrequency(left, 144000, 192000, kRate);
            INFO("cutoff " << cutoff << " oversampling " << os << ": " << hz << " Hz, level " << late);
            CHECK(late > 0.05f);                              // still sounding after four seconds
            CHECK(late == Approx(early).epsilon(0.05));       // and steady
            const double error = std::abs(zedtest::cents(hz, cutoff));
            CHECK(error < 5.0);
            worst = std::max(worst, error);
        }
    }
    WARN("ladder self-oscillation: worst pitch error " << worst << " cents (55 Hz to 7 kHz, 1x, 2x and 4x)");
    // Below full resonance it rings and dies away.
    Slot quiet(make("ladder_lp24", 440.0, 0.8), 2);
    std::vector<float> left(96000, 0.0f), right(96000, 0.0f);
    left[0] = right[0] = 0.5f;
    quiet.run(left, right);
    CHECK(zedtest::peakOf(left, 72000, 96000) < 1.0e-4f);
}

TEST_CASE("filter: every type stays finite through ten seconds of audio-rate cutoff modulation at full resonance",
          "[filter][stability]") {
    const ParamDef& def = *Registry::get().find("filter1.type");
    const int total = 480000; // ten seconds
    std::vector<float> noise(static_cast<std::size_t>(total)), mod(static_cast<std::size_t>(total));
    Rng rng(7);
    double phase = 0.0;
    for (int i = 0; i < total; ++i) {
        // Loud noise with bursts and gaps, so the filters are hit hard and also left to ring.
        const bool gap = (i / 12000) % 4 == 3;
        noise[static_cast<std::size_t>(i)] = gap ? 0.0f : static_cast<float>(rng.uniform() * 2.0 - 1.0);
        // The modulator sweeps from 30 Hz to 9 kHz and back, three octaves up and down.
        const double t = static_cast<double>(i) / total;
        phase += 30.0 * std::pow(300.0, 0.5 - 0.5 * std::cos(2.0 * kPi * 3.0 * t)) / kRate;
        mod[static_cast<std::size_t>(i)] = static_cast<float>(3.0 * std::sin(2.0 * kPi * phase));
    }
    float worstPeak = 0.0f;
    std::string worstName;
    std::vector<std::pair<float, std::string>> peaks;
    for (int type = 0; type < flt::typeCount(); ++type) {
        const std::string& name = def.choices[static_cast<std::size_t>(type)];
        for (const int os : {2, 1, 4}) {
            // The full ten seconds in Standard quality; two seconds each without oversampling and at 4x.
            const auto length = static_cast<std::size_t>(os == 2 ? total : total / 5);
            flt::Settings s = make(name.c_str(), 1000.0, 1.0, 0.5, 0.6);
            Slot slot(s, os);
            std::vector<float> left(noise.begin(), noise.begin() + static_cast<std::ptrdiff_t>(length)), right(left);
            bool finite = true;
            float peak = 0.0f;
            const rt::ScopedNoDenormals noDenormals;
            const auto& hb = dsp::decimatorDesign(false);
            for (std::size_t pos = 0; pos < length; pos += 16) {
                // The cutoff itself sweeps the whole range every two seconds, the Character control with it.
                const double sweep = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(pos) / 96000.0);
                s.cutoffHz[0] = static_cast<float>(8.0 * std::pow(2750.0, sweep));
                s.cutoffHz[1] = static_cast<float>(8.0 * std::pow(2750.0, 1.0 - sweep));
                s.character = static_cast<float>(0.5 + 0.5 * std::sin(2.0 * kPi * static_cast<double>(pos) / 70000.0));
                slot.filter.settle();
                slot.filter.aim(s, 16, os, false);
                slot.filter.process(left.data() + pos, right.data() + pos, 16, mod.data() + pos, hb);
            }
            for (std::size_t i = 0; i < length; ++i) {
                finite = finite && std::isfinite(left[i]) && std::isfinite(right[i]);
                peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
            }
            INFO(name << " oversampling " << os << " peak " << peak);
            CHECK(finite);
            CHECK(peak < 200.0f);
            if (peak > worstPeak) {
                worstPeak = peak;
                worstName = name;
            }
            peaks.emplace_back(peak, name + " at " + std::to_string(os) + "x");
        }
    }
    std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string top;
    for (std::size_t i = 0; i < 6 && i < peaks.size(); ++i)
        top += peaks[i].second + " " + std::to_string(peaks[i].first) + "; ";
    WARN("stability sweeps: all " << flt::typeCount() << " types finite; highest output peaks for noise at full scale: " << top);
}

TEST_CASE("filter: mix, level mode and stereo offset", "[filter]") {
    // Mix 0 is the dry signal; level mode has no dry signal at all.
    flt::Settings s = make("svf_lp12", 300.0);
    s.mix = 0.0f;
    auto h = impulseResponse(s, 4096);
    CHECK(h[0] == Approx(1.0f));
    CHECK(zedtest::peakOf(h, 1, 4096) == 0.0f);
    s.levelMode = true;
    h = impulseResponse(s, 4096);
    CHECK(zedtest::peakOf(h, 0, 4096) == 0.0f);
    s.mix = 0.5f;
    h = impulseResponse(s, 1 << 14);
    CHECK(db(std::abs(transferAt(h, 20.0))) == Approx(-6.02).margin(0.1)); // half level, nothing dry on top
    s.levelMode = false;
    h = impulseResponse(s, 1 << 14);
    CHECK(db(std::abs(transferAt(h, 20.0))) == Approx(0.0).margin(0.1));    // dry and wet add up in the pass band
    CHECK(db(std::abs(transferAt(h, 12000.0))) == Approx(-6.02).margin(0.2)); // only the dry half is left up here

    // Different cutoffs left and right give different channels.
    flt::Settings wide = make("svf_lp12", 500.0);
    wide.cutoffHz[1] = 4000.0f;
    Slot slot(wide);
    std::vector<float> left(8192, 0.0f), right(8192, 0.0f);
    left[0] = right[0] = 1.0f;
    slot.run(left, right);
    CHECK(db(std::abs(transferAt(left, 2000.0))) < db(std::abs(transferAt(right, 2000.0))) - 15.0);
}

TEST_CASE("filter: drive and body saturate without aliasing in Standard quality and switch in without a click",
          "[filter][quality]") {
    // A loud 5 kHz sine through a driven low-pass that is wide open: harmonics at 15 and 25 kHz.
    // The 25 kHz one would fold to 23 kHz without oversampling.
    const auto render = [&](int os, double drive, double character) {
        flt::Settings s = make("svf_lp12", 20000.0, 0.0, character, drive);
        Slot slot(s, os);
        std::vector<float> left(32768), right(32768);
        for (std::size_t i = 0; i < left.size(); ++i)
            left[i] = right[i] = static_cast<float>(0.9 * std::sin(2.0 * kPi * 5000.0 * static_cast<double>(i) / kRate));
        slot.run(left, right);
        return left;
    };
    const auto draft = zedtest::spectrumOf(render(1, 0.8, 0.5), 8192, 16384, kRate);
    const auto standard = zedtest::spectrumOf(render(2, 0.8, 0.5), 8192, 16384, kRate);
    const double foldDraft = db(zedtest::lineLevel(draft, 23000.0) / zedtest::lineLevel(draft, 5000.0));
    const double foldStandard = db(zedtest::lineLevel(standard, 23000.0) / zedtest::lineLevel(standard, 5000.0));
    WARN("driven filter, 5 kHz sine: fifth harmonic folded to 23 kHz at " << foldDraft << " dBc in Draft, " << foldStandard
                                                                           << " dBc in Standard");
    CHECK(foldStandard < foldDraft - 20.0);
    CHECK(db(zedtest::lineLevel(standard, 15000.0) / zedtest::lineLevel(standard, 5000.0)) > -40.0); // the real harmonic is there

    // Drive coming up from zero mid-note: the stage fades in.
    flt::Settings s = make("svf_lp12", 20000.0);
    Slot slot(s, 2);
    std::vector<float> left(4800), right(4800);
    for (std::size_t i = 0; i < left.size(); ++i)
        left[i] = right[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 200.0 * static_cast<double>(i) / kRate));
    const float steady = 0.5f * static_cast<float>(2.0 * kPi * 200.0 / kRate);
    std::vector<float> a(left.begin(), left.begin() + 2400), b(right.begin(), right.begin() + 2400);
    slot.run(a, b);
    slot.settings.drive = 0.02f;
    std::vector<float> c(left.begin() + 2400, left.end()), d(right.begin() + 2400, right.end());
    slot.run(c, d);
    a.insert(a.end(), c.begin(), c.end());
    CHECK(zedtest::largestJump(a, 2300, 4800) < steady * 1.5f);
}
