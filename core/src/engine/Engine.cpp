#include "zederia/engine/Engine.h"

#include "zederia/dsp/HalfBand.h"
#include "zederia/dsp/Simd.h"
#include "zederia/engine/Envelope.h"
#include "zederia/engine/Lfo.h"
#include "zederia/engine/ModMath.h"
#include "zederia/engine/OscKernel.h"
#include "zederia/engine/VoiceAllocator.h"
#include "zederia/fx/Effect.h"
#include "zederia/play/Player.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/rt/Denormals.h"
#include "zederia/rt/Handoff.h"
#include "zederia/util/Math.h"

#include "../fx/FxDsp.h"
#include "../fx/FxImpl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(disable : 4324) // structures padded because of alignas: intended
#endif

namespace zed {
namespace {

#include "EngineHelpers.inl"
#include "EngineTypes.inl"
#include "EngineFxTypes.inl"

constexpr float kLimiterCeiling = 0.96605088f; // output protection: -0.3 dBFS

// What liveValue() finds for a parameter nothing modulates.
constexpr float kNotModulated = -1.0e30f;

} // namespace

struct Engine::Impl {
    const Registry& reg = Registry::get();
    ParamStore store{reg.size()};
    // Parameter values on the audio thread (see EngineMod.inl).
    std::vector<double> p, nb, nt, pe;
    std::vector<std::uint8_t> isFloat, moving, moduleOf;
    std::vector<int> movingList;
    int movingCount = 0;
    std::vector<std::int16_t> polySlotOf, globSlotOf;
    std::vector<double> divisionBeats;
    Ids ids;
    double sampleRate = 48000.0;
    std::atomic<bool> offline{false};
    bool offlineSeen = false;

    VoiceAllocator allocator;
    VoiceAllocator::Actions actions;
    std::array<Voice, kVoiceSlots> voices;
    std::vector<float> filterMemory; // delay lines of every voice's two filters
    int activeCount = 0;
    int lastStarted = -1; // voice slot of the most recent note
    std::atomic<int> activeForOthers{0};

    int samplesToTick = kInterval;
    bool dirty = true;         // a parameter or a piece of structure changed since refresh() last ran
    bool derivedDirty = true;  // pe changed since derive() last ran
    bool snapSmoothing = true; // no glides for the first block after a reset
    // Profiling: seconds per section while switched on (see Engine::setProfiling).
    enum ProfSection { kProfSources, kProfFilters, kProfVoiceTick, kProfShared, kProfPlay, kProfOutput, kProfFx, kProfCount = kProfFx + 13 };
    bool profiling = false;
    double prof[kProfCount] = {};
    using ProfClock = std::chrono::steady_clock;
    ProfClock::time_point profNow() const { return profiling ? ProfClock::now() : ProfClock::time_point{}; }
    void profAdd(int section, ProfClock::time_point since) {
        if (profiling)
            prof[section] += std::chrono::duration<double>(ProfClock::now() - since).count();
    }
    // A change of quality while notes play: the voices duck around the switch.
    int appliedQuality = 1, pendingQuality = -1, duckState = 0; // 0 none, 1 fading out, 2 fading in
    float qualityDuck = 1.0f;
    double smoothing = 0.1;    // control-rate one-pole coefficient of the parameter smoothing
    float controllerSmoothing = 0.1f;
    float warmTilt = 0.4f; // one-pole coefficient of the Warmth tilt (3.5 kHz)

    // Derived from parameters.
    double velocitySens = 0.5;
    double referenceHz = 440.0, bendUp = 2.0, bendDown = 2.0, mpeBendRange = 48.0, masterTuneSemis = 0.0;
    double master = 0.0, masterAim = 0.0, masterTarget = 0.0, masterStep = 0.0;
    float followerAttack = 0.01f, followerRelease = 0.001f, followerLevel = 0.0f;
    bool followerSidechain = false, followerUsed = false;
    int oversample = 2;
    const dsp::DecimatorDesign* decimator = nullptr;
    int polyphony = 16;
    bool mpe = false, glideLegato = false, glideScaled = false;
    float cutoffOctaves = 11.4f; // the cutoff range: what a route amount of 100 % moves it by

    OscShared osc[kOscCount];
    SubShared sub;
    TextureShared texture;
    FilterShared filter[kNumFilters];
    EnvShared env[kNumEnvs];
    LfoShared lfo[kNumLfos];
    FluxShared flux[kNumFlux];
    Routing routing;
    bool tapped[kNumSources] = {}; // sources a route reads as audio

    rt::Handoff<TableData> tables[kOscCount];
    rt::Handoff<SampleData> sampleSlot;
    rt::Handoff<WarpCurve> curves[kOscCount][2];
    rt::Handoff<RouteTable> routes;
    rt::Handoff<LfoTable> lfoTables[kNumLfos];
    rt::Handoff<EnvShapeData> envShapes[kNumEnvs];
    rt::Handoff<Tuning> tuningSlot;
    rt::Handoff<play::PlayData> playData;
    play::Player player;
    MidiEvent playEvents[play::Player::kMaxEvents];
    const Tuning* tuningNow = nullptr;
    LfoTable sineTable;
    Tuning equalTuning;
    std::atomic<double> envSyncBeats[kNumEnvs][5];
    std::atomic<bool> envSyncDirty{true};
    std::atomic<bool> tuningLocked{false};

    // The matrix as the audio thread evaluates it.
    ActiveRoute polyRoutes[kNumModRoutes], globRoutes[kNumModRoutes];
    int polyCount = 0, globCount = 0;
    int polyDest[kNumModRoutes] = {}, globDest[kNumModRoutes] = {};
    int polyDestCount = 0, globDestCount = 0;
    float gOff[kNumModRoutes] = {}; // sum of the shared routes per destination, normalized
    std::uint32_t polyMask = 0;     // parameter groups a per-voice route reaches
    bool srcIsVoice[kNamedSources] = {};
    bool srcBipolar[kSrcCount] = {};
    float gsrc[kSrcCount] = {};          // shared sources at the next tick
    float ctl[kSrcCount] = {};           // controllers as they last arrived
    float leadSrc[kNamedSources] = {};   // per-voice sources of the most recent voice
    std::uint8_t ccUsed[128] = {};
    int ccUsedCount = 0;

    // MIDI.
    bool keyDown[128] = {};
    int keysDown = 0, keysHeldBefore = 0, eventChannel = 0;
    float releaseVelocity = 0.0f;
    float polyPressure[128] = {};
    float channelBend[16] = {}, channelPressure[16] = {}, channelSlide[16] = {};
    double lastKey = -1.0;   // where the last note's pitch is headed; glides start there
    float alternate = -1.0f; // flips with every note

    // Transport.
    double bpm = 120.0, blockPpq = 0.0, ppqNow = 0.0, ppqTarget = 0.0, barStartPpq = 0.0, beatsPerBar = 4.0;
    bool playing = false;
    const float* sidechain[2] = {nullptr, nullptr};

    // For other threads.
    std::unique_ptr<std::atomic<float>[]> live;
    std::atomic<float> liveSrc[kSrcCount];
    flt::Settings viewSettings[kNumFilters];
    struct ViewAtoms {
        std::atomic<bool> on{false}, levelMode{false};
        std::atomic<int> type{1};
        std::atomic<float> cutoff[2], resonance{0.0f}, drive{0.0f}, character{0.5f}, mix{1.0f};
    } view[kNumFilters];
    std::atomic<int> viewOversample{1};
    std::atomic<double> bpmForOthers{120.0};

    Rng rng;
    std::uint64_t seed = 1;

