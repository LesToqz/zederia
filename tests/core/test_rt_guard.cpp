// The audio-thread guard (plan 5.3, 6.1): proves that the guard catches allocations, frees and
// locks inside the audio callback, and that the engine's callback is clean under load.
#include "../support/TestUtil.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/util/Math.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <mutex>
#include <thread>

using namespace zed;

namespace {
// Keeps the optimizer from removing the test allocations.
volatile std::size_t opaqueSize = 64;
void* volatile opaquePointer = nullptr;
} // namespace

TEST_CASE("guard: catches an allocation and a free inside the audio callback", "[rt][guard-selftest]") {
    rt::resetViolations();
    int* block = new int[opaqueSize]; // outside the callback: allowed
    opaquePointer = block;
    CHECK(rt::violationCount() == 0);
    CHECK_FALSE(rt::onAudioThread());
    // The test macros allocate, so inside the scope the results are only recorded, and checked after.
    bool inScope = false;
    int afterNew = 0, afterDelete = 0, afterVector = 0;
    const char* newKind = "";
    const char* deleteKind = "";
    {
        const rt::AudioThreadScope scope;
        inScope = rt::onAudioThread();
        int* inside = new int[opaqueSize];
        opaquePointer = inside;
        afterNew = rt::violationCount();
        newKind = rt::lastViolation();
        delete[] inside;
        afterDelete = rt::violationCount();
        deleteKind = rt::lastViolation();
        std::vector<float> grows(opaqueSize); // containers count too
        opaquePointer = grows.data();
        afterVector = rt::violationCount();
    }
    const int afterScope = rt::violationCount();
    CHECK(inScope);
    CHECK(afterNew == 1);
    CHECK(std::string(newKind) == "allocation");
    CHECK(afterDelete == 2);
    CHECK(std::string(deleteKind) == "free");
    CHECK(afterVector > afterDelete); // one or more, depending on the standard library
    CHECK(afterScope > afterVector); // the vector was freed inside the scope
    CHECK_FALSE(rt::onAudioThread());
    delete[] block;
    CHECK(rt::violationCount() == afterScope); // outside again: not counted
    rt::resetViolations();
    CHECK(rt::violationCount() == 0);
    CHECK(std::string(rt::lastViolation()).empty());
}

TEST_CASE("guard: catches a lock inside the audio callback", "[rt][guard-selftest]") {
    rt::resetViolations();
    rt::Mutex mutex;
    {
        const std::lock_guard<rt::Mutex> outside(mutex); // allowed
    }
    CHECK(rt::violationCount() == 0);
    int afterLock = 0, afterTry = 0;
    const char* kind = "";
    {
        const rt::AudioThreadScope scope;
        const std::lock_guard<rt::Mutex> inside(mutex);
        afterLock = rt::violationCount();
        kind = rt::lastViolation();
    }
    {
        const rt::AudioThreadScope scope;
        if (mutex.try_lock())
            mutex.unlock();
        afterTry = rt::violationCount();
    }
    CHECK(afterLock == 1);
    CHECK(std::string(kind) == "lock");
    CHECK(afterTry == 2);
    rt::resetViolations();
}

TEST_CASE("guard: is per thread", "[rt][guard-selftest]") {
    rt::resetViolations();
    const rt::AudioThreadScope scope; // this thread is "in the callback" ...
    std::thread other([] {
        // ... but another thread is not, and may allocate freely.
        const auto p = std::make_unique<int[]>(opaqueSize);
        opaquePointer = p.get();
    });
    other.join(); // join itself frees thread bookkeeping on this thread, which is why this is a self-test
    rt::resetViolations();
}

