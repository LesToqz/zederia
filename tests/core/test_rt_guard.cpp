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
    std::vector<float> left(1024), right(1024);
    std::vector<MidiEvent> events;
    events.reserve(64);
    for (int block = 0; block < 2000; ++block) {
        events.clear();
        const int count = static_cast<int>(rng.next() % 16);
        for (int i = 0; i < count; ++i) {
            const int offset = i * 8;
            const int note = static_cast<int>(rng.next() % 128);
            switch (rng.next() % 5) {
            case 0:
            case 1: events.push_back(MidiEvent::noteOn(offset, note, static_cast<float>(rng.uniform()))); break;
            case 2: events.push_back(MidiEvent::noteOff(offset, note)); break;
            case 3: events.push_back(MidiEvent::pitchBend(offset, static_cast<float>(rng.uniform() - 0.5))); break;
            default: events.push_back(MidiEvent::controller(offset, static_cast<int>(rng.next() % 128), static_cast<float>(rng.uniform()))); break;
            }
        }
        if (block % 10 == 0) // every parameter moves, as a host automating everything would
            for (const auto& d : reg.params())
                engine.params().set(d.index, d.fromNormalized(rng.uniform()));
        const int frames = 1 + static_cast<int>(rng.next() % 1024);
        for (auto& e : events)
            e.offset = std::min(e.offset, frames - 1);
        engine.process({left.data(), right.data(), frames, events, {}});
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
