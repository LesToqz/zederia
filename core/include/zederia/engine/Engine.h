// The sound engine (plan 5.3, 5.4). Milestone 1 is the skeleton: voice allocation, sample-accurate
// MIDI, the control-rate tick and a placeholder band-limited saw through the amp envelope.
//
// Threading: construct, prepare() and reset() off the audio thread. process() is the audio
// callback: it never allocates, locks, waits, logs or touches files. Parameter values reach it
// through params(), which any thread may write.
#pragma once

#include "zederia/engine/MidiEvent.h"
#include "zederia/engine/ParamStore.h"
#include "zederia/patch/Patch.h"

#include <cstdint>
#include <memory>
#include <span>

namespace zed {

struct Transport {
    double bpm = 120.0;
    double ppqPosition = 0.0; // quarter notes since the start of the song
    bool playing = false;
};

struct ProcessArgs {
    float* left = nullptr; // output, `frames` samples each; overwritten
    float* right = nullptr;
    int frames = 0;
    std::span<const MidiEvent> midi; // sorted by offset, offsets inside the block
    Transport transport;
};

class Engine {
public:
    // Modulation is evaluated every 16 samples and interpolated per sample in between. The grid
    // is fixed in absolute time, so the output does not depend on the host's block size.
    static constexpr int kControlInterval = 16;

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void prepare(double sampleRate);
    void reset();                      // silences everything and restarts the random sequence
    void setSeed(std::uint64_t seed);  // takes effect at the next reset()
    void loadPatch(const Patch& patch); // writes every value into params(); any non-audio thread

    ParamStore& params() noexcept;
    const ParamStore& params() const noexcept;

    void process(const ProcessArgs& args) noexcept;

    int activeVoices() const noexcept; // any thread
    double sampleRate() const noexcept;
    int latencySamples() const noexcept { return 0; }
    double tailSeconds() const noexcept; // how long sound can continue after the last note-off

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zed