    // --- Effects rack (EngineRack.inl) ---------------------------------------------------------
    std::vector<const ParamDef*> defs; // the definition governing each value: an effect's own for rack slots
    // Message side: the layout last offered, guarded by rackMutex (never taken by the audio thread).
    rt::Mutex rackMutex;
    std::uint32_t rackUid[kRackSlots] = {};
    std::atomic<int> rackType[kRackSlots]; // read by tailSeconds() from any thread
    std::uint32_t nextUid = 1;
    std::atomic<RackPlan*> rackPending{nullptr};
    RetireBin<fx::Effect, 64> fxBin;
    RetireBin<RackPlan, 8> planBin;
    std::atomic<bool> rackLockedFlag{false};
    // Audio side.
    FxInstance pool[kFxPool];
    int chain[kNumFxLanes][kNumFxSlots];
    int chainCount[kNumFxLanes] = {};
    RackPlan* staged = nullptr;
    RackPlan* planHeld = nullptr;
    fx::Effect* fxHeld[kFxPool] = {};
    int fxHeldCount = 0;
    int bRouting = 0, bRoutingWanted = 0;
    bool bSwitching = false;
    fx::Context fxCtx;
    float fxMix = 1.0f, fxMixTarget = 1.0f, fxFadeStep = 0.002f;
    bool fxCleared = false;
    fx::Ramp sendRamp[3]; // the three buses' shares into lane B
    bool rackSplit = false;
    bool outputSilent = false; // the rack passed exact silence in the current run
    // Output stage.
    float dcCoef = 0.9993f, dcX[2] = {}, dcY[2] = {};
    bool dcOn = true;
    int protection = 0;
    fx::PeakLimiter outLimiter;
    dsp::DecimatorState clipUp[2], clipDown[2];
    // The protection clipper runs at a fixed 4x in every quality mode, so it never switches rate.
    int clipFactor = 4;
    const dsp::DecimatorDesign* clipDesign = &dsp::decimatorDesign(true);
    // For other threads.
    std::atomic<std::uint8_t> fxState[kRackSlots];
    std::atomic<float> fxMeterValue[kRackSlots];
    std::atomic<int> fxRunning{0};
    std::atomic<int> arpStepShown{-1}; // the arpeggiator step that sounded last, for the interface

    void updateSmoothing() {
        smoothing = 1.0 - std::exp(-static_cast<double>(kInterval) / (kSmoothingSeconds * sampleRate));
        controllerSmoothing = static_cast<float>(smoothing);
        warmTilt = static_cast<float>(1.0 - std::exp(-2.0 * kPi * 3500.0 / sampleRate));
        fxFadeStep = static_cast<float>(1.0 / (kFxFadeSeconds * sampleRate));
        dcCoef = static_cast<float>(1.0 - 2.0 * kPi * 5.0 / sampleRate);
        outLimiter.setRelease(80.0, sampleRate);
    }

    Impl() {
        const auto count = static_cast<std::size_t>(reg.size());
        p.assign(count, 0.0);
        nb.assign(count, 0.0);
        nt.assign(count, 0.0);
        pe.assign(count, 0.0);
        isFloat.assign(count, 0);
        moving.assign(count, 0);
        moduleOf.assign(count, static_cast<std::uint8_t>(kModOther));
        movingList.assign(count, 0);
        polySlotOf.assign(count, -1);
        globSlotOf.assign(count, -1);
        defs.assign(count, nullptr);
        for (auto& t : rackType)
            t.store(-1, std::memory_order_relaxed);
        for (auto& lane : chain)
            std::fill(std::begin(lane), std::end(lane), -1);
        for (auto& s : fxState)
            s.store(0, std::memory_order_relaxed);
        for (auto& m : fxMeterValue)
            m.store(0.0f, std::memory_order_relaxed);
        live = std::make_unique<std::atomic<float>[]>(count);
        for (const ParamDef& def : reg.params()) {
            const auto i = static_cast<std::size_t>(def.index);
            defs[i] = &def;
            isFloat[i] = def.type == ParamType::Float ? 1 : 0;
            live[i].store(kNotModulated, std::memory_order_relaxed);
            const std::string& m = def.module;
            int module = kModOther;
            if (m.size() == 4 && m.compare(0, 3, "osc") == 0)
                module = kModOsc + (m[3] - '1');
            else if (m == "sub")
                module = kModSub;
            else if (m == "texture")
                module = kModTexture;
            else if (m.size() == 7 && m.compare(0, 6, "filter") == 0)
                module = kModFilter + (m[6] - '1');
            else if (m.size() == 4 && m.compare(0, 3, "env") == 0)
                module = kModEnv + (m[3] - '1');
            else if (m.size() == 4 && m.compare(0, 3, "lfo") == 0)
                module = kModLfo + (m[3] - '1');
            else if (m == "voice")
                module = kModVoice;
            moduleOf[i] = static_cast<std::uint8_t>(module);
        }
        for (const std::string& token : tempoDivisions())
            divisionBeats.push_back(std::max(divisionInBeats(token), 1.0e-3));
        const ParamDef& cutoff = reg.at(ids.filter[0].cutoff);
        cutoffOctaves = static_cast<float>(std::log2(cutoff.max / cutoff.min));

        // The source positions used throughout must be the registry's.
        const auto sources = reg.modSources();
        assert(static_cast<int>(sources.size()) == kNamedSources);
        assert(sources[kSrcLfo].id == "lfo1" && sources[kSrcMacro].id == "macro1" && sources[kSrcVelocity].id == "velocity");
        assert(sources[kSrcUnisonIndex].id == "unison_index" && sources[kSrcAudio].id == "osc1_audio");
        assert(sources[kSrcBarPhase].id == "bar_phase");
        for (int i = 0; i < kNamedSources && i < static_cast<int>(sources.size()); ++i)
            srcIsVoice[i] = sources[static_cast<std::size_t>(i)].scope == Scope::Voice;
        srcIsVoice[kSrcUnisonIndex] = false; // handled per lane, see classify()
        for (int k = 0; k < kNumFlux; ++k)
            srcBipolar[kSrcFlux + k] = true;
        for (int j = 0; j < kNumSources; ++j)
            srcBipolar[kSrcAudio + j] = true;
        srcBipolar[kSrcNote] = srcBipolar[kSrcPitchBend] = srcBipolar[kSrcAlternate] = srcBipolar[kSrcUnisonIndex] = true;

        updateSmoothing();
        // Everything with lazy one-time setup is touched here, off the audio thread.
        decimator = &dsp::decimatorDesign(false);
        (void)dsp::decimatorDesign(true);
        (void)osc::defaultWarpCurve();
        (void)dsp::cpuHasAvx2();
        (void)flt::typeCount();
        buildLfoTable(Shape{}, sineTable);
        tuningNow = &equalTuning;
        for (int i = 0; i < kOscCount; ++i) {
            tables[i].set(TableData::builtin());
            tables[i].take();
            osc[i].table = tables[i].current();
            osc[i].curve[0] = osc[i].curve[1] = osc::defaultWarpCurve();
            Rng fixed(0x5eed0000u + static_cast<std::uint64_t>(i)); // "consistent" start phases: the same for every note
            for (float& phase : osc[i].consistentPhase)
                phase = static_cast<float>(fixed.uniform());
        }
        sampleSlot.set(SampleData::builtin());
        sampleSlot.take();
        playData.set(play::PlayData::fromPatch(Patch{}));
        playData.take();
        texture.sample = sampleSlot.current();
        for (auto& l : lfo)
            l.table = &sineTable;
        for (auto& segments : envSyncBeats)
            for (auto& beats : segments)
                beats.store(0.0, std::memory_order_relaxed);
        for (auto& s : liveSrc)
            s.store(0.0f, std::memory_order_relaxed);
        for (auto& v : view) {
            v.cutoff[0].store(2000.0f, std::memory_order_relaxed);
            v.cutoff[1].store(2000.0f, std::memory_order_relaxed);
        }
        prepareFilters();
    }

