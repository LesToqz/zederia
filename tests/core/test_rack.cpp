// The effects rack in the engine (plan 3.8, 5.3, 5.5, 6.1): lanes and sends, click-free edits while
// audio plays, idle effects, FX off, rack lock, rack and effect presets, effect parameters as
// modulation destinations, and real-time safety while another thread edits the rack.
#include "../support/Analysis.h"
#include "../support/EngineRig.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/rt/AudioThreadGuard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <functional>
#include <stdexcept>
#include <thread>

using namespace zed;
using Catch::Approx;
using zedtest::EngineRig;

namespace {

const Registry& reg() { return Registry::get(); }

int effect(const char* id) { return Registry::get().effectIndex(id); }

int paramOf(int type, const char* name) {
    const auto& params = reg().effectTypes()[static_cast<std::size_t>(type)].params;
    for (std::size_t k = 0; k < params.size(); ++k)
        if (params[k].id.substr(params[k].id.rfind('.') + 1) == name)
            return static_cast<int>(k);
    throw std::runtime_error(std::string("no such effect parameter: ") + name);
}

void setFx(Patch& p, int lane, int slot, const char* name, double value) {
    p.set(reg().fxSlotParam(lane, slot, paramOf(p.fxSlotType(lane, slot), name)), value);
}

int addFx(Patch& p, int lane, const char* id) { return p.addFxSlot(lane, effect(id)); }

// A sustained sine voice: what a click would stand out against.
Patch sinePatch() {
    Patch p = zedtest::flatPatch();
    p.set("osc1.on", 1);
    p.set("osc1.level", -6.0);
    p.set("env1.release", 20.0);
    return p;
}

void useSine(EngineRig& rig) { rig.engine.setTable(0, zedtest::sineTableData()); }

double level(const std::vector<float>& x, std::size_t from, std::size_t to) { return 20.0 * std::log10(zedtest::rmsOf(x, from, to)); }

} // namespace

TEST_CASE("rack: edits move values, curves and routes with their effect", "[rack][patch]") {
    Patch p;
    addFx(p, 0, "reverb");
    addFx(p, 0, "delay");
    setFx(p, 0, 0, "decay", 4321.0);
    setFx(p, 0, 1, "feedback", 0.77);
    p.fxCurves[0][1][0] = zedtest::lineShape(0.0, 1.0);
    zedtest::addRoute(p, "lfo1", "fx.a2.p10", 0.3); // the delay's feedback
    zedtest::addRoute(p, "lfo2", "fx.a1.p3", 0.2);  // the reverb's decay

    FxMoves moves;
    REQUIRE(p.moveFxSlot(0, 1, 0, 0, &moves)); // the delay to the front
    CHECK(p.fxSlotType(0, 0) == effect("delay"));
    CHECK(p.fxSlotType(0, 1) == effect("reverb"));
    CHECK(p.get(reg().fxSlotParam(0, 0, paramOf(effect("delay"), "feedback"))) == Approx(0.77));
    CHECK(p.get(reg().fxSlotParam(0, 1, paramOf(effect("reverb"), "decay"))) == Approx(4321.0));
    CHECK_FALSE(p.fxCurves[0][0][0].empty());
    CHECK(p.fxCurves[0][1][0].empty());
    CHECK(p.routes[0].dest == "fx.a1.p10");
    CHECK(p.routes[1].dest == "fx.a2.p3");
    CHECK(moves.from[0] == 1);
    CHECK(moves.from[1] == 0);

    REQUIRE(p.moveFxSlot(0, 1, 2, 0, &moves)); // the reverb to the master lane
    CHECK(p.fxSlotCount(0) == 1);
    CHECK(p.fxSlotType(2, 0) == effect("reverb"));
    CHECK(p.routes[1].dest == "fx.m1.p3");
    CHECK(moves.from[2 * kNumFxSlots] == 1);

    REQUIRE(p.insertFxSlot(0, 0, effect("eq"), &moves));
    CHECK(p.fxSlotType(0, 1) == effect("delay"));
    CHECK(p.routes[0].dest == "fx.a2.p10");
    CHECK(moves.from[0] == -1);
    CHECK(moves.from[1] == 0);

    REQUIRE(p.removeFxSlot(0, 1, &moves)); // the delay goes, and so does its route
    REQUIRE(p.routes.size() == 1);
    CHECK(p.routes[0].dest == "fx.m1.p3");
    CHECK(p.get(reg().modAmount(0)) == Approx(0.2));

    CHECK_FALSE(p.insertFxSlot(0, 5, effect("eq"))); // past the end of the lane
    CHECK_FALSE(p.removeFxSlot(1, 0));

    // The edited patch saves and loads unchanged.
    const LoadResult again = loadPatch(savePatch(p));
    REQUIRE(again.ok());
    CHECK(again.patch == p);
}

