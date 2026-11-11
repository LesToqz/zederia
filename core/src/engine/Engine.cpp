#include "zederia/engine/Engine.h"

#include "zederia/engine/Envelope.h"
#include "zederia/engine/VoiceAllocator.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/rt/Denormals.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

namespace zed {
namespace {

constexpr double kStealFadeSeconds = 0.004;
constexpr int kFixedPitchNote = 60;

// One voice of the placeholder sound source. Amplitude and pitch move in straight lines between
// control ticks: each has a current value and a per-sample step toward the value at the next tick.
struct Voice {
    bool active = false;
    bool done = false; // envelope finished; freed at the next tick, once the last ramp has reached zero
    int note = kFixedPitchNote;
    double velocity = 0.0;
    double phase = 0.0, inc = 0.0, incTarget = 0.0, incStep = 0.0;
    double envTarget = 0.0, envStep = 0.0; // envelope level at the next tick and its per-sample slope
    float ampL = 0.0f, ampR = 0.0f, stepL = 0.0f, stepR = 0.0f;
    Envelope env;
};

// Registry positions of the parameters the skeleton listens to.
struct Ids {
    int mode, polyphony, steal, priority, bendUp, bendDown, master, masterTune, reference, velocitySens;
    int oscOn, oscOctave, oscSemi, oscFine, oscCoarse, oscLevel, oscPan, oscPhase, oscPhaseRandom, oscKeytrack;
    int envDelay, envAttack, envHold, envDecay, envSustain, envRelease;

    Ids() {
        const auto& r = Registry::get();
        mode = r.indexOf("global.mode");
        polyphony = r.indexOf("global.polyphony");
        steal = r.indexOf("voice.steal");
        priority = r.indexOf("voice.priority");
        bendUp = r.indexOf("global.bend_up");
        bendDown = r.indexOf("global.bend_down");
        master = r.indexOf("global.master");
        masterTune = r.indexOf("global.master_tune");
        reference = r.indexOf("global.reference");
        velocitySens = r.indexOf("voice.velocity_sens");
        oscOn = r.indexOf("osc1.on");
        oscOctave = r.indexOf("osc1.octave");
        oscSemi = r.indexOf("osc1.semi");
        oscFine = r.indexOf("osc1.fine");
        oscCoarse = r.indexOf("osc1.coarse");
        oscLevel = r.indexOf("osc1.level");
        oscPan = r.indexOf("osc1.pan");
        oscPhase = r.indexOf("osc1.phase");
        oscPhaseRandom = r.indexOf("osc1.phase_random");
        oscKeytrack = r.indexOf("osc1.keytrack");
        envDelay = r.indexOf("env1.delay");
        envAttack = r.indexOf("env1.attack");
        envHold = r.indexOf("env1.hold");
        envDecay = r.indexOf("env1.decay");
        envSustain = r.indexOf("env1.sustain");
        envRelease = r.indexOf("env1.release");
    }
};

} // namespace

struct Engine::Impl {
    ParamStore store{Registry::get().size()};
    std::vector<double> p = std::vector<double>(static_cast<std::size_t>(Registry::get().size())); // audio thread's copy
    Ids ids;
    double sampleRate = 48000.0;

    VoiceAllocator allocator;
    VoiceAllocator::Actions actions;
    std::array<Voice, kVoiceSlots> voices;
    int activeCount = 0;
    std::atomic<int> activeForOthers{0};

    int samplesToTick = kControlInterval;
    bool dirty = true;      // a parameter changed since derived values were last computed
    bool snapMaster = true; // no ramp for the first block after a reset

    // Derived from parameters.
    EnvelopeTimes envTimes;
    double gainL = 0.0, gainR = 0.0, velocitySens = 0.5;
    double tuneSemis = 0.0, referenceHz = 440.0, bendUp = 2.0, bendDown = 2.0;
    double startPhase = 0.0, phaseRandom = 0.0;
    bool keytrack = true;
    double master = 0.0, masterTarget = 0.0, masterStep = 0.0;