    // Gives every voice's filters their delay memory for the current sample rate.
    void prepareFilters() {
        const int length = flt::delayLengthFor(sampleRate);
        filterMemory.assign(static_cast<std::size_t>(kVoiceSlots) * kNumFilters * 2 * static_cast<std::size_t>(length), 0.0f);
        std::size_t at = 0;
        for (auto& v : voices) {
            for (auto& f : v.filter) {
                f.filter.prepare(sampleRate, filterMemory.data() + at, length);
                f.running = false;
                at += 2 * static_cast<std::size_t>(length);
            }
        }
    }

    ~Impl() {
        for (FxInstance& x : pool)
            delete x.effect;
        delete rackPending.exchange(nullptr);
        delete staged;
        delete planHeld;
        for (int i = 0; i < fxHeldCount; ++i)
            delete fxHeld[i];
    }

#include "EngineMod.inl"
#include "EngineVoice.inl"
#include "EngineRender.inl"
#include "EngineRack.inl"

    // --- Structure ----------------------------------------------------------------------------

    // Everything that is not a continuous value: switches, choices, counts, the route lists. Runs
    // at the start of a block in which something changed.
    void refresh() {
        int quality = std::clamp(choice(offlineSeen ? ids.qualityOffline : ids.quality), 0, 3);
        if (quality != appliedQuality && !snapSmoothing && activeCount > 0) {
            // Notes are playing: their oversampling filters cannot change rate without a click. The
            // voices fade out first (render() switches when they are silent) and fade back in.
            pendingQuality = quality;
            if (duckState != 1)
                duckState = 1;
            quality = appliedQuality;
        } else {
            appliedQuality = quality;
            pendingQuality = -1;
        }
        const int newOversample = 1 << quality; // Draft 1x, Standard 2x, High 4x, Ultra 8x
        decimator = &dsp::decimatorDesign(quality >= 2);
        const bool qualityChanged = newOversample != oversample;
        oversample = newOversample;
        polyphony = static_cast<int>(p[static_cast<std::size_t>(ids.polyphony)]);
        mpe = flag(ids.mpe);
        glideLegato = choice(ids.glideMode) == 1;
        glideScaled = flag(ids.glideScaled);
        followerSidechain = choice(ids.followerSource) == 1;
        dcOn = flag(ids.dcFilter);
        if (!dcOn)
            dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0.0f;
        const int newProtection = std::clamp(choice(ids.limiter), 0, 2);
        if (newProtection != protection)
            outLimiter.reset();
        protection = newProtection;
        fxMixTarget = flag(ids.fxOff) ? 0.0f : 1.0f;
        if (snapSmoothing) { // the first block after a reset: no fades
            fxMix = fxMixTarget;
            for (int b = 0; b < 3; ++b)
                sendRamp[b].aim(static_cast<float>(value(ids.fxBalance[b])), true);
        }
        requestBRouting(std::clamp(choice(ids.fxBRouting), 0, 1));

        bool relayout[kOscCount] = {};
        for (int i = 0; i < kOscCount; ++i) {
            const Ids::Osc& id = ids.osc[i];
            OscShared& o = osc[i];
            o.turnedOn = flag(id.on) && !o.on;
            o.on = flag(id.on);
            o.classic = choice(id.engine) == 1;
            o.keytrack = flag(id.keytrack);
            o.continuePhase = choice(id.phaseMode) == 1;
            const int count = std::clamp(choice(id.voices), 1, kMaxLanes);
            const auto law = static_cast<UnisonLaw>(choice(id.law));
            const auto stack = static_cast<UnisonStack>(choice(id.stack));
            const osc::RenderFn render = osc::pickRender(count);
            const int width = render == osc::renderScalar ? 1 : render == osc::renderSse2 ? 4 : 8;
            const int padded = std::min((count + width - 1) / width * width, kMaxLanes);
            relayout[i] = count != o.count || law != o.law || padded != o.padded;
            bool changed = relayout[i] || stack != o.stack;
            o.render = render;
            o.padded = padded;
            o.count = count;
            o.law = law;
            o.stack = stack;
            o.unisonPhase = static_cast<UnisonPhase>(choice(id.unisonPhase));
            for (int s = 0; s < 2; ++s) {
                auto mode = static_cast<WarpMode>(std::clamp(choice(id.warpMode[s]), 0, osc::kNumWarpModes - 1));
                if (o.classic && osc::warpIsPhaseShaper(mode))
                    mode = WarpMode::Off; // phase shaping needs a wavetable
                changed = changed || mode != o.mode[s];
                o.mode[s] = mode;
                o.modSource[s] = std::clamp(choice(id.warpSource[s]), 0, kNumSources - 1);
            }
            if (changed) {
                o.lanesDirty = true;
                ++o.structure;
            }
        }
        sub.turnedOn = flag(ids.sub.on) && !sub.on;
        sub.on = flag(ids.sub.on);
        sub.direct = flag(ids.sub.direct);
        sub.shape = static_cast<SubShape>(choice(ids.sub.shape));
        texture.turnedOn = flag(ids.texture.on) && !texture.on;
        texture.on = flag(ids.texture.on);
        texture.direct = flag(ids.texture.direct);
        texture.loop = choice(ids.texture.mode) == 1;
        texture.keytrack = flag(ids.texture.keytrack);
        for (int f = 0; f < kNumFilters; ++f) {
            filter[f].turnedOn = flag(ids.filter[f].on) && !filter[f].on;
            filter[f].on = flag(ids.filter[f].on);
            filter[f].type = choice(ids.filter[f].type);
            filter[f].levelMode = choice(ids.filter[f].mixMode) == 1;
        }
        const bool syncChanged = envSyncDirty.exchange(false, std::memory_order_acq_rel);
        for (int k = 0; k < kNumEnvs; ++k) {
            env[k].multi = ids.env[k].mode >= 0 && choice(ids.env[k].mode) == 1;
            if (syncChanged)
                for (int j = 0; j < 5; ++j)
                    env[k].syncBeats[j] = envSyncBeats[k][j].load(std::memory_order_relaxed);
        }
        for (int k = 0; k < kNumLfos; ++k) {
            const auto& id = ids.lfo[k];
            LfoShared& l = lfo[k];
            l.mode = static_cast<LfoMode>(std::clamp(choice(id.mode), 0, 3));
            l.sync = flag(id.sync);
            l.beats = divisionBeats[static_cast<std::size_t>(choice(id.division))];
            l.anchor = flag(id.anchor);
            l.p.unipolar = choice(id.polarity) == 1;
            l.global = choice(id.scope) == 1;
            l.audioRate = flag(id.audioRate);
        }
        for (int k = 0; k < kNumFlux; ++k) {
            flux[k].type = static_cast<FluxType>(std::clamp(choice(ids.flux[k].type), 0, 3));
            flux[k].global = choice(ids.flux[k].scope) == 1;
        }

        classify();
        const bool rateChanged = updateRouting() || qualityChanged;
        derive();
        derivedDirty = false;

        actions.count = 0;
        allocator.configure(static_cast<VoiceMode>(choice(ids.mode)), polyphony, static_cast<StealMode>(choice(ids.steal)),
                            static_cast<NotePriority>(choice(ids.priority)), actions);
        applyActions();
        for (auto& v : voices) {
            if (!v.active)
                continue;
            for (int i = 0; i < kOscCount; ++i) {
                if (relayout[i] && osc[i].on)
                    relayoutVoice(v, i);
                else if (osc[i].turnedOn)
                    aimOsc(v, i, true); // its lanes may hold values from another table or none at all
            }
            if (sub.turnedOn)
                aimSub(v, true);
            if (texture.turnedOn)
                aimTexture(v, true);
            if (rateChanged)
                resetRates(v);
            evalVoice(v);
            aimVoice(v, false, rateChanged);
        }
    }