TEST_CASE("guard: the engine's audio callback never allocates, frees or locks", "[rt]") {
    rt::resetViolations();
    const auto& reg = Registry::get();
    Engine engine;
    engine.prepare(48000.0);
    Patch patch;
    patch.set("global.polyphony", 64);
    engine.loadPatch(patch);

    Rng rng(5);
    // Routes from every kind of source to every kind of destination, replaced now and then, as
    // an editor or an agent would while the instrument plays.
    std::vector<std::string> sources, destinations;
    for (const ModSourceDef& s : reg.modSources())
        sources.push_back(s.id);
    sources.emplace_back("cc21");
    for (const ParamDef& d : reg.params())
        if (d.modulatable)
            destinations.push_back(d.id);
    const auto randomRoutes = [&] {
        std::vector<ModRoute> routes(1 + rng.next() % kNumModRoutes);
        for (ModRoute& r : routes) {
            r.source = sources[rng.next() % sources.size()];
            r.dest = destinations[rng.next() % destinations.size()];
            if (rng.next() % 3 == 0)
                r.via = sources[rng.next() % sources.size()];
            r.polarity = rng.next() % 2 == 0 ? Polarity::Bi : Polarity::Uni;
            r.curve = static_cast<ModCurve>(rng.next() % 5);
            if (r.curve == ModCurve::Drawn)
                r.curveShape.points = {{0.0, 0.0, 0.3}, {0.4, 1.0, -0.5}, {1.0, 0.2, 0.0}};
            r.solo = rng.next() % 40 == 0;
            r.mute = rng.next() % 20 == 0;
        }
        return routes;
    };
    const auto randomShape = [&] {
        Shape shape;
        const int points = static_cast<int>(rng.next() % 12);
        for (int i = 0; i < points; ++i)
            shape.points.push_back({static_cast<double>(i) / std::max(points - 1, 1), rng.uniform() * 2.0 - 1.0, rng.uniform() * 2.0 - 1.0});
        shape.loopStart = rng.uniform();
        return shape;
    };

    std::vector<float> left(1024), right(1024), side(1024, 0.25f);
    std::vector<MidiEvent> events;
    events.reserve(64);
    Transport transport;
    transport.playing = true;
    for (int block = 0; block < 2000; ++block) {
        events.clear();
        const int count = static_cast<int>(rng.next() % 16);
        for (int i = 0; i < count; ++i) {
            const int offset = i * 8;
            const int note = static_cast<int>(rng.next() % 128);
            const int channel = static_cast<int>(rng.next() % 16);
            const auto value = static_cast<float>(rng.uniform());
            switch (rng.next() % 8) {
            case 0:
            case 1:
            case 2: events.push_back(MidiEvent::noteOn(offset, note, value, channel)); break;
            case 3: events.push_back(MidiEvent::noteOff(offset, note, value, channel)); break;
            case 4: events.push_back(MidiEvent::pitchBend(offset, value - 0.5f, channel)); break;
            case 5: events.push_back({offset, MidiEvent::Type::ChannelPressure, static_cast<std::uint8_t>(channel), 0, value}); break;
            case 6: events.push_back({offset, MidiEvent::Type::PolyPressure, static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(note), value}); break;
            default: events.push_back(MidiEvent::controller(offset, static_cast<int>(rng.next() % 128), value, channel)); break;
            }
        }
        if (block % 10 == 0) // every parameter moves, as a host automating everything would
            for (const auto& d : reg.params())
                engine.params().set(d.index, d.fromNormalized(rng.uniform()));
        if (block % 7 == 0) {
            engine.setRoutes(randomRoutes());
            engine.setLfoShape(static_cast<int>(rng.next() % kNumLfos), randomShape());
            EnvShape env;
            const int points = static_cast<int>(rng.next() % 9);
            for (int i = 0; i < points; ++i)
                env.points.push_back({rng.uniform() * 40.0, i % 3 == 0 ? "1/32" : "", rng.uniform(), rng.uniform() * 2.0 - 1.0});
            env.sustainPoint = static_cast<int>(rng.next() % 10) - 1;
            env.loopStart = static_cast<int>(rng.next() % 4) - 1;
            env.loopEnd = static_cast<int>(rng.next() % 8) - 1;
            engine.setEnvShape(1 + static_cast<int>(rng.next() % 3), env);
            engine.setEnvSync(static_cast<int>(rng.next() % kNumEnvs), static_cast<int>(rng.next() % 5), rng.next() % 2 == 0 ? 0.0 : 0.25);
            auto tuning = std::make_shared<Tuning>();
            for (double& key : tuning->note)
                key += rng.uniform() - 0.5;
            engine.setTuning(rng.next() % 3 == 0 ? nullptr : tuning);
        }
        const int frames = 1 + static_cast<int>(rng.next() % 1024);
        for (auto& e : events)
            e.offset = std::min(e.offset, frames - 1);
        transport.bpm = 60.0 + 120.0 * rng.uniform();
        ProcessArgs args{left.data(), right.data(), frames, events, transport};
        if (block % 2 == 0) {
            args.sidechainLeft = side.data();
            args.sidechainRight = side.data();
        }
        engine.process(args);
        transport.ppqPosition += frames * transport.bpm / (60.0 * 48000.0);
        bool finite = true;
        for (int i = 0; i < frames; ++i)
            finite = finite && std::isfinite(left[static_cast<std::size_t>(i)]) && std::isfinite(right[static_cast<std::size_t>(i)]);
        REQUIRE(finite); // whatever the settings, the output is always a number
    }
    INFO("last violation: " << rt::lastViolation());
    REQUIRE(rt::violationCount() == 0);
}

TEST_CASE("guard: offline renders are clean as well", "[rt]") {
    rt::resetViolations();
    const NoteSpec notes[] = {{40, 0.0, 0.3, 1.0}, {52, 0.05, 0.3, 0.6}, {64, 0.1, 0.3, 0.8}};
    const RenderResult r = renderOffline(Patch(), sequenceFromNotes(notes, 48000.0), {});
    CHECK(r.peak() > 0.0f);
    CHECK(rt::violationCount() == 0);
}