TEST_CASE("rack: lanes A and B in parallel or in series, then Master", "[rack]") {
    Patch base = sinePatch();
    base.set("fx.filter2_balance", 0.5); // the voices reach the rack on the filter 2 bus
    const auto render = [](const Patch& p) {
        EngineRig rig(p);
        useSine(rig);
        rig.run(512, {MidiEvent::noteOn(0, 57, 1.0f)});
        rig.runBlocks(48000, 512);
        return level(rig.left, 24000, 48000);
    };
    const double dry = render(base);

    Patch p = base;
    addFx(p, 0, "utility");
    setFx(p, 0, 0, "gain", 20.0 * std::log10(0.5));
    addFx(p, 1, "utility");
    setFx(p, 1, 0, "gain", 20.0 * std::log10(0.25));
    // Parallel: half the signal at x 0.5 plus half at x 0.25.
    CHECK(render(p) - dry == Approx(20.0 * std::log10(0.375)).margin(0.05));
    // Series: lane B hears lane A's output as well as its own share.
    p.set("fx.b_routing", 1);
    CHECK(render(p) - dry == Approx(20.0 * std::log10((0.25 + 0.5) * 0.25)).margin(0.05));
    // Master follows both.
    addFx(p, 2, "utility");
    setFx(p, 2, 0, "gain", 20.0 * std::log10(2.0));
    CHECK(render(p) - dry == Approx(20.0 * std::log10(0.375)).margin(0.05));
}

