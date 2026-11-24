// The play features (plan 3.9): chord memory, scale lock and strum, the arpeggiator with its step
// lane and pattern slots, and the clip player. The player sits between the incoming MIDI and the
// voices: it turns the block's events into the events the voices play, and reports the notes it
// made so they can be sent to the host.
//
//  incoming ── clip player (a key starts and transposes a clip) ─────────────────────┐
//          └─ scale lock ─ chord memory ─ strum ─┬─ arpeggiator ─────────────────────┼─► voices, MIDI out
//                                                 └─ (arpeggiator off) ──────────────┘
//
// Threading: PlayData is built off the audio thread and handed over whole; Player::process runs on
// the audio thread and never allocates. Everything random comes from a seeded generator, so a
// render with the same seed is the same every time.
#pragma once

#include "zederia/engine/MidiEvent.h"
#include "zederia/patch/Patch.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace zed {

struct Transport;

namespace play {

enum class ArpMode : std::uint8_t { Up, Down, UpDown, UpDownRepeat, Random, Walk, AsPlayed, Chord };

// The play parameters as the player reads them at the start of a block.
struct Settings {
    bool arpOn = false;
    ArpMode arpMode = ArpMode::Up;
    int octaves = 1;
    double stepBeats = 0.25; // one step, in quarter notes
    double gate = 0.5;       // note length as a share of the step (up to 2)
    double swing = 0.0;      // 0..1: every second step is late by up to half a step
    bool latch = false, restart = true;
    int steps = 16;   // length of the step lane
    int pattern = 0;  // 0..11
    bool chordMemory = false, scaleLock = false;
    int key = 0;      // root of the scale, 0 = C
    int scale = 0;    // index into the registry's scale list
    bool filter = false; // scale lock drops notes outside the scale instead of moving them
    double strumMs = 0.0;
    int strumDirection = 0; // up, down, alternate
    bool clipOn = false;
    int clipSlot = 0;
    bool clipFromKey = true; // a key starts the clip; otherwise it follows the host transport
    bool clipLatch = false, clipLoop = true;
};

// The structure the player needs from a patch, laid out for the audio thread.
struct PlayData {
    ArpStep steps[kNumArpPatterns][kMaxArpSteps];
    int chord[kMaxChordNotes] = {0, 4, 7};
    int chordCount = 3;
    struct ClipData {
        std::vector<ClipNote> notes; // sorted by start
        double lengthBeats = 4.0;
        int root = 60;
    } clips[kNumClips];
    static std::shared_ptr<const PlayData> fromPatch(const Patch& patch);
};

// Semitones of each scale in the registry's order (chord.scale); a set bit marks a scale degree.
std::uint16_t scaleMask(int scale) noexcept;
// The nearest key inside the scale (the lower one on a tie).
int snapToScale(int key, int root, std::uint16_t mask) noexcept;

class Player {
public:
    static constexpr int kMaxEvents = 2048;  // events one block can produce
    static constexpr int kMaxPending = 1024; // events waiting for a later block

    void prepare(double sampleRate);
    void reset(std::uint64_t seed);

    // Turns the block's incoming events (sorted by offset) into the events for the voices, written
    // to `out` (sorted, at most kMaxEvents); returns how many. Note events the play features made
    // go to `generated` as well (at most `generatedCapacity`), counted in `generatedCount`.
    int process(const Settings& settings, const PlayData& data, const Transport& transport, int frames,
                std::span<const MidiEvent> in, MidiEvent* out, MidiEvent* generated, int generatedCapacity,
                int& generatedCount) noexcept;

    // True while the player changes what is played (any play feature is on).
    bool shaping() const noexcept { return shaping_; }
    // The step of the arpeggiator's lane that sounded last while it runs (a key held, the
    // arpeggiator on), -1 otherwise. For the step lane's marker; it changes nothing that plays.
    int playingStep() const noexcept { return s_.arpOn && running_ && heldCount_ > 0 ? playedStep_ : -1; }

private:
    struct Timed {
        std::int64_t at = 0; // absolute sample
        MidiEvent event;
        std::uint8_t origin = 0; // 0 chord stage, 1 arpeggiator, 2 clip
        std::uint8_t inputKey = 0;
    };
    struct Held {
        int key = 0;
        float velocity = 0.0f;
        std::uint32_t order = 0;
    };
    struct Sounding { // a note the arpeggiator or a clip has started
        int key = 0;
        std::int64_t offAt = 0;
        std::uint8_t origin = 0;
    };

