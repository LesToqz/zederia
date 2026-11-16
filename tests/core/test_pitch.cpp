// Tuning accuracy of the placeholder oscillator: within +/-0.1 cent from C0 to C8 (plan 5.7).
#include "../support/TestUtil.h"
#include "zederia/util/Math.h"

#include <catch2/catch_test_macros.hpp>

using namespace zed;

namespace {

// Renders a held note and measures its pitch over the sustained part.
double measuredCents(const Patch& patch, int note, double expectedHz, double sampleRate) {
    const double seconds = 2.5;
    const RenderResult r = zedtest::renderNote(patch, note, seconds, sampleRate);
    const auto from = static_cast<std::size_t>(0.2 * sampleRate);
    const auto to = static_cast<std::size_t>((seconds - 0.05) * sampleRate);
    const double measured = zedtest::measureFrequency(r.left, from, to, sampleRate);
    REQUIRE(measured > 0.0);
    return zedtest::cents(measured, expectedHz);
}

} // namespace

TEST_CASE("tuning: within 0.1 cent from C0 to C8", "[tuning]") {
    Patch patch;
    patch.set("global.dc_filter", 0); // the oscillator itself, not the output stage
    patch.set("osc1.phase_random", 0);
    double worst = 0.0;
    for (const double sampleRate : {44100.0, 48000.0, 96000.0}) {
        for (int note = 12; note <= 108; note += 4) { // C0 .. C8 in major thirds
            const double error = measuredCents(patch, note, noteToHz(note), sampleRate);
            INFO("note " << note << " at " << sampleRate << " Hz: " << error << " cents");
            CHECK(std::abs(error) < 0.1);
            worst = std::max(worst, std::abs(error));
        }
    }
    // The measurement itself is far finer than the requirement; report the margin.
    WARN("worst tuning error C0..C8: " << worst << " cents");
    CHECK(worst < 0.01);
}

TEST_CASE("tuning: offsets, master tune and reference pitch add up", "[tuning]") {
    Patch patch;
    patch.set("osc1.phase_random", 0);
    const double rate = 48000.0;

    patch.set("osc1.octave", -1);
    patch.set("osc1.semi", 7);
    patch.set("osc1.fine", 25);
    patch.set("osc1.coarse", -2.5);
    CHECK(std::abs(measuredCents(patch, 60, noteToHz(60 - 12 + 7 + 0.25 - 2.5), rate)) < 0.1);

    Patch tuned;
    tuned.set("osc1.phase_random", 0);
    tuned.set("global.master_tune", -37.3);
    CHECK(std::abs(measuredCents(tuned, 69, 440.0 * std::exp2(-37.3 / 1200.0), rate)) < 0.1);
    tuned.set("global.master_tune", 0);
    tuned.set("global.reference", 432);
    CHECK(std::abs(measuredCents(tuned, 69, 432.0, rate)) < 0.1);
    CHECK(std::abs(measuredCents(tuned, 57, 216.0, rate)) < 0.1);

    Patch fixed;
    fixed.set("osc1.phase_random", 0);
    fixed.set("osc1.keytrack", 0); // fixed pitch: middle C whatever the key
    CHECK(std::abs(measuredCents(fixed, 96, noteToHz(60), rate)) < 0.1);
    CHECK(std::abs(measuredCents(fixed, 30, noteToHz(60), rate)) < 0.1);
}