TEST_CASE("rack: inserting, moving, removing and switching effects while a note plays does not click", "[rack][click]") {
    Patch p = sinePatch();
    addFx(p, 0, "chorus");
    addFx(p, 0, "eq");
    setFx(p, 0, 1, "mid1_gain", 6.0);
    EngineRig rig(p);
    useSine(rig);
    rig.run(512, {MidiEvent::noteOn(0, 45, 1.0f)});
    rig.runBlocks(24000, 512);

    struct Edit {
        const char* name;
        std::function<void(Patch&, FxMoves&)> apply;
    };
    const std::vector<Edit> edits = {
        {"insert a delay at the front", [](Patch& q, FxMoves& m) { q.insertFxSlot(0, 0, effect("delay"), &m); }},
        {"insert a reverb in lane B", [](Patch& q, FxMoves& m) { q.insertFxSlot(1, 0, effect("reverb"), &m); }},
        {"move the EQ before the chorus", [](Patch& q, FxMoves& m) { q.moveFxSlot(0, 2, 0, 1, &m); }},
        {"move the chorus to Master", [](Patch& q, FxMoves& m) { q.moveFxSlot(0, 2, 2, 0, &m); }},
        {"remove the delay", [](Patch& q, FxMoves& m) { q.removeFxSlot(0, 0, &m); }},
        {"insert a drive", [](Patch& q, FxMoves& m) { q.insertFxSlot(0, 1, effect("drive"), &m); }},
    };
    for (const Edit& edit : edits) {
        FxMoves moves;
        edit.apply(p, moves);
        const std::size_t at = rig.left.size();
        rig.engine.setRack(p, &moves);
        rig.runBlocks(24000, 512);
        const double before = zedtest::largestJump(rig.left, at - 12000, at);
        const double after = zedtest::largestJump(rig.left, at + 12000, at + 24000);
        const double around = zedtest::largestJump(rig.left, at, at + 2400);
        INFO(edit.name << ": " << around << " against " << before << " before and " << after << " after");
        WARN("rack edit, " << edit.name << ": largest step " << around / std::max(before, after) << " times the steady signal's");
        CHECK(around <= 1.5 * std::max(before, after));
    }
    // Power switches, lane B routing and FX off.
    const std::vector<std::pair<const char*, std::function<void()>>> switches = {
        {"switch the drive off", [&] { rig.set("fx.a2.on", 0); }},
        {"switch it back on", [&] { rig.set("fx.a2.on", 1); }},
        {"lane B in series", [&] { rig.set("fx.b_routing", 1); }},
        {"FX off", [&] { rig.set("global.fx_off", 1); }},
        {"FX on", [&] { rig.set("global.fx_off", 0); }},
    };
    for (const auto& [name, apply] : switches) {
        const std::size_t at = rig.left.size();
        apply();
        rig.runBlocks(24000, 512);
        const double before = zedtest::largestJump(rig.left, at - 12000, at);
        const double after = zedtest::largestJump(rig.left, at + 12000, at + 24000);
        const double around = zedtest::largestJump(rig.left, at, at + 2400);
        INFO("largest jump " << zedtest::largestJumpAt(rig.left, at, at + 2400) - at << " samples after the change");
        INFO(name << ": " << around << " against " << before << " before and " << after << " after");
        WARN("rack switch, " << name << ": largest step " << around / std::max(before, after) << " times the steady signal's");
        CHECK(around <= 1.5 * std::max(before, after));
    }
}

TEST_CASE("rack: an effect idles once its tail has died away, and wakes with new input", "[rack][idle]") {
    Patch p = sinePatch();
    addFx(p, 0, "delay");
    setFx(p, 0, 0, "sync", 0);
    setFx(p, 0, 0, "time_l", 150.0);
    setFx(p, 0, 0, "feedback", 0.3);
    addFx(p, 0, "reverb");
    setFx(p, 0, 1, "decay", 400.0);
    addFx(p, 2, "eq");
    EngineRig rig(p);
    useSine(rig);
    rig.run(512, {MidiEvent::noteOn(0, 57, 1.0f)});
    rig.runBlocks(24000, 512);
    CHECK(rig.engine.fxSlotsRunning() == 3);
    CHECK(rig.engine.fxSlotStatus(0, 0).running);
    rig.run(512, {MidiEvent::noteOff(0, 57)});
    rig.runBlocks(48000 * 4, 512);
    for (int lane = 0; lane < kNumFxLanes; ++lane)
        for (int slot = 0; slot < p.fxSlotCount(lane); ++slot) {
            INFO("lane " << lane << " slot " << slot);
            CHECK(rig.engine.fxSlotStatus(lane, slot).idle);
            CHECK_FALSE(rig.engine.fxSlotStatus(lane, slot).running);
        }
    CHECK(rig.engine.fxSlotsRunning() == 0);
    CHECK(zedtest::peakOf(rig.left, rig.left.size() - 4800, rig.left.size()) == 0.0f);
    rig.run(512, {MidiEvent::noteOn(0, 57, 1.0f)});
    rig.run(512);
    CHECK(rig.engine.fxSlotsRunning() == 1); // the delay wakes; what follows it hears nothing for 150 ms
    rig.runBlocks(9600, 512);
    CHECK(rig.engine.fxSlotsRunning() == 3);
    CHECK(zedtest::peakOf(rig.left, rig.left.size() - 512, rig.left.size()) > 0.01f);

    // A frozen reverb never idles; an effect switched off never runs.
    Patch frozen = sinePatch();
    addFx(frozen, 0, "reverb");
    setFx(frozen, 0, 0, "freeze", 1);
    addFx(frozen, 0, "chorus");
    frozen.set(reg().fxSlotOn(0, 1), 0);
    EngineRig held(frozen);
    useSine(held);
    held.run(512, {MidiEvent::noteOn(0, 57, 1.0f)});
    held.runBlocks(48000, 512);
    held.run(512, {MidiEvent::noteOff(0, 57)});
    held.runBlocks(48000 * 3, 512);
    CHECK(held.engine.fxSlotStatus(0, 0).running);
    CHECK_FALSE(held.engine.fxSlotStatus(0, 1).running);
    CHECK_FALSE(held.engine.fxSlotStatus(0, 1).on);
}