    // Works out the evaluation order and the oversampling of every source. Returns true when a
    // source's rate changed.
    bool updateRouting() {
        Routing r;
        bool edge[kOscCount][kOscCount] = {}; // edge[j][i]: j modulates i
        for (int i = 0; i < kOscCount; ++i) {
            if (!osc[i].on)
                continue;
            for (int s = 0; s < 2; ++s) {
                if (!osc::warpUsesModulator(osc[i].mode[s]))
                    continue;
                const int j = osc[i].modSource[s];
                r.usedAsModulator[j] = true;
                if (j < kOscCount && j != i)
                    edge[j][i] = true;
            }
        }
        // Modulators first. Sub and Texture never depend on anything; oscillators that form a loop
        // keep their numeric order.
        int placed = 0;
        bool done[kOscCount] = {};
        r.order[placed++] = kSub;
        r.order[placed++] = kTexture;
        for (int round = 0; round < kOscCount; ++round) {
            int pick = -1;
            for (int i = 0; i < kOscCount && pick < 0; ++i) {
                if (done[i])
                    continue;
                bool waiting = false;
                for (int j = 0; j < kOscCount; ++j)
                    waiting = waiting || (edge[j][i] && !done[j]);
                if (!waiting)
                    pick = i;
            }
            if (pick < 0) // a loop: take the lowest oscillator that is left
                for (int i = 0; i < kOscCount && pick < 0; ++i)
                    if (!done[i])
                        pick = i;
            done[pick] = true;
            r.order[placed++] = pick;
        }
        int position[kNumSources] = {};
        for (int k = 0; k < kNumSources; ++k)
            position[r.order[k]] = k;
        for (int i = 0; i < kOscCount; ++i) {
            if (!osc[i].on)
                continue;
            for (int s = 0; s < 2; ++s) {
                if (!osc::warpUsesModulator(osc[i].mode[s]))
                    continue;
                r.delayed[i][s] = position[osc[i].modSource[s]] >= position[i];
                r.loop = r.loop || r.delayed[i][s];
            }
        }
        routing = r;

        // Warped and modulated paths, and the modulators that feed them, run oversampled. So
        // does the Classic engine: its step correction is cheap but leaves more aliasing than
        // the wavetable levels do, and oversampling is what pushes that down.
        bool changed = false;
        for (int i = 0; i < kOscCount; ++i) {
            const bool warped = osc[i].mode[0] != WarpMode::Off || osc[i].mode[1] != WarpMode::Off;
            const int rate = warped || r.usedAsModulator[i] || osc[i].classic ? oversample : 1;
            changed = changed || rate != osc[i].oversample;
            osc[i].oversample = rate;
        }
        const int subRate = r.usedAsModulator[kSub] ? oversample : 1;
        const int textureRate = r.usedAsModulator[kTexture] ? oversample : 1;
        changed = changed || subRate != sub.oversample || textureRate != texture.oversample;
        sub.oversample = subRate;
        texture.oversample = textureRate;
        return changed;
    }

    // --- Things handed over from other threads ---------------------------------------------------

    template <class T>
    static bool swapIn(rt::Handoff<T>& slot) {
        const bool taken = slot.take();
        slot.retirePrevious();
        return taken;
    }

    // Routes, shapes and the tuning: switched at the start of a block, before refresh().
    void takeStructure() {
        if (swapIn(routes))
            dirty = true;
        for (int k = 0; k < kNumLfos; ++k) {
            swapIn(lfoTables[k]);
            const LfoTable* table = lfoTables[k].current();
            lfo[k].table = table != nullptr ? table : &sineTable;
        }
        for (auto& shape : envShapes)
            if (swapIn(shape))
                dirty = true;
        swapIn(tuningSlot);
        swapIn(playData);
        const Tuning* tuning = tuningSlot.current();
        tuningNow = tuning != nullptr ? tuning : &equalTuning;
        if (envSyncDirty.load(std::memory_order_relaxed))
            dirty = true;
    }

    // Tables, samples and warp curves.
    void takeHandoffs() {
        for (int i = 0; i < kOscCount; ++i) {
            OscShared& o = osc[i];
            if (o.fadeLeft <= 0) {
                o.oldTable = nullptr;
                tables[i].retirePrevious();
            }
            if (tables[i].previous() == nullptr && tables[i].take()) {
                o.oldTable = tables[i].previous();
                o.table = tables[i].current();
                o.fadeLeft = activeCount > 0 && o.on && !o.classic && o.oldTable != nullptr ? kFade : 0;
                for (auto& v : voices) {
                    if (!v.active || !o.on)
                        continue;
                    OscVoice& ov = v.osc[i];
                    ov.fade = ov.main; // keeps playing the old table from the same phases
                    ov.fadeLeft = o.fadeLeft;
                    aimOsc(v, i, true);
                }
            }
            for (int s = 0; s < 2; ++s) {
                swapIn(curves[i][s]);
                const WarpCurve* curve = curves[i][s].current();
                o.curve[s] = curve != nullptr ? curve->values.data() : osc::defaultWarpCurve();
            }
        }
        if (texture.fadeLeft <= 0) {
            texture.oldSample = nullptr;
            sampleSlot.retirePrevious();
        }
        if (sampleSlot.previous() == nullptr && sampleSlot.take()) {
            texture.oldSample = sampleSlot.previous();
            texture.sample = sampleSlot.current();
            texture.fadeLeft = activeCount > 0 && texture.on && texture.oldSample != nullptr ? kFade : 0;
            for (auto& v : voices) {
                if (!v.active)
                    continue;
                v.texture.fade = v.texture.main;
                v.texture.fadeLeft = texture.fadeLeft;
                // The new sample starts from the same relative place.
                const double oldLength = std::max(texture.oldSample != nullptr ? texture.oldSample->frames : 1, 1);
                v.texture.main.position = std::clamp(v.texture.main.position / oldLength, 0.0, 1.0) *
                                          std::max(texture.sample->frames - 1, 0);
                v.texture.main.finished = false;
                if (texture.on)
                    aimTexture(v, true);
            }
        }
    }

    // --- The control tick: every kControlInterval samples, on a grid fixed in absolute time -------

    void tick() {
        samplesToTick = kInterval;
        master = masterAim;
        const auto sharedStart = profNow();
        globalTick(kInterval, false);
        masterAim = masterTarget;
        masterStep = (masterAim - master) / kInterval;
        rackTick();
        profAdd(kProfShared, sharedStart);
        const auto voicesStart = profNow();
        for (int slot = 0; slot < kVoiceSlots; ++slot) {
            Voice& v = voices[static_cast<std::size_t>(slot)];
            if (!v.active)
                continue;
            if (v.done) {
                v.active = false;
                --activeCount;
                allocator.voiceFinished(slot);
                continue;
            }
            voiceTick(v, slot);
        }
        profAdd(kProfVoiceTick, voicesStart);
    }

    // The play parameters as the player reads them.
    play::Settings playSettings() const {
        play::Settings st;
        st.arpOn = flag(ids.arp.on);
        st.arpMode = static_cast<play::ArpMode>(std::clamp(choice(ids.arp.mode), 0, 7));
        st.octaves = std::clamp(choice(ids.arp.octaves), 1, 4);
        st.stepBeats = divisionBeats[static_cast<std::size_t>(std::clamp(choice(ids.arp.rate), 0, static_cast<int>(divisionBeats.size()) - 1))];
        st.gate = value(ids.arp.gate);
        st.swing = value(ids.arp.swing);
        st.latch = flag(ids.arp.latch);
        st.restart = flag(ids.arp.restart);
        st.steps = std::clamp(choice(ids.arp.steps), 1, kMaxArpSteps);
        st.pattern = std::clamp(choice(ids.arp.pattern) - 1, 0, kNumArpPatterns - 1);
        st.chordMemory = flag(ids.chord.memory);
        st.scaleLock = flag(ids.chord.scaleLock);
        st.key = std::clamp(choice(ids.chord.key), 0, 11);
        st.scale = choice(ids.chord.scale);
        st.filter = choice(ids.chord.lockMode) == 1;
        st.strumMs = value(ids.chord.strum);
        st.strumDirection = std::clamp(choice(ids.chord.strumDirection), 0, 2);
        st.clipOn = flag(ids.clip.on);
        st.clipSlot = std::clamp(choice(ids.clip.slot) - 1, 0, kNumClips - 1);
        st.clipFromKey = choice(ids.clip.trigger) == 1;
        st.clipLatch = flag(ids.clip.latch);
        st.clipLoop = flag(ids.clip.loop);
        return st;
    }

