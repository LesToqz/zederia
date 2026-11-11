// Decides which voice slot plays which note (plan 3.6): poly, mono and legato modes, note
// priority, the sustain pedal and voice stealing. It holds no audio state, so it can be tested
// on its own; the engine carries out the actions it returns. Nothing here allocates.
#pragma once

#include "zederia/params/Registry.h"

#include <array>
#include <cstdint>

namespace zed {

enum class VoiceMode : std::uint8_t { Poly, Mono, Legato };
enum class StealMode : std::uint8_t { Oldest, Quietest, Release };
enum class NotePriority : std::uint8_t { Last, Low, High };

// Spare slots let a stolen voice fade out while its replacement starts.
inline constexpr int kVoiceSlots = kMaxPolyphony + 16;

struct VoiceAction {
    enum class Kind : std::uint8_t {
        Start,     // begin a new note on a silent slot
        Retrigger, // restart the envelopes from their current level with a new note
        Glide,     // change pitch only (legato)
        Release,   // key up: enter the release stage
        Kill,      // stolen: fade out quickly
    };
    Kind kind = Kind::Start;
    int slot = 0;
    int note = 0;
    float velocity = 0.0f;
};

class VoiceAllocator {
public:
    struct Actions {
        std::array<VoiceAction, kVoiceSlots + 8> items;
        int count = 0;
        void push(VoiceAction::Kind kind, int slot, int note = 0, float velocity = 0.0f) {
            if (count < static_cast<int>(items.size()))
                items[static_cast<std::size_t>(count++)] = {kind, slot, note, velocity};
        }
    };

    enum class State : std::uint8_t { Free, Held, Sustained, Releasing, Dying };

    VoiceAllocator() { reset(); }

    // Changing the mode releases everything; lowering the polyphony steals the excess.
    void configure(VoiceMode mode, int polyphony, StealMode steal, NotePriority priority, Actions& out);
    void noteOn(int note, float velocity, Actions& out);
    void noteOff(int note, Actions& out);
    void sustainPedal(bool down, Actions& out);
    void allNotesOff(Actions& out); // releases every note, pedal or not
    void allSoundOff(Actions& out); // fades every voice out at once
    void voiceFinished(int slot);   // the engine reports that a slot has gone silent
    void setLevel(int slot, float level) { slots_[static_cast<std::size_t>(slot)].level = level; }
    void reset();                   // forgets everything without emitting actions

    State state(int slot) const { return slots_[static_cast<std::size_t>(slot)].state; }
    int note(int slot) const { return slots_[static_cast<std::size_t>(slot)].note; }
    int sounding() const; // voices that count against the polyphony (not free, not dying)

private:
    struct Slot {
        State state = State::Free;
        int note = 0;
        std::uint64_t age = 0;
        float level = 0.0f;
    };
    struct HeldKey {
        int note = 0;
        float velocity = 0.0f;
    };

    int findFree() const;
    int pickVictim() const;
    int oldestDying() const;
    void start(int note, float velocity, Actions& out);
    void monoNoteOn(int note, float velocity, Actions& out);
    void monoNoteOff(int note, Actions& out);
    const HeldKey* selectHeld() const;
    void removeHeld(int note);

    std::array<Slot, kVoiceSlots> slots_{};
    std::array<HeldKey, 128> held_{}; // keys physically down, in the order pressed (mono modes)
    int heldCount_ = 0;
    int monoSlot_ = -1;
    std::uint64_t counter_ = 0;
    bool pedal_ = false;
    VoiceMode mode_ = VoiceMode::Poly;
    int polyphony_ = 16;
    StealMode steal_ = StealMode::Oldest;
    NotePriority priority_ = NotePriority::Last;
};

} // namespace zed