TEST_CASE("rack: a new layout takes over from sleeping effects at once, and none of them plays another effect's values",
          "[rack][idle][preset]") {
    // Browsing presets: by the time the next one loads, the last one's effects sleep. The new
    // layout must not wait for the next note to replace them, and an effect still in place must
    // never run with the values the store already holds for the new layout, in another effect's
    // units: the compressor here would take the reverb's 12 kHz high cut as 12000 dB of make-up
    // gain. The infinity and NaN that followed stayed in the DC filter and the engine was silent
    // until it was reset.
    Patch first = sinePatch();
    first.set("global.dc_filter", 1);
    first.set("osc1.level", 0.0); // a full-scale sine
    addFx(first, 0, "drive");
    addFx(first, 2, "compressor");
    addFx(first, 2, "utility");
    Patch second = sinePatch();
    second.set("global.dc_filter", 1);
    second.set("osc1.level", 0.0);
    addFx(second, 0, "chorus");     // where the drive was
    addFx(second, 2, "reverb");     // where the compressor was
    addFx(second, 2, "compressor"); // where the utility was
    addFx(second, 2, "utility");
    EngineRig rig(first);
    useSine(rig);
    rig.run(512, {MidiEvent::noteOn(0, 57, 1.0f)});
    rig.runBlocks(24000, 512);
    rig.run(512, {MidiEvent::noteOff(0, 57)});
    rig.runBlocks(48000, 512);
    for (int slot = 0; slot < 2; ++slot)
        REQUIRE(rig.engine.fxSlotStatus(2, slot).idle);

    rig.engine.loadPatch(second);
    rig.run(512);
    rig.run(512);
    CHECK(rig.engine.fxSlotStatus(2, 2).present); // the new layout is in place without waiting for a note

    rig.runBlocks(48000, 512); // the new values have long glided in
    const std::size_t from = rig.left.size();
    rig.run(512, {MidiEvent::noteOn(0, 57, 1.0f)});
    rig.runBlocks(24000, 512);
    rig.run(512, {MidiEvent::noteOff(0, 57)});
    rig.runBlocks(4800, 512);
    bool finite = true;
    for (std::size_t i = from; i < rig.left.size(); ++i)
        finite = finite && std::isfinite(rig.left[i]);
    CHECK(finite);
    CHECK(zedtest::peakOf(rig.left, from, rig.left.size()) > 0.1f);
    CHECK(rig.engine.outputRecoveries() == 0);
}

TEST_CASE("rack: switching lane B between parallel and series while its effects sleep holds nothing up", "[rack][idle]") {
    // Lane B's effects fade out for the switch and back in after it. Asleep (nothing is sent to
    // lane B), they used to keep their presence: the switch never finished, and every later rack
    // change waited for it.
    Patch p = sinePatch();
    addFx(p, 1, "chorus");
    EngineRig rig(p);
    useSine(rig);
    rig.runBlocks(48000, 512); // nothing reaches lane B, and the chorus goes to sleep
    REQUIRE(rig.engine.fxSlotStatus(1, 0).idle);
    rig.set("fx.b_routing", 1);
    rig.run(512);
    rig.run(512);
    Patch next = p;
    next.set("fx.b_routing", 1);
    addFx(next, 2, "eq");
    rig.engine.setRack(next);
    rig.run(512);
    rig.run(512);
    CHECK(rig.engine.fxSlotStatus(2, 0).present);
}