    // Once per block: what other threads may look at.
    void publish() {
        activeForOthers.store(activeCount, std::memory_order_relaxed);
        const Voice* lead = leadVoice();
        const bool sounding = lead != nullptr && lead->active;
        for (int i = 0; i < kNamedSources; ++i)
            liveSrc[i].store(srcIsVoice[i] ? (sounding ? leadSrc[i] : 0.0f) : gsrc[i], std::memory_order_relaxed);
        for (int n = 0; n < ccUsedCount; ++n)
            liveSrc[kSrcCc + ccUsed[n]].store(gsrc[kSrcCc + ccUsed[n]], std::memory_order_relaxed);
        for (int n = 0; n < globDestCount; ++n)
            live[static_cast<std::size_t>(globDest[n])].store(static_cast<float>(pe[static_cast<std::size_t>(globDest[n])]),
                                                              std::memory_order_relaxed);
        for (int n = 0; n < polyDestCount; ++n) {
            const int index = polyDest[n];
            const double shown = sounding ? vval(*lead, index) : pe[static_cast<std::size_t>(index)];
            live[static_cast<std::size_t>(index)].store(static_cast<float>(shown), std::memory_order_relaxed);
        }
        for (int f = 0; f < kNumFilters; ++f) {
            flt::Settings s = viewSettings[f];
            if (!sounding || !filter[f].on) { // no voice to show: the patch's own settings
                s.type = filter[f].type;
                s.cutoffHz[0] = s.cutoffHz[1] = filter[f].t.cutoff;
                s.resonance = filter[f].t.resonance;
                s.drive = filter[f].t.drive;
                s.character = filter[f].t.character;
                s.mix = filter[f].t.mix;
                s.levelMode = filter[f].levelMode;
            }
            ViewAtoms& a = view[f];
            a.on.store(filter[f].on, std::memory_order_relaxed);
            a.levelMode.store(s.levelMode, std::memory_order_relaxed);
            a.type.store(s.type, std::memory_order_relaxed);
            a.cutoff[0].store(s.cutoffHz[0], std::memory_order_relaxed);
            a.cutoff[1].store(s.cutoffHz[1], std::memory_order_relaxed);
            a.resonance.store(s.resonance, std::memory_order_relaxed);
            a.drive.store(s.drive, std::memory_order_relaxed);
            a.character.store(s.character, std::memory_order_relaxed);
            a.mix.store(s.mix, std::memory_order_relaxed);
        }
        viewOversample.store(oversample, std::memory_order_relaxed);
        bpmForOthers.store(bpm, std::memory_order_relaxed);

        int running = 0;
        for (int k = 0; k < kRackSlots; ++k) {
            const int lane = k / kNumFxSlots, pos = k % kNumFxSlots;
            std::uint8_t state = 0;
            float meter = 0.0f;
            if (pos < chainCount[lane]) {
                const FxInstance& x = pool[chain[lane][pos]];
                state = static_cast<std::uint8_t>(1u | (x.gTarget > 0.0f ? 2u : 0u) | (x.idle ? 4u : 0u) | (x.ran ? 8u : 0u));
                running += x.ran ? 1 : 0;
                meter = x.effect->meter();
            }
            fxState[k].store(state, std::memory_order_relaxed);
            fxMeterValue[k].store(meter, std::memory_order_relaxed);
        }
        fxRunning.store(running, std::memory_order_relaxed);
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
    auto& s = *impl_;
    const std::lock_guard<rt::Mutex> lock(s.rackMutex);
    s.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    s.updateSmoothing();
    s.prepareFilters();
    // Audio is stopped: every effect, running or waiting to be taken, gets memory for the new rate.
    for (FxInstance& x : s.pool)
        if (x.effect != nullptr) {
            x.effect->prepare(s.sampleRate);
            x.needsSet = x.snap = true;
        }
    if (RackPlan* plan = s.rackPending.load(std::memory_order_acquire))
        for (auto& effect : plan->fresh)
            if (effect != nullptr)
                effect->prepare(s.sampleRate);
    s.player.prepare(s.sampleRate);
    reset();
}

void Engine::reset() {
    auto& s = *impl_;
    s.allocator.reset();
    for (auto& v : s.voices) {
        v.active = false;
        v.done = false;
        for (auto& o : v.osc) {
            std::fill(o.main.lanes.phase, o.main.lanes.phase + kMaxLanes, 0u);
            o.fadeLeft = 0;
        }
        v.texture.fadeLeft = 0;
        for (auto& f : v.filter)
            f.running = false;
    }
    s.activeCount = 0;
    s.lastStarted = -1;
    s.activeForOthers.store(0, std::memory_order_relaxed);
    s.samplesToTick = kControlInterval;
    std::fill(std::begin(s.ctl), std::end(s.ctl), 0.0f);
    std::fill(std::begin(s.gsrc), std::end(s.gsrc), 0.0f);
    std::fill(std::begin(s.leadSrc), std::end(s.leadSrc), 0.0f);
    std::fill(std::begin(s.keyDown), std::end(s.keyDown), false);
    std::fill(std::begin(s.polyPressure), std::end(s.polyPressure), 0.0f);
    std::fill(std::begin(s.channelBend), std::end(s.channelBend), 0.0f);
    std::fill(std::begin(s.channelPressure), std::end(s.channelPressure), 0.0f);
    std::fill(std::begin(s.channelSlide), std::end(s.channelSlide), 0.0f);
    s.keysDown = s.keysHeldBefore = 0;
    s.lastKey = -1.0;
    s.alternate = -1.0f;
    s.followerLevel = 0.0f;
    s.rng.seed(s.seed);
    s.player.reset(s.seed);
    for (int k = 0; k < kNumLfos; ++k) {
        s.lfo[k].state = LfoState{};
        s.lfo[k].state.age = 1.0e12; // a shared LFO that no note has started yet runs without delay
        s.lfo[k].value = 0.0f;
        s.lfo[k].freePhase = 0.0;
    }
    for (int k = 0; k < kNumFlux; ++k) {
        fluxReset(s.flux[k].state, s.seed * 0x9E3779B97F4A7C15ull + 0x51ull + static_cast<std::uint64_t>(k));
        s.flux[k].value = 0.0f;
    }
    s.snapSmoothing = true;
    s.dirty = true;
    // The rack: every tail silenced, every fade finished.
    for (FxInstance& x : s.pool) {
        if (x.effect == nullptr)
            continue;
        x.effect->clear();
        x.g = x.gTarget;
        x.idle = false;
        x.silent = 0;
        x.needsSet = x.snap = true;
    }
    s.fxMix = s.fxMixTarget;
    for (auto& b : s.sendRamp)
        b.aim(b.target, true);
    s.dcX[0] = s.dcX[1] = s.dcY[0] = s.dcY[1] = 0.0f;
    s.outLimiter.reset();
    s.duckState = 0;
    s.pendingQuality = -1;
    s.qualityDuck = 1.0f;
    for (int c = 0; c < 2; ++c) {
        s.clipUp[c].reset();
        s.clipDown[c].reset();
    }
}

void Engine::setSeed(std::uint64_t seed) { impl_->seed = seed; }

void Engine::loadPatch(const Patch& patch) {
    const auto& reg = Registry::get();
    const bool keepRack = rackLocked();
    if (!keepRack)
        setRack(patch);
    for (int i = 0; i < impl_->store.size(); ++i) {
        int lane = 0, slot = 0, field = 0;
        if (keepRack && (reg.fxSlotOf(i, lane, slot, field) || reg.at(i).module == "fx"))
            continue; // a locked rack keeps its own values
        impl_->store.set(i, patch.get(i));
    }
    setRoutes(patch.routes);
    setPlayData(patch);
    for (int k = 0; k < kNumLfos; ++k)
        setLfoShape(k, patch.lfoShapes[static_cast<std::size_t>(k)]);
    for (int k = 0; k < kNumEnvs; ++k) {
        setEnvShape(k, patch.envShapes[static_cast<std::size_t>(k)]);
        for (int j = 0; j < 5; ++j)
            setEnvSync(k, j, divisionInBeats(patch.envSync[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)]));
    }
    for (int i = 0; i < kNumOscs; ++i) {
        for (int slot = 0; slot < 2; ++slot) {
            const Shape& shape = patch.warpCurves[static_cast<std::size_t>(i)][static_cast<std::size_t>(slot)];
            if (shape.empty()) {
                setWarpCurve(i, slot, {});
            } else {
                std::array<float, osc::kCurvePoints + 1> values{};
                compileShape(shape, values.data(), osc::kCurvePoints);
                setWarpCurve(i, slot, values);
            }
        }
    }
    if (!tuningLocked()) {
        if (patch.tuning.keys.size() == 128) {
            auto tuning = std::make_shared<Tuning>();
            tuning->name = patch.tuning.name;
            std::copy(patch.tuning.keys.begin(), patch.tuning.keys.end(), tuning->note.begin());
            setTuning(std::move(tuning));
        } else {
            setTuning(nullptr);
        }
    }
}

void Engine::setOffline(bool offline) { impl_->offline.store(offline, std::memory_order_relaxed); }

ParamStore& Engine::params() noexcept { return impl_->store; }
const ParamStore& Engine::params() const noexcept { return impl_->store; }
int Engine::activeVoices() const noexcept { return impl_->activeForOthers.load(std::memory_order_relaxed); }
double Engine::sampleRate() const noexcept { return impl_->sampleRate; }

double Engine::tailSeconds() const noexcept {
    const auto& s = *impl_;
    const auto& reg = Registry::get();
    const double bpm = std::max(s.bpmForOthers.load(std::memory_order_relaxed), 1.0);
    const double beats = s.envSyncBeats[0][4].load(std::memory_order_relaxed);
    const double release = beats > 0.0 ? beats * 60.0 / bpm : s.store.get(s.ids.env[0].release) / 1000.0;
    // Envelope variance can stretch a note's times to twice their length.
    double tail = release * (1.0 + s.store.get(s.ids.varEnv)) + kStealFadeSeconds + 0.01;
    // The DC filter lets a removed offset fade out after the voices stop (5 Hz: 32 ms per 1/e).
    if (s.store.get(s.ids.dcFilter) >= 0.5)
        tail += 0.3;
    // Effects ring on after the voices: reverbs for their decay, delays until the echoes fall 60 dB.
    if (s.store.get(s.ids.fxOff) >= 0.5)
        return tail;
    double fxTail = 0.0;
    const auto reverb = reg.effectIndex("reverb"), delay = reg.effectIndex("delay");
    for (int k = 0; k < kRackSlots; ++k) {
        const int lane = k / kNumFxSlots, pos = k % kNumFxSlots;
        const int type = s.rackType[k].load(std::memory_order_relaxed);
        if (type < 0 || s.store.get(reg.fxSlotOn(lane, pos)) < 0.5)
            continue;
        const auto value = [&](int param) { return s.store.get(reg.fxSlotParam(lane, pos, param)); };
        if (type == reverb) {
            if (value(9) >= 0.5) // frozen
                return std::numeric_limits<double>::infinity();
            fxTail += value(2) / 1000.0 + value(3) / 1000.0;
        } else if (type == delay) {
            // The longer side's time (synced: its note length at the tempo, with the fine offset);
            // echoes fall by the feedback at each repeat until they are 60 dB down.
            const double feedback = std::clamp(value(9), 0.0, 1.0);
            double time = std::max(value(2), value(3)) / 1000.0;
            if (value(1) >= 0.5)
                time = std::max(fx::divisionBeats(static_cast<int>(value(4))) * std::pow(1.5, std::clamp(value(6), -1.0, 1.0)),
                                fx::divisionBeats(static_cast<int>(value(5))) * std::pow(1.5, std::clamp(value(7), -1.0, 1.0))) *
                       60.0 / bpm;
            fxTail += feedback >= 0.999 ? 60.0 : time * (1.0 + std::log(0.001) / std::log(std::max(feedback, 0.001)));
        } else {
            fxTail += 0.05;
        }
    }
    return tail + std::min(fxTail, 120.0);
}

void Engine::setTable(int osc, std::shared_ptr<const TableData> table) {
    if (osc < 0 || osc >= kOscCount)
        return;
    impl_->tables[osc].set(table != nullptr ? std::move(table) : TableData::builtin());
}

std::shared_ptr<const TableData> Engine::table(int osc) const {
    return osc >= 0 && osc < kOscCount ? impl_->tables[osc].latest() : nullptr;
}

void Engine::setTexture(std::shared_ptr<const SampleData> sample) {
    impl_->sampleSlot.set(sample != nullptr && sample->frames >= 2 ? std::move(sample) : SampleData::builtin());
}

std::shared_ptr<const SampleData> Engine::texture() const { return impl_->sampleSlot.latest(); }

void Engine::setWarpCurve(int osc, int slot, std::span<const float> values) {
    if (osc < 0 || osc >= kOscCount || slot < 0 || slot > 1)
        return;
    auto curve = std::make_shared<WarpCurve>();
    osc::fillWarpCurve(values, curve->values.data());
    impl_->curves[osc][slot].set(std::move(curve));
}

int Engine::sourceIndex(std::string_view id) {
    const auto sources = Registry::get().modSources();
    for (std::size_t i = 0; i < sources.size(); ++i)
        if (sources[i].id == id)
            return static_cast<int>(i);
    if (id.size() >= 3 && id.substr(0, 2) == "cc" && Registry::get().isModSource(id)) {
        int number = 0;
        std::from_chars(id.data() + 2, id.data() + id.size(), number);
        return kSrcCc + number;
    }
    return -1;
}

void Engine::setRoutes(std::span<const ModRoute> list) {
    const auto& reg = Registry::get();
    auto table = std::make_shared<RouteTable>();
    bool solo = false;
    for (const ModRoute& r : list)
        solo = solo || (r.enabled && r.solo);
    const std::size_t count = std::min(list.size(), static_cast<std::size_t>(kNumModRoutes));
    for (std::size_t i = 0; i < count; ++i) {
        const ModRoute& r = list[i];
        RouteDef& d = table->route[i];
        d.source = static_cast<std::int16_t>(sourceIndex(r.source));
        d.via = r.via.empty() ? std::int16_t{-1} : static_cast<std::int16_t>(sourceIndex(r.via));
        const ParamDef* dest = reg.find(r.dest);
        d.dest = dest != nullptr && dest->modulatable ? static_cast<std::int16_t>(dest->index) : std::int16_t{-1};
        d.viaInvert = r.viaInvert;
        d.polarity = r.polarity;
        d.curve = r.curve;
        if (r.curve == ModCurve::Drawn && !r.curveShape.empty()) {
            table->curves.emplace_back();
            compileShape(r.curveShape, table->curves.back().data(), kModCurveSize);
            for (float& y : table->curves.back())
                y = std::clamp(y, 0.0f, 1.0f);
            d.drawn = static_cast<std::int16_t>(table->curves.size() - 1);
        }
        d.active = r.enabled && !r.mute && (!solo || r.solo) && d.source >= 0 && d.dest >= 0;
    }
    impl_->routes.set(std::move(table));
}

void Engine::setLfoShape(int lfo, const Shape& shape) {
    if (lfo < 0 || lfo >= kNumLfos)
        return;
    auto table = std::make_shared<LfoTable>();
    buildLfoTable(shape, *table);
    impl_->lfoTables[lfo].set(std::move(table));
}

void Engine::setEnvShape(int env, const EnvShape& shape) {
    if (env < 0 || env >= kNumEnvs)
        return;
    auto data = std::make_shared<EnvShapeData>();
    data->count = static_cast<int>(std::min(shape.points.size(), static_cast<std::size_t>(kMaxEnvPoints)));
    for (int n = 0; n < data->count; ++n) {
        const EnvPoint& point = shape.points[static_cast<std::size_t>(n)];
        data->ms[n] = std::max(point.timeMs, 0.0);
        data->beats[n] = point.sync.empty() ? 0.0 : divisionInBeats(point.sync);
        data->level[n] = std::clamp(point.level, 0.0, 1.0);
        data->curve[n] = std::clamp(point.curve, -1.0, 1.0);
    }
    const auto inRange = [&](int index) { return index >= 0 && index < data->count ? index : -1; };
    data->sustainPoint = inRange(shape.sustainPoint);
    data->loopStart = inRange(shape.loopStart);
    data->loopEnd = inRange(shape.loopEnd);
    impl_->envShapes[env].set(std::move(data));
}

void Engine::setEnvSync(int env, int segment, double beats) {
    if (env < 0 || env >= kNumEnvs || segment < 0 || segment >= 5)
        return;
    impl_->envSyncBeats[env][segment].store(std::max(beats, 0.0), std::memory_order_relaxed);
    impl_->envSyncDirty.store(true, std::memory_order_release);
}

void Engine::setTuning(std::shared_ptr<const Tuning> tuning) {
    impl_->tuningSlot.set(tuning != nullptr ? std::move(tuning) : std::make_shared<const Tuning>());
}

std::shared_ptr<const Tuning> Engine::tuning() const { return impl_->tuningSlot.latest(); }
void Engine::setTuningLocked(bool locked) { impl_->tuningLocked.store(locked, std::memory_order_relaxed); }
bool Engine::tuningLocked() const { return impl_->tuningLocked.load(std::memory_order_relaxed); }

void Engine::collectGarbage() {
    for (int i = 0; i < kOscCount; ++i) {
        impl_->tables[i].collect();
        impl_->curves[i][0].collect();
        impl_->curves[i][1].collect();
    }
    impl_->sampleSlot.collect();
    impl_->routes.collect();
    for (auto& table : impl_->lfoTables)
        table.collect();
    for (auto& shape : impl_->envShapes)
        shape.collect();
    impl_->tuningSlot.collect();
    impl_->fxBin.collect();
    impl_->planBin.collect();
    impl_->playData.collect();
}

void Engine::setPlayData(const Patch& patch) { impl_->playData.set(play::PlayData::fromPatch(patch)); }

// --- Effects rack -------------------------------------------------------------------------------

void Engine::setRack(const Patch& patch, const FxMoves* moves) {
    auto& s = *impl_;
    const auto& reg = Registry::get();
    const std::lock_guard<rt::Mutex> lock(s.rackMutex);
    s.fxBin.collect();
    s.planBin.collect();
    auto plan = std::make_unique<RackPlan>();
    // A plan the audio thread has not taken yet comes back: its new effects can be offered again.
    std::unique_ptr<RackPlan> untaken(s.rackPending.exchange(nullptr, std::memory_order_acq_rel));
    bool used[kRackSlots] = {};
    std::uint32_t uid[kRackSlots] = {};
    int type[kRackSlots];
    for (int k = 0; k < kRackSlots; ++k)
        type[k] = -1;
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        for (int pos = 0; pos < patch.fxSlotCount(lane); ++pos) {
            const int k = lane * kNumFxSlots + pos;
            const int t = patch.fxSlotType(lane, pos);
            const int oldType = s.rackType[k].load(std::memory_order_relaxed);
            const int from = moves != nullptr ? moves->from[static_cast<std::size_t>(k)] : (oldType == t ? k : -1);
            if (from >= 0 && from < kRackSlots && !used[from] && s.rackUid[from] != 0 &&
                s.rackType[from].load(std::memory_order_relaxed) == t) {
                used[from] = true;
                uid[k] = s.rackUid[from];
                if (untaken != nullptr)
                    for (int j = 0; j < kRackSlots; ++j)
                        if (untaken->uid[j] == uid[k] && untaken->fresh[j] != nullptr)
                            plan->fresh[k] = std::move(untaken->fresh[j]);
            } else {
                uid[k] = s.nextUid++;
                plan->fresh[k] = fx::makeEffect(t, s.sampleRate);
            }
            type[k] = t;
            plan->uid[k] = uid[k];
            plan->type[k] = t;
            plan->count[lane] = pos + 1;
            for (int c = 0; c < 2; ++c) {
                const Shape& shape = patch.fxCurves[static_cast<std::size_t>(lane)][static_cast<std::size_t>(pos)][static_cast<std::size_t>(c)];
                if (shape.empty())
                    continue;
                compileShape(shape, plan->curves[k][c], fx::kCurveSize);
                for (float& y : plan->curves[k][c])
                    y = std::clamp(y, 0.0f, 1.0f);
                plan->hasCurve[k][c] = true;
            }
        }
    }
    for (int k = 0; k < kRackSlots; ++k) {
        const int base = reg.fxSlotOn(k / kNumFxSlots, k % kNumFxSlots);
        for (int f = 0; f < kSlotFields; ++f)
            plan->values[k][f] = patch.get(base + f);
        s.rackUid[k] = uid[k];
        s.rackType[k].store(type[k], std::memory_order_relaxed);
    }
    s.rackPending.store(plan.release(), std::memory_order_release);
    // The values follow the plan into the parameter store (see EngineRack.inl for the order).
    for (int k = 0; k < kRackSlots; ++k) {
        const int base = reg.fxSlotOn(k / kNumFxSlots, k % kNumFxSlots);
        for (int f = 0; f < kSlotFields; ++f)
            s.store.set(base + f, patch.get(base + f));
    }
}

