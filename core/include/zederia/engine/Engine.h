// The sound engine (plan 3.1 to 3.7, 5.3, 5.4). Per voice: three oscillators (wavetable or
// Classic, two warp slots, unison), a Sub oscillator and a Texture sample player, mixed through
// per-source sends into two filter slots (in series, in parallel or anywhere between) and a
// direct bus, then the amp envelope. Four envelopes, eight LFOs, two Flux generators, eight
// macros, the MIDI and per-note sources and a 64-route matrix move any continuous parameter.
//
// Threading: construct, prepare() and reset() off the audio thread. process() is the audio
// callback: it never allocates, locks, waits, logs or touches files. Parameter values reach it
// through params(), which any thread may write. Everything that is not a parameter (tables,
// samples, shapes, routes, the tuning) is built elsewhere and handed over with the setters below;
// the audio thread switches to it at the start of a block and gives the old object back to be
// freed off the audio thread.
#pragma once

#include "zederia/engine/Filter.h"
#include "zederia/engine/MidiEvent.h"
#include "zederia/engine/ParamStore.h"
#include "zederia/engine/SampleData.h"
#include "zederia/patch/Patch.h"
#include "zederia/table/TableData.h"
#include "zederia/tuning/Tuning.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace zed {

struct Transport {
    double bpm = 120.0;
    double ppqPosition = 0.0; // quarter notes since the start of the song
    bool playing = false;
    int timeSigNumerator = 4, timeSigDenominator = 4;
    double barStartPpq = 0.0; // position of a bar line (any one): bar phase is counted from it
};

struct ProcessArgs {
    float* left = nullptr; // output, `frames` samples each; overwritten
    float* right = nullptr;
    int frames = 0;
    std::span<const MidiEvent> midi; // sorted by offset, offsets inside the block
    Transport transport;
    // Optional side-chain input for the envelope follower, `frames` samples each.
    const float* sidechainLeft = nullptr;
    const float* sidechainRight = nullptr;
};

// The sources of a voice, in the order used by the warp slots' "source" parameter.
enum class Source : std::uint8_t { Osc1, Osc2, Osc3, Sub, Texture };
inline constexpr int kNumSources = 5;

// One filter slot as it is right now, for the response display: the settings of the most recent
// voice after key tracking and modulation (the patch's own settings while no note plays).
struct FilterView {
    bool on = false;
    flt::Settings settings;
    int oversample = 1;
};

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
    void reset();                     // silences everything and restarts the random sequence
    void setSeed(std::uint64_t seed); // takes effect at the next reset()
    // Writes every value into params() and hands over the patch's routes, shapes, envelope
    // shapes, tempo-linked times, warp curves and tuning. Any non-audio thread. Tables and the
    // texture sample are loaded by reference elsewhere and set with setTable() and setTexture().
    void loadPatch(const Patch& patch);
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

    // --- Modulation structure (any non-audio thread) -----------------------------------------
    // The matrix: route i takes its amount from the parameter mod.(i+1).amount. Routes with an
    // unknown source or destination are ignored.
    void setRoutes(std::span<const ModRoute> routes);
    void setLfoShape(int lfo, const Shape& shape); // 0..7; an empty shape is a sine
    // The multi-segment shape of envelope 0..3 (used when its mode is "multi").
    void setEnvShape(int env, const EnvShape& shape);
    // Links one time of a standard envelope to the tempo: segment 0..4 is delay, attack, hold,
    // decay, release; `beats` is its length in quarter notes, 0 returns it to milliseconds.
    void setEnvSync(int env, int segment, double beats);

    // --- Tuning (any non-audio thread) -------------------------------------------------------
    void setTuning(std::shared_ptr<const Tuning> tuning); // null = equal temperament
    std::shared_ptr<const Tuning> tuning() const;
    // While locked, loadPatch() keeps the current tuning instead of taking the patch's.
    void setTuningLocked(bool locked);
    bool tuningLocked() const;

    // Frees objects the audio thread has finished with. The setters above do this too; call it
    // now and then when nothing else is being set.
    void collectGarbage();

    void process(const ProcessArgs& args) noexcept;

    // --- What the engine is doing (any thread; updated once per block) ------------------------
    int activeVoices() const noexcept;
    // Position of a modulation source in Registry::modSources(), or of "ccN" after them; -1 when unknown.
    static int sourceIndex(std::string_view id);
    // A source's current output; per-voice sources report the most recent voice.
    float sourceValue(int source) const noexcept;
    // A parameter's value after smoothing and modulation (the most recent voice's for per-voice
    // routes); the plain stored value when nothing modulates it.
    double liveValue(int paramIndex) const noexcept;
    FilterView filterView(int slot) const noexcept;
    // Magnitude (dB) and phase (radians) of a filter slot at the given frequencies, at its
    // current, modulated settings. Either output span may be empty.
    void filterResponse(int slot, std::span<const double> hz, std::span<float> magnitudeDb,
                        std::span<float> phase) const;

    double sampleRate() const noexcept;
    // The oversampling filters are minimum-delay designs: no quality mode adds latency.
    int latencySamples() const noexcept { return 0; }
    double tailSeconds() const noexcept; // how long sound can continue after the last note-off

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zed