TEST_CASE("rack: FX off plays the dry signal", "[rack]") {
    Patch dry = sinePatch();
    Patch wet = dry;
    addFx(wet, 0, "drive");
    setFx(wet, 0, 0, "amount", 0.8);
    addFx(wet, 2, "reverb");
    wet.set("global.fx_off", 1);
    EngineRig a(dry), b(wet);
    useSine(a);
    useSine(b);
    for (EngineRig* rig : {&a, &b}) {
        rig->run(512, {MidiEvent::noteOn(0, 57, 1.0f)});
        rig->runBlocks(24000, 512);
    }
    CHECK(zedtest::maxDifference(a.left, b.left) < 1.0e-6);
    CHECK(b.engine.fxSlotsRunning() == 0);
}

TEST_CASE("rack: a locked rack stays when another patch loads", "[rack][lock]") {
    Patch first = sinePatch();
    addFx(first, 0, "reverb");
    setFx(first, 0, 0, "decay", 9000.0);
    Patch second = sinePatch();
    addFx(second, 0, "chorus");
    addFx(second, 1, "delay");
    EngineRig rig(first);
    useSine(rig);
    rig.engine.setRackLocked(true);
    rig.engine.loadPatch(second);
    rig.run(512);
    CHECK(rig.engine.fxSlotStatus(0, 0).present);
    CHECK_FALSE(rig.engine.fxSlotStatus(1, 0).present);
    CHECK(rig.engine.params().get(reg().fxSlotParam(0, 0, paramOf(effect("reverb"), "decay"))) == Approx(9000.0));
    rig.engine.setRackLocked(false);
    rig.engine.loadPatch(second);
    rig.run(512); // the old reverb fades out first
    rig.run(512);
    CHECK(rig.engine.fxSlotStatus(1, 0).present);

    // The same at the patch level: the new patch takes the old rack, its routes onto effects go.
    Patch incoming = second;
    zedtest::addRoute(incoming, "lfo1", "fx.a1.p2", 0.5);
    zedtest::addRoute(incoming, "lfo1", "filter1.cutoff", 0.5);
    incoming.copyRackFrom(first);
    CHECK(incoming.fxSlotType(0, 0) == effect("reverb"));
    CHECK(incoming.fxSlotCount(1) == 0);
    REQUIRE(incoming.routes.size() == 1);
    CHECK(incoming.routes[0].dest == "filter1.cutoff");
}

TEST_CASE("rack: rack and effect presets", "[rack][preset]") {
    Patch p;
    addFx(p, 0, "drive");
    setFx(p, 0, 0, "amount", 0.66);
    p.fxCurves[0][0][1] = zedtest::lineShape(0.0, 0.5);
    addFx(p, 2, "limiter");
    p.set("fx.b_routing", 1);
    const std::string rack = saveRackPreset(p);
    Patch target;
    addFx(target, 1, "phaser");
    std::vector<Issue> issues;
    REQUIRE(loadRackPreset(rack, target, issues));
    CHECK(target.fxSlotType(0, 0) == effect("drive"));
    CHECK(target.fxSlotCount(1) == 0);
    CHECK(target.fxSlotType(2, 0) == effect("limiter"));
    CHECK(target.get("fx.b_routing") == 1.0);
    CHECK(target.fxCurves[0][0][1] == p.fxCurves[0][0][1]);
    CHECK(saveRackPreset(target) == rack);

    const std::string slot = saveSlotPreset(p, 0, 0);
    Patch other;
    addFx(other, 0, "eq");
    addFx(other, 0, "chorus");
    FxMoves moves;
    REQUIRE(loadSlotPreset(slot, other, 0, 1, &moves, issues)); // replaces the chorus
    CHECK(other.fxSlotType(0, 1) == effect("drive"));
    CHECK(other.get(reg().fxSlotParam(0, 1, paramOf(effect("drive"), "amount"))) == Approx(0.66));
    CHECK(moves.from[0] == 0);
    CHECK(moves.from[1] == -1);
    REQUIRE(loadSlotPreset(slot, other, 0, 2, nullptr, issues)); // appends
    CHECK(other.fxSlotCount(0) == 3);

    issues.clear();
    CHECK_FALSE(loadRackPreset("{\"format\":\"zederia.patch\",\"version\":1}", target, issues));
    CHECK_FALSE(issues.empty());
    issues.clear();
    CHECK_FALSE(loadSlotPreset("{\"format\":\"zederia.fx\",\"version\":1,\"slot\":{\"type\":\"nope\"}}", other, 0, 0, nullptr, issues));
    REQUIRE_FALSE(issues.empty());
    CHECK(issues[0].path == "/slot/type");
}

