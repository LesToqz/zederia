#include "zederia/engine/VoiceAllocator.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using namespace zed;
using Kind = VoiceAction::Kind;
using State = VoiceAllocator::State;

namespace {

struct Harness {
    VoiceAllocator alloc;
    VoiceAllocator::Actions actions;

    explicit Harness(VoiceMode mode = VoiceMode::Poly, int polyphony = 4, StealMode steal = StealMode::Oldest,
                     NotePriority priority = NotePriority::Last) {
        alloc.configure(mode, polyphony, steal, priority, actions);
        actions.count = 0;
    }
    std::vector<VoiceAction> on(int note, float velocity = 0.8f) {
        actions.count = 0;
        alloc.noteOn(note, velocity, actions);
        return taken();
    }
    std::vector<VoiceAction> off(int note) {
        actions.count = 0;
        alloc.noteOff(note, actions);
        return taken();
    }
    std::vector<VoiceAction> pedal(bool down) {
        actions.count = 0;
        alloc.sustainPedal(down, actions);
        return taken();
    }
    std::vector<VoiceAction> taken() {
        return {actions.items.begin(), actions.items.begin() + actions.count};
    }
    // The engine reports dying and released voices as finished once they are silent.
    void finishAll(State which) {
        for (int i = 0; i < kVoiceSlots; ++i)
            if (alloc.state(i) == which)
                alloc.voiceFinished(i);
    }
    int count(State which) const {
        int n = 0;
        for (int i = 0; i < kVoiceSlots; ++i)
            n += alloc.state(i) == which ? 1 : 0;
        return n;
    }
};

} // namespace

TEST_CASE("allocator: poly starts and releases voices", "[allocator]") {
    Harness h;
    const auto a = h.on(60);
    REQUIRE(a.size() == 1);
    CHECK(a[0].kind == Kind::Start);
    CHECK(a[0].note == 60);
    CHECK(a[0].velocity == 0.8f);
    const auto b = h.on(64);
    REQUIRE(b.size() == 1);
    CHECK(b[0].slot != a[0].slot);
    CHECK(h.alloc.sounding() == 2);

    const auto r = h.off(60);
    REQUIRE(r.size() == 1);
    CHECK(r[0].kind == Kind::Release);
    CHECK(r[0].slot == a[0].slot);
    CHECK(h.alloc.state(a[0].slot) == State::Releasing);
    CHECK(h.alloc.sounding() == 2); // a releasing voice still counts
    h.alloc.voiceFinished(a[0].slot);
    CHECK(h.alloc.sounding() == 1);
    CHECK(h.off(61).empty()); // a key that is not down
}

TEST_CASE("allocator: the same key twice releases the earlier voice", "[allocator]") {
    Harness h;
    const int first = h.on(60)[0].slot;
    const auto again = h.on(60);
    REQUIRE(again.size() == 2);
    CHECK(again[0].kind == Kind::Release);
    CHECK(again[0].slot == first);
    CHECK(again[1].kind == Kind::Start);
    CHECK(again[1].slot != first);
    const auto off = h.off(60);
    REQUIRE(off.size() == 1); // only the new voice was still held
    CHECK(off[0].slot == again[1].slot);
}

TEST_CASE("allocator: stealing follows the chosen rule", "[allocator]") {
    SECTION("oldest") {
        Harness h(VoiceMode::Poly, 3, StealMode::Oldest);
        const int first = h.on(60)[0].slot;
        h.on(62);
        h.on(64);
        const auto a = h.on(65);
        REQUIRE(a.size() == 2);
        CHECK(a[0].kind == Kind::Kill);
        CHECK(a[0].slot == first);
        CHECK(a[1].kind == Kind::Start);
        CHECK(a[1].slot != first); // the stolen voice fades out on its own slot
        CHECK(h.alloc.state(first) == State::Dying);
        CHECK(h.alloc.sounding() == 3);
    }
    SECTION("quietest") {
        Harness h(VoiceMode::Poly, 3, StealMode::Quietest);
        const int a = h.on(60)[0].slot, b = h.on(62)[0].slot, c = h.on(64)[0].slot;
        h.alloc.setLevel(a, 0.9f);
        h.alloc.setLevel(b, 0.1f);
        h.alloc.setLevel(c, 0.5f);
        const auto steal = h.on(65);
        REQUIRE(steal.size() == 2);
        CHECK(steal[0].kind == Kind::Kill);
        CHECK(steal[0].slot == b);
    }
    SECTION("release first, falling back to the oldest") {
        Harness h(VoiceMode::Poly, 3, StealMode::Release);
        const int a = h.on(60)[0].slot, b = h.on(62)[0].slot;
        h.on(64);
        h.off(62); // b is now releasing
        auto steal = h.on(65);
        REQUIRE(steal.size() == 2);
        CHECK(steal[0].slot == b);
        steal = h.on(67); // nothing is releasing now
        REQUIRE(steal.size() == 2);
        CHECK(steal[0].slot == a);
    }
}

TEST_CASE("allocator: polyphony is never exceeded and slots never run out", "[allocator]") {
    Harness h(VoiceMode::Poly, kMaxPolyphony);
    // Far more notes than slots, with stolen voices never reported as finished: the allocator
    // must take over fading voices instead of dropping notes.
    for (int i = 0; i < 400; ++i) {
        const auto a = h.on(i % 128);
        REQUIRE(!a.empty());
        const auto& last = a.back();
        CHECK((last.kind == Kind::Start || last.kind == Kind::Retrigger));
        CHECK(h.alloc.sounding() <= kMaxPolyphony);
        CHECK(h.alloc.state(last.slot) == State::Held);
    }
    CHECK(h.count(State::Free) == 0);
    h.finishAll(State::Dying);
    CHECK(h.alloc.sounding() == kMaxPolyphony);
}