void Engine::setRackLocked(bool locked) { impl_->rackLockedFlag.store(locked, std::memory_order_relaxed); }
bool Engine::rackLocked() const { return impl_->rackLockedFlag.load(std::memory_order_relaxed); }

FxSlotStatus Engine::fxSlotStatus(int lane, int slot) const noexcept {
    FxSlotStatus out;
    if (lane < 0 || lane >= kNumFxLanes || slot < 0 || slot >= kNumFxSlots)
        return out;
    const int k = lane * kNumFxSlots + slot;
    const std::uint8_t state = impl_->fxState[k].load(std::memory_order_relaxed);
    out.present = (state & 1u) != 0;
    out.on = (state & 2u) != 0;
    out.idle = (state & 4u) != 0;
    out.running = (state & 8u) != 0;
    out.meter = impl_->fxMeterValue[k].load(std::memory_order_relaxed);
    return out;
}

int Engine::fxSlotsRunning() const noexcept { return impl_->fxRunning.load(std::memory_order_relaxed); }

int Engine::arpStep() const noexcept { return impl_->arpStepShown.load(std::memory_order_relaxed); }

bool Engine::fxResponse(int lane, int slot, std::span<const double> hz, std::span<float> magnitudeDb) const {
    if (lane < 0 || lane >= kNumFxLanes || slot < 0 || slot >= kNumFxSlots)
        return false;
    const auto& reg = Registry::get();
    const int type = impl_->rackType[lane * kNumFxSlots + slot].load(std::memory_order_relaxed);
    if (type < 0)
        return false;
    double values[kNumFxSlotParams] = {};
    for (int k = 0; k < kNumFxSlotParams; ++k)
        values[k] = impl_->store.get(reg.fxSlotParam(lane, slot, k));
    for (std::size_t i = 0; i < hz.size() && i < magnitudeDb.size(); ++i) {
        std::complex<double> h;
        if (!fx::effectResponse(type, values, impl_->sampleRate, hz[i], h))
            return false;
        magnitudeDb[i] = static_cast<float>(gainToDb(std::abs(h)));
    }
    return true;
}