TEST_CASE("rack: effect parameters are modulation destinations", "[rack][modulation]") {
    Patch p = sinePatch();
    addFx(p, 0, "utility"); // p1 is its gain, -36..36 dB
    zedtest::addRoute(p, "macro1", "fx.a1.p1", 0.25, Polarity::Uni);
    const auto render = [](Patch q, double macro) {
        q.set("macro1.value", macro);
        EngineRig rig(q);
        useSine(rig);
        rig.run(512, {MidiEvent::noteOn(0, 57, 1.0f)});
        rig.runBlocks(24000, 512);
        return level(rig.left, 12000, 24000);
    };
    CHECK(render(p, 1.0) - render(p, 0.0) == Approx(18.0).margin(0.2)); // a quarter of 72 dB

    // The live value of a modulated effect parameter is reported in the effect's own unit.
    p.set("macro1.value", 1.0);
    EngineRig rig(p);
    useSine(rig);
    rig.run(512);
    rig.run(512);
    CHECK(rig.engine.liveValue(reg().fxSlotParam(0, 0, 0)) == Approx(18.0).margin(0.01));
}

TEST_CASE("rack: display responses and meters", "[rack]") {
    Patch p;
    addFx(p, 0, "eq");
    setFx(p, 0, 0, "mid2_gain", -9.0);
    setFx(p, 0, 0, "mid2_freq", 3000.0);
    addFx(p, 0, "widener");
    Engine engine;
    engine.prepare(48000.0);
    engine.loadPatch(p);
    const double hz[] = {3000.0, 100.0};
    float db[2] = {};
    REQUIRE(engine.fxResponse(0, 0, hz, db));
    CHECK(db[0] == Approx(-9.0).margin(0.01));
    CHECK(db[1] == Approx(0.0).margin(0.1));
    CHECK_FALSE(engine.fxResponse(0, 1, hz, db)); // a widener has no response curve
}

TEST_CASE("rack: edits from another thread while audio runs are real-time safe", "[rack][rt]") {
    rt::resetViolations();
    Patch p = sinePatch();
    EngineRig rig(p);
    useSine(rig);
    std::atomic<bool> stop{false};
    std::thread editor([&] {
        Rng rng(11);
        const auto count = static_cast<std::uint64_t>(reg().effectTypes().size());
        while (!stop.load()) {
            Patch q = p;
            for (int lane = 0; lane < kNumFxLanes; ++lane)
                for (int n = static_cast<int>(rng.next() % 4); n > 0; --n)
                    q.addFxSlot(lane, static_cast<int>(rng.next() % count));
            FxMoves moves;
            for (int k = 0; k < kNumFxLanes * kNumFxSlots; ++k)
                moves.from[static_cast<std::size_t>(k)] = static_cast<int>(rng.next() % 30) - 6;
            rig.engine.setRack(q, &moves);
            rig.engine.collectGarbage();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    rig.run(512, {MidiEvent::noteOn(0, 57, 1.0f), MidiEvent::noteOn(5, 64, 1.0f)});
    for (int block = 0; block < 600; ++block)
        rig.run(256);
    stop = true;
    editor.join();
    INFO("last violation: " << rt::lastViolation());
    CHECK(rt::violationCount() == 0);
    bool finite = true;
    for (const float s : rig.left)
        finite = finite && std::isfinite(s);
    CHECK(finite);
}