    double bend = 0.0; // pitch wheel, -1..1
    double modWheel = 0.0;
    Rng rng;
    std::uint64_t seed = 1;

    double value(int index) const { return p[static_cast<std::size_t>(index)]; }

    // --- Parameters ---------------------------------------------------------------------------

    void refresh() {
        const double msToSamples = sampleRate / 1000.0;
        envTimes.delay = value(ids.envDelay) * msToSamples;
        envTimes.attack = value(ids.envAttack) * msToSamples;
        envTimes.hold = value(ids.envHold) * msToSamples;
        envTimes.decay = value(ids.envDecay) * msToSamples;
        envTimes.sustain = value(ids.envSustain);
        envTimes.release = value(ids.envRelease) * msToSamples;

        const double level = value(ids.oscOn) >= 0.5 ? dbToGain(value(ids.oscLevel)) : 0.0;
        const double angle = (value(ids.oscPan) + 1.0) * kPi * 0.25; // constant power, -3 dB at the centre
        gainL = level * std::cos(angle);
        gainR = level * std::sin(angle);
        velocitySens = value(ids.velocitySens);

        tuneSemis = value(ids.oscOctave) * 12.0 + value(ids.oscSemi) + value(ids.oscCoarse) +
                    (value(ids.oscFine) + value(ids.masterTune)) / 100.0;
        referenceHz = value(ids.reference);
        bendUp = value(ids.bendUp);
        bendDown = value(ids.bendDown);
        keytrack = value(ids.oscKeytrack) >= 0.5;
        startPhase = value(ids.oscPhase) / 360.0;
        phaseRandom = value(ids.oscPhaseRandom);

        masterTarget = dbToGain(value(ids.master));
        if (snapMaster) {
            master = masterTarget;
            masterStep = 0.0;
            snapMaster = false;
        } else {
            masterStep = (masterTarget - master) / samplesToTick;
        }

        actions.count = 0;
        allocator.configure(static_cast<VoiceMode>(static_cast<int>(value(ids.mode))),
                            static_cast<int>(value(ids.polyphony)),
                            static_cast<StealMode>(static_cast<int>(value(ids.steal))),
                            static_cast<NotePriority>(static_cast<int>(value(ids.priority))), actions);
        applyActions();
        for (auto& v : voices) {
            if (!v.active)
                continue;
            aimPitch(v);
            aimAmp(v);
        }
    }

    // --- Voices -------------------------------------------------------------------------------

    double velocityGain(const Voice& v) const { return 1.0 - velocitySens + velocitySens * v.velocity; }

    double pitchInc(const Voice& v) const {
        const double bendSemis = bend >= 0.0 ? bend * bendUp : bend * bendDown;
        const double note = (keytrack ? v.note : kFixedPitchNote) + tuneSemis + bendSemis;
        return std::min(noteToHz(note, referenceHz) / sampleRate, 0.49);
    }

    // Point the ramps at the values wanted at the next control tick.
    void aimPitch(Voice& v) {
        v.incTarget = pitchInc(v);
        v.incStep = (v.incTarget - v.inc) / samplesToTick;
    }
    void aimAmp(Voice& v) {
        const double gain = v.envTarget * velocityGain(v);
        v.stepL = static_cast<float>((gain * gainL - v.ampL) / samplesToTick);
        v.stepR = static_cast<float>((gain * gainR - v.ampR) / samplesToTick);
    }

    // Envelope level at this exact sample: the ramp runs backwards from its target.
    double envNow(const Voice& v) const { return v.envTarget - v.envStep * samplesToTick; }

    // After an envelope stage change at the current sample: advance to the next tick and re-aim.
    void restage(Voice& v, double from) {
        v.envTarget = v.env.advance(samplesToTick, envTimes);
        v.envStep = (v.envTarget - from) / samplesToTick;
        v.done = false;
        aimAmp(v);
    }