// --- Profiling ------------------------------------------------------------------------------------

void Engine::setProfiling(bool on) {
    impl_->profiling = on;
    std::fill(std::begin(impl_->prof), std::end(impl_->prof), 0.0);
}

std::vector<Engine::ProfileEntry> Engine::profile() const {
    static const char* const names[] = {"voice sources", "voice filters and mix", "voice control ticks", "shared control ticks",
                                        "play features", "output stage"};
    std::vector<ProfileEntry> out;
    for (int i = 0; i < Impl::kProfFx; ++i)
        out.push_back({names[i], impl_->prof[i]});
    const auto types = Registry::get().effectTypes();
    for (std::size_t t = 0; t < types.size() && Impl::kProfFx + static_cast<int>(t) < Impl::kProfCount; ++t)
        out.push_back({"effect: " + types[t].id, impl_->prof[Impl::kProfFx + static_cast<int>(t)]});
    return out;
}

float Engine::sourceValue(int source) const noexcept {
    return source >= 0 && source < kSrcCount ? impl_->liveSrc[source].load(std::memory_order_relaxed) : 0.0f;
}

double Engine::liveValue(int paramIndex) const noexcept {
    if (paramIndex < 0 || paramIndex >= impl_->store.size())
        return 0.0;
    const float shown = impl_->live[static_cast<std::size_t>(paramIndex)].load(std::memory_order_relaxed);
    return shown == kNotModulated ? impl_->store.get(paramIndex) : static_cast<double>(shown);
}

