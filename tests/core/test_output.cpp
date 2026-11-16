// The output stage and the global settings (plan 3.12, 5.7): master volume after the effects, the
// DC filter, output protection, latency, and quality modes that switch without clicks.
#include "../support/Analysis.h"
#include "../support/EngineRig.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace zed;
using Catch::Approx;
using zedtest::EngineRig;

namespace {

Patch sinePatch() {
    Patch p = zedtest::flatPatch();
    p.set("osc1.on", 1);
    p.set("osc1.level", -6.0);
    return p;
}

int effect(const char* id) { return Registry::get().effectIndex(id); }

} // namespace

TEST_CASE("output: the DC filter removes an offset and lets nothing else change", "[output][dc]") {
    // A 25 % pulse from the Sub carries a large offset. (C5: well above the filter's 5 Hz corner.)
    Patch p = zedtest::flatPatch();
    p.set("sub.on", 1);
    p.set("sub.shape", Registry::get().find("sub.shape")->choiceIndex("pulse25"));
    p.set("global.dc_filter", 1);
    EngineRig on(p);
    on.run(512, {MidiEvent::noteOn(0, 84, 1.0f)});
    on.runBlocks(96000, 512);
    p.set("global.dc_filter", 0);
    EngineRig off(p);
    off.run(512, {MidiEvent::noteOn(0, 84, 1.0f)});
    off.runBlocks(96000, 512);
    const auto mean = [](const std::vector<float>& x, std::size_t from, std::size_t to) {
        double sum = 0.0;
        for (std::size_t i = from; i < to; ++i)
            sum += x[i];
        return sum / static_cast<double>(to - from);
    };
    CHECK(std::abs(mean(off.left, 48000, 96000)) > 0.1);
    CHECK(std::abs(mean(on.left, 48000, 96000)) < 0.01); // what is left is the part period at the window's end
    // The shape above 50 Hz is the same: only the offset went.
    std::vector<float> difference(off.left.size());
    for (std::size_t i = 0; i < difference.size(); ++i)
        difference[i] = off.left[i] - on.left[i];
    double wobble = 0.0;
    const double offset = mean(difference, 48000, 96000);
    for (std::size_t i = 48000; i < 96000; ++i)
        wobble = std::max(wobble, std::abs(difference[i] - offset));
    CHECK(wobble < 0.02);
}

TEST_CASE("output: protection keeps the output below full scale", "[output][limiter]") {
    Patch p;
    p.set("global.master", 12.0);
    p.set("osc1.unison.voices", 4);
    for (const char* mode : {"soft_clip", "limiter"}) {
        p.set("global.limiter", Registry::get().find("global.limiter")->choiceIndex(mode));
        EngineRig rig(p);
        rig.run(512, {MidiEvent::noteOn(0, 36, 1.0f), MidiEvent::noteOn(0, 43, 1.0f), MidiEvent::noteOn(0, 84, 1.0f),
                      MidiEvent::noteOn(0, 55, 1.0f)});
        rig.runBlocks(48000, 512);
        const float peak = std::max(zedtest::peakOf(rig.left, 0, rig.left.size()), zedtest::peakOf(rig.right, 0, rig.right.size()));
        INFO(mode << ": peak " << peak);
        CHECK(peak <= (std::string(mode) == "limiter" ? 0.96605088f : 1.0f));
        CHECK(peak > 0.5f);
    }
    // Off, the same patch goes far over.
    p.set("global.limiter", 0);
    EngineRig loud(p);
    loud.run(512, {MidiEvent::noteOn(0, 36, 1.0f), MidiEvent::noteOn(0, 43, 1.0f)});
    loud.runBlocks(24000, 512);
    CHECK(zedtest::peakOf(loud.left, 0, loud.left.size()) > 1.5f);
}

