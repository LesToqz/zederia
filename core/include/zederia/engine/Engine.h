// The sound engine (plan 3.1 to 3.4, 5.3, 5.4). Per voice: three oscillators (wavetable or
// Classic, two warp slots, unison), a Sub oscillator and a Texture sample player, mixed through
// per-source sends into two placeholder filter slots (the filters themselves arrive in
// milestone 3), then the amp envelope.
//
// Threading: construct, prepare() and reset() off the audio thread. process() is the audio
// callback: it never allocates, locks, waits, logs or touches files. Parameter values reach it
// through params(), which any thread may write. Tables, samples and curves are built elsewhere
// and handed over with setTable(), setTexture() and setWarpCurve(); the audio thread switches to
// them at the start of a block and gives the old ones back to be freed off the audio thread.
#pragma once

#include "zederia/engine/MidiEvent.h"
#include "zederia/engine/ParamStore.h"
#include "zederia/engine/SampleData.h"
#include "zederia/patch/Patch.h"
#include "zederia/table/TableData.h"

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

// The sources of a voice, in the order used by the warp slots' "source" parameter.
enum class Source : std::uint8_t { Osc1, Osc2, Osc3, Sub, Texture };
inline constexpr int kNumSources = 5;

class Engine {
public:
    // Modulation is evaluated every 16 samples and interpolated per sample in between. The grid
    // is fixed in absolute time, so the output does not depend on the host's block size.
    static constexpr int kControlInterval = 16;
    // A table or sample swapped in while notes play is crossfaded over this many samples.
    static constexpr int kSwapFadeSamples = 256;

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void prepare(double sampleRate);
    void reset();                       // silences everything and restarts the random sequence
    void setSeed(std::uint64_t seed);   // takes effect at the next reset()
    void loadPatch(const Patch& patch); // writes every value into params(); any non-audio thread
    // Offline renders use the patch's offline quality instead of its realtime quality.
    void setOffline(bool offline);

    ParamStore& params() noexcept;
    const ParamStore& params() const noexcept;

    // --- Tables, samples and curves (any non-audio thread) -----------------------------------
    // The wavetable of oscillator 0..2; null selects the built-in table. Notes that are playing
    // crossfade to the new table.
    void setTable(int osc, std::shared_ptr<const TableData> table);
    std::shared_ptr<const TableData> table(int osc) const;
    // The Texture player's sample; null selects the built-in noise.
    void setTexture(std::shared_ptr<const SampleData> sample);
    std::shared_ptr<const SampleData> texture() const;
    // The drawn phase curve of a warp slot (0 = A, 1 = B): any number of evenly spaced values
    // from 0 to 1 across the cycle; an empty span restores the default curve.
    void setWarpCurve(int osc, int slot, std::span<const float> values);
    // Frees objects the audio thread has finished with. The setters above do this too; call it
    // now and then when nothing else is being set.
    void collectGarbage();

    void process(const ProcessArgs& args) noexcept;

    int activeVoices() const noexcept; // any thread
    double sampleRate() const noexcept;
    // The oversampling decimators are minimum-delay designs: no quality mode adds latency.
    int latencySamples() const noexcept { return 0; }
    double tailSeconds() const noexcept; // how long sound can continue after the last note-off

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zed