    void applyActions() {
        for (int i = 0; i < actions.count; ++i) {
            const VoiceAction& a = actions.items[static_cast<std::size_t>(i)];
            Voice& v = voices[static_cast<std::size_t>(a.slot)];
            switch (a.kind) {
            case VoiceAction::Kind::Start: {
                const bool wasActive = v.active;
                v = Voice{};
                v.active = true;
                v.note = a.note;
                v.velocity = a.velocity;
                const double phase = startPhase + phaseRandom * rng.uniform();
                v.phase = phase - std::floor(phase);
                v.inc = v.incTarget = pitchInc(v);
                v.env.trigger(0.0);
                restage(v, 0.0);
                activeCount += wasActive ? 0 : 1;
                break;
            }
            case VoiceAction::Kind::Retrigger: {
                if (!v.active)
                    break;
                const double from = envNow(v);
                v.note = a.note;
                v.velocity = a.velocity;
                v.env.trigger(from);
                restage(v, from);
                aimPitch(v);
                break;
            }
            case VoiceAction::Kind::Glide:
                if (v.active) {
                    v.note = a.note;
                    aimPitch(v);
                }
                break;
            case VoiceAction::Kind::Release:
                if (v.active) {
                    const double from = envNow(v);
                    v.env.release(from);
                    restage(v, from);
                }
                break;
            case VoiceAction::Kind::Kill:
                if (v.active) {
                    const double from = envNow(v);
                    v.env.kill(from, kStealFadeSeconds * sampleRate);
                    restage(v, from);
                }
                break;
            }
        }
        actions.count = 0;
    }

    void handle(const MidiEvent& e) {
        actions.count = 0;
        switch (e.type) {
        case MidiEvent::Type::NoteOn:
            allocator.noteOn(e.number, e.value, actions);
            break;
        case MidiEvent::Type::NoteOff:
            allocator.noteOff(e.number, actions);
            break;
        case MidiEvent::Type::PitchBend:
            bend = std::clamp(static_cast<double>(e.value), -1.0, 1.0);
            for (auto& v : voices)
                if (v.active)
                    aimPitch(v);
            break;
        case MidiEvent::Type::Controller:
            if (e.number == 64)
                allocator.sustainPedal(e.value >= 0.5f, actions);
            else if (e.number == 120)
                allocator.allSoundOff(actions);
            else if (e.number == 123)
                allocator.allNotesOff(actions);
            else if (e.number == 1)
                modWheel = e.value;
            break;
        case MidiEvent::Type::ChannelPressure:
        case MidiEvent::Type::PolyPressure:
            break; // modulation sources arrive in milestone 3
        }
        applyActions();
    }

    // The control-rate tick: every kControlInterval samples, on a grid fixed in absolute time.
    // Milestone 3 evaluates LFOs and the modulation matrix here; for now it advances the amp
    // envelope and settles the pitch ramps.
    void tick() {
        samplesToTick = kControlInterval;
        master = masterTarget;
        masterStep = 0.0;
        for (int i = 0; i < kVoiceSlots; ++i) {
            Voice& v = voices[static_cast<std::size_t>(i)];
            if (!v.active)
                continue;
            if (v.done) {
                v.active = false;
                --activeCount;
                allocator.voiceFinished(i);
                continue;
            }
            v.inc = v.incTarget;
            v.incStep = 0.0;
            const double from = v.envTarget;
            v.envTarget = v.env.advance(kControlInterval, envTimes);
            v.envStep = (v.envTarget - from) / kControlInterval;
            aimAmp(v);
            v.done = v.env.finished();
            allocator.setLevel(i, static_cast<float>(v.envTarget * velocityGain(v)));
        }
    }

    // --- Audio --------------------------------------------------------------------------------