FilterView Engine::filterView(int slot) const noexcept {
    FilterView out;
    if (slot < 0 || slot >= kNumFilters)
        return out;
    const auto& a = impl_->view[slot];
    out.on = a.on.load(std::memory_order_relaxed);
    out.settings.type = a.type.load(std::memory_order_relaxed);
    out.settings.cutoffHz[0] = a.cutoff[0].load(std::memory_order_relaxed);
    out.settings.cutoffHz[1] = a.cutoff[1].load(std::memory_order_relaxed);
    out.settings.resonance = a.resonance.load(std::memory_order_relaxed);
    out.settings.drive = a.drive.load(std::memory_order_relaxed);
    out.settings.character = a.character.load(std::memory_order_relaxed);
    out.settings.mix = a.mix.load(std::memory_order_relaxed);
    out.settings.levelMode = a.levelMode.load(std::memory_order_relaxed);
    out.oversample = impl_->viewOversample.load(std::memory_order_relaxed);
    return out;
}

void Engine::filterResponse(int slot, std::span<const double> hz, std::span<float> magnitudeDb, std::span<float> phase) const {
    const FilterView view = filterView(slot);
    for (std::size_t i = 0; i < hz.size(); ++i) {
        // A filter that is off passes the signal unchanged.
        const std::complex<double> h = view.on ? flt::response(view.settings, 0, impl_->sampleRate, hz[i], view.oversample)
                                               : std::complex<double>(1.0, 0.0);
        if (i < magnitudeDb.size())
            magnitudeDb[i] = static_cast<float>(gainToDb(std::abs(h)));
        if (i < phase.size())
            phase[i] = static_cast<float>(std::arg(h));
    }
}

void Engine::process(const ProcessArgs& args) noexcept {
    const rt::AudioThreadScope audioThread;
    const rt::ScopedNoDenormals noDenormals;
    auto& s = *impl_;
    if (args.frames <= 0 || args.left == nullptr || args.right == nullptr)
        return;

    s.rackBlockStart(); // before the drain: a new layout's values must meet its effects
    s.store.drain([&s](int index, double value) { s.setParam(index, value); });
    const bool offline = s.offline.load(std::memory_order_relaxed);
    if (offline != s.offlineSeen) {
        s.offlineSeen = offline;
        s.dirty = true;
    }
    const double bpm = std::clamp(args.transport.bpm > 0.0 ? args.transport.bpm : 120.0, 1.0, 999.0);
    if (bpm != s.bpm) {
        s.bpm = bpm;
        s.derivedDirty = true; // tempo-linked rates and times
    }
    s.playing = args.transport.playing;
    s.blockPpq = s.ppqNow = args.transport.ppqPosition;
    s.barStartPpq = args.transport.barStartPpq;
    s.beatsPerBar = args.transport.timeSigNumerator > 0 && args.transport.timeSigDenominator > 0
                        ? args.transport.timeSigNumerator * 4.0 / args.transport.timeSigDenominator
                        : 4.0;
    const double beatsPerSample = s.playing ? s.bpm / (60.0 * s.sampleRate) : 0.0;

    s.takeStructure();
    if (s.snapSmoothing)
        s.snapParams();
    if (s.dirty) {
        s.refresh();
        s.dirty = false;
    }
    if (s.snapSmoothing) {
        // The first block after a reset: every shared value is in place before a note can start.
        s.globalTick(s.samplesToTick, true);
        s.master = s.masterAim = s.masterTarget;
        s.masterStep = 0.0;
        s.snapSmoothing = false;
    }
    s.takeHandoffs();

    // The play features turn the incoming events into the ones the voices play.
    int generated = 0;
    const bool sendOut = s.flag(s.ids.midiOut) && args.midiOut != nullptr;
    const auto playStart = s.profNow();
    Transport transport = args.transport;
    transport.bpm = s.bpm; // a host without a tempo plays at 120
    const int playCount = s.player.process(s.playSettings(), *s.playData.current(), transport, args.frames, args.midi,
                                           s.playEvents, sendOut ? args.midiOut : nullptr,
                                           sendOut ? args.midiOutCapacity : 0, generated);
    s.arpStepShown.store(s.player.playingStep(), std::memory_order_relaxed);
    if (args.midiOutCount != nullptr)
        *args.midiOutCount = sendOut ? generated : 0;
    const std::span<const MidiEvent> midi(s.playEvents, static_cast<std::size_t>(playCount));
    s.profAdd(Impl::kProfPlay, playStart);
    std::size_t next = 0;
    int pos = 0;
    while (pos < args.frames) {
        while (next < midi.size() && midi[next].offset <= pos)
            s.handle(midi[next++]);
        int end = args.frames;
        if (next < midi.size())
            end = std::min(end, midi[next].offset);
        const int frames = std::min(end - pos, s.samplesToTick);
        s.sidechain[0] = args.sidechainLeft != nullptr ? args.sidechainLeft + pos : nullptr;
        s.sidechain[1] = args.sidechainRight != nullptr ? args.sidechainRight + pos : nullptr;
        s.render(args.left + pos, args.right + pos, frames);
        pos += frames;
        s.samplesToTick -= frames;
        s.ppqNow = s.blockPpq + pos * beatsPerSample;
        if (s.samplesToTick == 0)
            s.tick();
    }
    while (next < midi.size()) // offsets at or past the end of the block
        s.handle(midi[next++]);
    s.publish();
}

} // namespace zed