    void emit(const MidiEvent& e, std::int64_t at, std::uint8_t origin) noexcept;
    void schedule(const MidiEvent& e, std::int64_t at, std::uint8_t origin, std::uint8_t inputKey = 0) noexcept;
    void startNote(int key, float velocity, std::int64_t at, std::int64_t offAt, std::uint8_t origin) noexcept;
    void stopSounding(std::uint8_t origin, std::int64_t at) noexcept;
    void chordNoteOn(const MidiEvent* group, int count, std::int64_t at) noexcept;
    void chordNoteOff(int key, std::int64_t at) noexcept;
    void stageOn(int key, float velocity, std::int64_t at) noexcept;
    void stageOff(int key, std::int64_t at) noexcept;
    void arpAdd(int key, float velocity, std::int64_t at) noexcept;
    void arpRemove(int key) noexcept;
    void buildSequence() noexcept;
    void arpStep(std::int64_t at, std::int64_t stepLength) noexcept;
    void runArp(const Transport& transport, std::int64_t from, std::int64_t until) noexcept;
    void runClip(const Transport& transport, std::int64_t from, std::int64_t until) noexcept;
    void allOff(std::int64_t at) noexcept;
    std::uint32_t random() noexcept;

    double sampleRate_ = 48000.0;
    std::int64_t now_ = 0; // absolute sample at the start of the block
    Settings s_;
    const PlayData* data_ = nullptr;
    bool shaping_ = false;
    std::uint64_t rng_ = 1;

    // Output of the block being made.
    struct OutEvent {
        MidiEvent event;
        std::int32_t seq = 0;
        bool generated = false;
    };
    OutEvent outBuf_[kMaxEvents];
    int outCount_ = 0, seq_ = 0;

    Timed pending_[kMaxPending];
    int pendingCount_ = 0;

    // Chord stage: what each incoming key became, and how many incoming keys hold each output key.
    std::uint8_t mapped_[128][kMaxChordNotes] = {};
    std::uint8_t mappedCount_[128] = {};
    std::uint8_t refs_[128] = {};
    int mappedKeys_ = 0; // incoming keys that have a mapping (held down through the chord stage)
    bool strumUp_ = true;

    // Arpeggiator.
    Held held_[128];
    int heldCount_ = 0;
    int physical_ = 0; // keys the chord stage holds down right now
    // Keys that went straight through while nothing was shaped and are still down: if shaping
    // starts meanwhile, their release still has to reach the engine as it is.
    std::uint8_t through_[128] = {};
    std::uint8_t down_[128] = {}; // the same, per key: a latched key that is no longer down leaves when Latch goes off
    std::uint32_t order_ = 0;
    int seq[512];
    float seqVelocity[512];
    int seqCount_ = 0;
    bool seqDirty_ = true, running_ = false, wasArp_ = false;
    int cursor_ = 0, walkDir_ = 1, laneStep_ = 0, chordStep_ = 0;
    int playedStep_ = -1;
    std::int64_t nextStepAt_ = 0, stepCounter_ = 0;
    double lastGridIndex_ = -1.0;
    bool startNow_ = false;
    std::int64_t startAt_ = 0;
    Sounding sounding_[256];
    int soundingCount_ = 0;
    int lastStepNotes_[16] = {};
    int lastStepCount_ = 0;
    std::uint8_t direct_[128] = {}; // keys the chord stage sent straight to the voices

    // Clip player.
    bool clipRunning_ = false;
    std::int64_t clipStart_ = 0;
    int clipTranspose_ = 0, clipKey_ = -1, clipKeysDown_ = 0;
    double clipPos_ = 0.0; // beats into the clip at the start of the block (key trigger)
    bool transportWasPlaying_ = false;
};

} // namespace play
} // namespace zed