TEST_CASE("allocator: lowering the polyphony steals the excess", "[allocator]") {
    Harness h(VoiceMode::Poly, 8);
    for (int n = 60; n < 68; ++n)
        h.on(n);
    h.actions.count = 0;
    h.alloc.configure(VoiceMode::Poly, 3, StealMode::Oldest, NotePriority::Last, h.actions);
    const auto a = h.taken();
    REQUIRE(a.size() == 5);
    for (const auto& action : a)
        CHECK(action.kind == Kind::Kill);
    CHECK(h.alloc.sounding() == 3);
}

TEST_CASE("allocator: sustain pedal holds released keys", "[allocator]") {
    Harness h;
    const int slot = h.on(60)[0].slot;
    CHECK(h.pedal(true).empty());
    CHECK(h.off(60).empty());
    CHECK(h.alloc.state(slot) == State::Sustained);
    const auto up = h.pedal(false);
    REQUIRE(up.size() == 1);
    CHECK(up[0].kind == Kind::Release);
    CHECK(up[0].slot == slot);

    h.finishAll(State::Releasing);
    h.pedal(true);
    const int a = h.on(62)[0].slot;
    h.off(62);
    h.actions.count = 0;
    h.alloc.allNotesOff(h.actions); // overrides the pedal
    REQUIRE(h.taken().size() == 1);
    CHECK(h.alloc.state(a) == State::Releasing);
}

TEST_CASE("allocator: mono retriggers and falls back to held keys", "[allocator]") {
    Harness h(VoiceMode::Mono);
    const auto a = h.on(60);
    REQUIRE(a.size() == 1);
    CHECK(a[0].kind == Kind::Start);
    const int slot = a[0].slot;

    const auto b = h.on(64, 0.5f);
    REQUIRE(b.size() == 1);
    CHECK(b[0].kind == Kind::Retrigger);
    CHECK(b[0].slot == slot);
    CHECK(b[0].note == 64);
    CHECK(h.alloc.sounding() == 1);

    const auto c = h.off(64); // 60 is still down
    REQUIRE(c.size() == 1);
    CHECK(c[0].kind == Kind::Retrigger);
    CHECK(c[0].note == 60);
    CHECK(c[0].velocity == 0.8f); // the velocity that key was struck with

    const auto d = h.off(60);
    REQUIRE(d.size() == 1);
    CHECK(d[0].kind == Kind::Release);

    const auto e = h.on(67); // while the voice is still releasing: same slot, no new voice
    REQUIRE(e.size() == 1);
    CHECK(e[0].kind == Kind::Retrigger);
    CHECK(e[0].slot == slot);
    CHECK(h.off(60).empty()); // releasing a key that is not sounding changes nothing
}

TEST_CASE("allocator: legato changes pitch without retriggering", "[allocator]") {
    Harness h(VoiceMode::Legato);
    const int slot = h.on(60)[0].slot;
    const auto b = h.on(64);
    REQUIRE(b.size() == 1);
    CHECK(b[0].kind == Kind::Glide);
    CHECK(b[0].slot == slot);
    const auto c = h.off(64);
    REQUIRE(c.size() == 1);
    CHECK(c[0].kind == Kind::Glide);
    CHECK(c[0].note == 60);
    REQUIRE(h.off(60).size() == 1);
    // Keys did not overlap this time: a detached note attacks again.
    const auto d = h.on(62);
    REQUIRE(d.size() == 1);
    CHECK(d[0].kind == Kind::Retrigger);
}

TEST_CASE("allocator: note priority picks the sounding key", "[allocator]") {
    SECTION("low") {
        Harness h(VoiceMode::Mono, 4, StealMode::Oldest, NotePriority::Low);
        h.on(60);
        CHECK(h.on(64).empty()); // higher key: the low note keeps sounding
        const auto lower = h.on(55);
        REQUIRE(lower.size() == 1);
        CHECK(lower[0].note == 55);
        const auto back = h.off(55);
        REQUIRE(back.size() == 1);
        CHECK(back[0].note == 60);
    }
    SECTION("high") {
        Harness h(VoiceMode::Mono, 4, StealMode::Oldest, NotePriority::High);
        h.on(60);
        CHECK(h.on(55).empty());
        const auto higher = h.on(72);
        REQUIRE(higher.size() == 1);
        CHECK(higher[0].note == 72);
        const auto back = h.off(72);
        REQUIRE(back.size() == 1);
        CHECK(back[0].note == 60);
    }
    SECTION("last") {
        Harness h(VoiceMode::Mono);
        h.on(60);
        h.on(72);
        h.on(55);
        const auto back = h.off(55);
        REQUIRE(back.size() == 1);
        CHECK(back[0].note == 72); // the most recent key still down
    }
}

TEST_CASE("allocator: changing mode releases what is sounding; all-sound-off fades everything", "[allocator]") {
    Harness h(VoiceMode::Poly, 8);
    h.on(60);
    h.on(64);
    h.actions.count = 0;
    h.alloc.configure(VoiceMode::Mono, 8, StealMode::Oldest, NotePriority::Last, h.actions);
    CHECK(h.taken().size() == 2);
    CHECK(h.count(State::Releasing) == 2);

    h.actions.count = 0;
    h.alloc.allSoundOff(h.actions);
    CHECK(h.taken().size() == 2);
    CHECK(h.count(State::Dying) == 2);
    CHECK(h.alloc.sounding() == 0);
    h.finishAll(State::Dying);
    CHECK(h.count(State::Free) == kVoiceSlots);
}