    static void renderVoice(Voice& v, float* left, float* right, int frames) {
        double phase = v.phase, inc = v.inc;
        float ampL = v.ampL, ampR = v.ampR;
        for (int i = 0; i < frames; ++i) {
            // Saw with a polynomial correction around the step, which keeps aliasing down.
            double s = 2.0 * phase - 1.0;
            if (phase < inc) {
                const double x = phase / inc;
                s -= x + x - x * x - 1.0;
            } else if (phase > 1.0 - inc) {
                const double x = (phase - 1.0) / inc;
                s -= x * x + x + x + 1.0;
            }
            const auto sample = static_cast<float>(s);
            left[i] += sample * ampL;
            right[i] += sample * ampR;
            ampL += v.stepL;
            ampR += v.stepR;
            phase += inc;
            if (phase >= 1.0)
                phase -= 1.0;
            inc += v.incStep;
        }
        v.phase = phase;
        v.inc = inc;
        v.ampL = ampL;
        v.ampR = ampR;
    }

    void render(float* left, float* right, int frames) {
        std::fill(left, left + frames, 0.0f);
        std::fill(right, right + frames, 0.0f);
        if (activeCount == 0) {
            for (int i = 0; i < frames; ++i)
                master += masterStep;
            return;
        }
        for (auto& v : voices)
            if (v.active)
                renderVoice(v, left, right, frames);
        for (int i = 0; i < frames; ++i) {
            const auto gain = static_cast<float>(master);
            left[i] *= gain;
            right[i] *= gain;
            master += masterStep;
        }
    }
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {
    const auto& reg = Registry::get();
    for (const auto& def : reg.params())
        impl_->store.set(def.index, def.def);
    reset();
}

Engine::~Engine() = default;

void Engine::prepare(double sampleRate) {
    impl_->sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void Engine::reset() {
    auto& s = *impl_;
    s.allocator.reset();
    for (auto& v : s.voices)
        v = Voice{};
    s.activeCount = 0;
    s.activeForOthers.store(0, std::memory_order_relaxed);
    s.samplesToTick = kControlInterval;
    s.bend = 0.0;
    s.modWheel = 0.0;
    s.rng.seed(s.seed);
    s.snapMaster = true;
    s.dirty = true;
}

void Engine::setSeed(std::uint64_t seed) { impl_->seed = seed; }

void Engine::loadPatch(const Patch& patch) {
    for (int i = 0; i < impl_->store.size(); ++i)
        impl_->store.set(i, patch.get(i));
}

ParamStore& Engine::params() noexcept { return impl_->store; }
const ParamStore& Engine::params() const noexcept { return impl_->store; }
int Engine::activeVoices() const noexcept { return impl_->activeForOthers.load(std::memory_order_relaxed); }
double Engine::sampleRate() const noexcept { return impl_->sampleRate; }

double Engine::tailSeconds() const noexcept {
    return impl_->store.get(impl_->ids.envRelease) / 1000.0 + kStealFadeSeconds + 0.01;
}

void Engine::process(const ProcessArgs& args) noexcept {
    const rt::AudioThreadScope audioThread;
    const rt::ScopedNoDenormals noDenormals;
    auto& s = *impl_;
    if (args.frames <= 0 || args.left == nullptr || args.right == nullptr)
        return;

    s.store.drain([&s](int index, double value) {
        s.p[static_cast<std::size_t>(index)] = value;
        s.dirty = true;
    });
    if (s.dirty) {
        s.refresh();
        s.dirty = false;
    }

    const auto& midi = args.midi;
    std::size_t next = 0;
    int pos = 0;
    while (pos < args.frames) {
        while (next < midi.size() && midi[next].offset <= pos)
            s.handle(midi[next++]);
        int end = args.frames;
        if (next < midi.size())
            end = std::min(end, midi[next].offset);
        const int frames = std::min(end - pos, s.samplesToTick);
        s.render(args.left + pos, args.right + pos, frames);
        pos += frames;
        s.samplesToTick -= frames;
        if (s.samplesToTick == 0)
            s.tick();
    }
    while (next < midi.size()) // offsets at or past the end of the block
        s.handle(midi[next++]);
    s.activeForOthers.store(s.activeCount, std::memory_order_relaxed);
}

} // namespace zed