TEST_CASE("output: the master volume comes after the effects", "[output]") {
    const auto tail = [](double master) {
        Patch p = sinePatch();
        p.set("global.master", master);
        p.addFxSlot(2, effect("reverb"));
        EngineRig rig(p);
        rig.engine.setTable(0, zedtest::sineTableData());
        rig.run(512, {MidiEvent::noteOn(0, 57, 1.0f)});
        rig.runBlocks(4800, 512);
        rig.run(512, {MidiEvent::noteOff(0, 57)});
        rig.runBlocks(24000, 512);
        return 20.0 * std::log10(zedtest::rmsOf(rig.left, rig.left.size() - 12000, rig.left.size()));
    };
    CHECK(tail(-12.0) - tail(0.0) == Approx(-12.0).margin(0.01)); // the reverb tail follows the master
}

TEST_CASE("output: no latency in any quality mode, with or without effects", "[output][latency]") {
    for (const bool effects : {false, true}) {
        for (int quality = 0; quality < 4; ++quality) {
            Patch p = sinePatch();
            p.set("global.quality", quality);
            p.set("osc1.warp_a.mode", Registry::get().find("osc1.warp_a.mode")->choiceIndex("bend_plus"));
            if (effects) {
                p.addFxSlot(0, effect("drive"));
                p.addFxSlot(0, effect("eq"));
                p.addFxSlot(2, effect("compressor"));
            }
            EngineRig rig(p);
            CHECK(rig.engine.latencySamples() == 0);
            rig.run(512);
            rig.run(512, {MidiEvent::noteOn(100, 69, 1.0f)});
            INFO("quality " << quality << (effects ? " with effects" : ""));
            CHECK(zedtest::peakOf(rig.left, 0, 512 + 100) == 0.0f);
            CHECK(zedtest::peakOf(rig.left, 512 + 100, 512 + 140) > 1.0e-4f); // sound within a millisecond
        }
    }
}

TEST_CASE("output: switching quality while a note plays does not click", "[output][quality]") {
    Patch p = sinePatch();
    p.set("osc1.warp_a.mode", Registry::get().find("osc1.warp_a.mode")->choiceIndex("bend_plus"));
    p.set("osc1.warp_a.amount", 0.3);
    p.addFxSlot(0, effect("drive"));
    p.addFxSlot(2, effect("filter"));
    p.set(Registry::get().fxSlotParam(2, 0, 0), Registry::get().effectTypes()[static_cast<std::size_t>(effect("filter"))].params[0].choiceIndex("ladder_lp24"));
    p.set("global.limiter", Registry::get().find("global.limiter")->choiceIndex("soft_clip"));
    EngineRig rig(p);
    rig.engine.setTable(0, zedtest::sineTableData());
    rig.run(512, {MidiEvent::noteOn(0, 45, 1.0f)});
    rig.runBlocks(24000, 512);
    for (const int quality : {2, 3, 0, 1}) {
        const std::size_t at = rig.left.size();
        rig.set("global.quality", quality);
        rig.runBlocks(24000, 512);
        const double before = zedtest::largestJump(rig.left, at - 12000, at);
        const double after = zedtest::largestJump(rig.left, at + 12000, at + 24000);
        const double around = zedtest::largestJump(rig.left, at, at + 4800);
        INFO("to quality " << quality << ": " << around << " against " << before << " before and " << after << " after");
        CHECK(around <= 1.5 * std::max(before, after));
    }
    // The offline switch (a host rendering faster than real time) is the same kind of change.
    const std::size_t at = rig.left.size();
    rig.engine.setOffline(true);
    rig.runBlocks(24000, 512);
    CHECK(zedtest::largestJump(rig.left, at, at + 4800) <=
          1.5 * std::max(zedtest::largestJump(rig.left, at - 12000, at), zedtest::largestJump(rig.left, at + 12000, at + 24000)));
}

TEST_CASE("output: offline renders use the offline quality", "[output][quality]") {
    Patch p;
    p.set("filter1.on", 1);
    p.set("global.quality", 1);         // Standard: 2x
    p.set("global.quality_offline", 3); // Ultra: 8x
    EngineRig rig(p);
    rig.run(512);
    CHECK(rig.engine.filterView(0).oversample == 2);
    rig.engine.setOffline(true);
    rig.run(512);
    rig.run(512);
    CHECK(rig.engine.filterView(0).oversample == 8);
    rig.engine.setOffline(false);
    rig.run(512);
    rig.run(512);
    CHECK(rig.engine.filterView(0).oversample == 2);
}
