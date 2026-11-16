// State of the engine: what every voice shares and what each voice keeps. Included by Engine.cpp
// inside its anonymous namespace, after EngineHelpers.inl.

// --- Modulation sources ------------------------------------------------------------------------
// Positions in Registry::modSources() (checked when the engine is built), then the controllers.
constexpr int kSrcEnv = 0, kSrcLfo = 4, kSrcFlux = 12, kSrcMacro = 14, kSrcVelocity = 22, kSrcReleaseVelocity = 23,
              kSrcNote = 24, kSrcPitchBend = 25, kSrcModWheel = 26, kSrcAftertouch = 27, kSrcPolyAftertouch = 28,
              kSrcMpePressure = 29, kSrcMpeSlide = 30, kSrcExpression = 31, kSrcBreath = 32, kSrcSustain = 33,
              kSrcRandom1 = 34, kSrcRandom2 = 35, kSrcAlternate = 36, kSrcVoiceIndex = 37, kSrcUnisonIndex = 38,
              kSrcConstant = 39, kSrcAudio = 40, kSrcFollower = 45, kSrcBarPhase = 46, kNamedSources = 47,
              kSrcCc = kNamedSources, kSrcCount = kNamedSources + 128;

// Groups of parameters that are derived together; a voice keeps its own copy of a group only
// while a per-voice route reaches into it.
constexpr int kModOsc = 0, kModSub = 3, kModTexture = 4, kModFilter = 5, kModEnv = 7, kModLfo = 11, kModVoice = 19,
              kModOther = 20;

constexpr int kMaxAudioRoutes = 4; // audio-rate routes onto one filter's cutoff
constexpr int kMaxUniTerms = 16;   // unison-index routes onto one oscillator
constexpr double kDriftSemis = 0.25;       // Drift at 100 %: a wander of up to a quarter tone
constexpr double kVarianceCutoffOct = 1.0; // Cutoff variance at 100 %: up to an octave either way
constexpr double kVarianceLevelDb = 6.0;
constexpr float kNoiseFloorGain = 0.01f;   // Noise floor at 100 %: -40 dB
constexpr float kStereoOffsetOct = 1.0f;   // Stereo offset at 100 %: each side moves an octave
constexpr double kMaxLfoHz = 100.0, kMaxAudioLfoHz = 2000.0;

// --- Ramps -------------------------------------------------------------------------------------

struct Ramp1 { // one value moving in a straight line to where it should be at the next control tick
    float now = 0.0f, step = 0.0f, target = 0.0f;
    void aim(float value, int samples, bool snap) {
        target = value;
        if (snap)
            now = value;
        step = (value - now) / static_cast<float>(samples);
    }
    void settle() {
        now = target;
        step = 0.0f;
    }
};

struct SourceGain { // a source's level and pan as left and right gains, ramped per sample
    float now[2] = {0.0f, 0.0f}, step[2] = {0.0f, 0.0f}, target[2] = {0.0f, 0.0f};
    void aim(float left, float right, int samples, bool snap) {
        target[0] = left;
        target[1] = right;
        for (int c = 0; c < 2; ++c) {
            if (snap)
                now[c] = target[c];
            step[c] = (target[c] - now[c]) / static_cast<float>(samples);
        }
    }
    void settle() {
        for (int c = 0; c < 2; ++c) {
            now[c] = target[c];
            step[c] = 0.0f;
        }
    }
    bool silent() const { return now[0] == 0.0f && now[1] == 0.0f && target[0] == 0.0f && target[1] == 0.0f; }
};

struct SendRamp { // a source's shares into filter 1, filter 2 and the direct bus
    float now[3] = {1.0f, 0.0f, 0.0f}, step[3] = {0.0f, 0.0f, 0.0f}, target[3] = {1.0f, 0.0f, 0.0f};
    void aim(const float value[3], int samples, bool snap) {
        for (int b = 0; b < 3; ++b) {
            target[b] = value[b];
            if (snap)
                now[b] = value[b];
            step[b] = (value[b] - now[b]) / static_cast<float>(samples);
        }
    }
    void settle() {
        for (int b = 0; b < 3; ++b) {
            now[b] = target[b];
            step[b] = 0.0f;
        }
    }
};

// --- Oscillators -------------------------------------------------------------------------------

// One oscillator's unison lanes plus the values they move toward at the next control tick.
struct LaneSet {
    osc::Lanes lanes;
    alignas(32) float incT[kMaxLanes], fposT[kMaxLanes], dAT[kMaxLanes], dBT[kMaxLanes];
    alignas(32) float gLT[kMaxLanes], gRT[kMaxLanes], gMT[kMaxLanes];
    int level[kMaxLanes] = {};     // band-limited level each lane reads
    int levelNext[kMaxLanes] = {}; // level it is fading to (equal to level when not fading)
    bool levelFade = false;
};

void setLaneLevel(LaneSet& s, int lane, const TableData& table, int level, bool second) {
    const TableData::Level& l = table.level(level);
    if (second) {
        s.lanes.tabOff2[lane] = static_cast<float>(l.offset);
        s.lanes.tabStride2[lane] = static_cast<float>(l.stride);
        s.lanes.tabSize2[lane] = static_cast<float>(l.size);
    } else {
        s.lanes.tabOff[lane] = static_cast<float>(l.offset);
        s.lanes.tabStride[lane] = static_cast<float>(l.stride);
        s.lanes.tabSize[lane] = static_cast<float>(l.size);
    }
}

// The controls that decide the per-lane values.
struct LaneInputs {
    float position = 0.0f, amount[2] = {0.0f, 0.0f}, spreadSemis = 0.0f, blend = 0.0f, width = 0.0f, posSpread = 0.0f,
          warpSpread = 0.0f;
    bool operator==(const LaneInputs&) const = default;
};

// An oscillator's continuous controls after smoothing and modulation.
struct OscTargets {
    double tuneSemis = 0.0, levelGain = 0.0, pan = 0.0;
    LaneInputs in;
    float shape = 0.0f, pulseWidth = 0.5f;
    float send[3] = {1.0f, 0.0f, 0.0f};
};

// Per-lane values (plan 3.4): shared by every voice unless a per-voice route moves the oscillator.
struct LaneTable {
    float x[kMaxLanes] = {};        // lane position, -1..1, centre first
    float pos[kMaxLanes] = {};      // table position 0..1
    float d[2][kMaxLanes] = {};     // derived warp values
    float slope[kMaxLanes] = {};    // how much the warps steepen the phase
    float gL[kMaxLanes] = {}, gR[kMaxLanes] = {}, gM[kMaxLanes] = {};
    float ratio[kMaxLanes] = {};    // detune and stack as a frequency ratio
    float stackSemis[kMaxLanes] = {};
    float extraSemis[kMaxLanes] = {}; // pitch offsets from unison-index routes
    float spreadSemis = 0.0f;
    std::uint32_t epoch = 0;
};

// A route from the unison index onto one of an oscillator's per-lane values.
enum class UniDest : std::uint8_t { Position, WarpA, WarpB, Pan, Semis };
struct UniTerm {
    UniDest dest = UniDest::Position;
    float gain = 0.0f; // amount times via, in the destination's units
    Polarity polarity = Polarity::Bi;
    ModCurve curve = ModCurve::Linear;
    const float* drawn = nullptr;
};

struct OscShared {
    // Structure, straight from the parameters.
    bool on = false, classic = false, keytrack = true, continuePhase = false;
    bool turnedOn = false; // switched on by the last parameter update: voices need its lanes set up
    int count = 1;
    UnisonLaw law = UnisonLaw::Linear;
    UnisonStack stack = UnisonStack::Off;
    UnisonPhase unisonPhase = UnisonPhase::Random;
    WarpMode mode[2] = {WarpMode::Off, WarpMode::Off}; // effective: off when the engine cannot do it
    int modSource[2] = {0, 0};
    float startPhase = 0.0f, phaseRandom = 0.0f;
    std::uint32_t structure = 1; // changes whenever the lane layout does
    // Continuous controls and the lane values that follow from them.
    OscTargets t;
    LaneTable lanes;
    LaneInputs lanesFor;
    bool lanesDirty = true;
    float consistentPhase[kMaxLanes] = {};
    // Unison-index routes that reach this oscillator.
    int uniCount = 0;
    std::uint8_t uniRoute[kMaxUniTerms] = {};
    UniDest uniDest[kMaxUniTerms] = {};
    float uniScale[kMaxUniTerms] = {}; // the destination's range: what an amount of 100 % adds
    // Rendering.
    int oversample = 1;
    int padded = 1; // lanes the chosen kernel touches
    osc::RenderFn render = osc::renderScalar;
    const TableData* table = nullptr;
    const TableData* oldTable = nullptr; // during a swap
    int fadeLeft = 0;
    const float* curve[2] = {nullptr, nullptr};
};

void computeLaneTable(const OscShared& o, const LaneInputs& in, const UniTerm* terms, int termCount, LaneTable& t) {
    const int count = o.count;
    layoutLanes(count, t.x);
    float uPos[kMaxLanes] = {}, uAmt[2][kMaxLanes] = {}, uPan[kMaxLanes] = {};
    std::fill(t.extraSemis, t.extraSemis + kMaxLanes, 0.0f);
    for (int k = 0; k < termCount; ++k) {
        const UniTerm& u = terms[k];
        for (int l = 0; l < count; ++l) {
            const float v = u.gain * modShape(t.x[l], true, u.polarity, u.curve, u.drawn);
            switch (u.dest) {
            case UniDest::Position: uPos[l] += v; break;
            case UniDest::WarpA: uAmt[0][l] += v; break;
            case UniDest::WarpB: uAmt[1][l] += v; break;
            case UniDest::Pan: uPan[l] += v; break;
            case UniDest::Semis: t.extraSemis[l] += v; break;
            }
        }
    }

    // Centre against side voices, then scaled so the summed power does not depend on the count.
    const float angle = std::clamp(in.blend, 0.0f, 1.0f) * static_cast<float>(kPi * 0.5);
    const float centre = count <= 2 ? 1.0f : std::cos(angle), side = std::sin(angle);
    float weight[kMaxLanes], power = 0.0f;
    for (int l = 0; l < count; ++l) {
        weight[l] = isCentreLane(l, count) ? centre : side;
        power += weight[l] * weight[l];
    }
    const float norm = power > 1.0e-9f ? 1.0f / std::sqrt(power) : 0.0f;
    for (int l = 0; l < kMaxLanes; ++l) {
        if (l >= count) {
            t.gL[l] = t.gR[l] = t.gM[l] = 0.0f;
            continue;
        }
        const float gain = power > 1.0e-9f ? weight[l] * norm : 1.0f / std::sqrt(static_cast<float>(count));
        // Neighbouring detunes go to opposite sides, so neither side is flat or sharp as a whole.
        const int rank = count % 2 == 1 ? (l + 1) / 2 : l / 2 + 1;
        const float sideSign = (t.x[l] < 0.0f ? -1.0f : 1.0f) * (rank % 2 == 1 ? -1.0f : 1.0f);
        lanePan(in.width * std::abs(t.x[l]) * sideSign + uPan[l], gain, t.gL[l], t.gR[l]);
        t.gM[l] = gain;
    }
    for (int l = 0; l < kMaxLanes; ++l) {
        const int src = l < count ? l : 0;
        t.pos[l] = std::clamp(in.position + in.posSpread * t.x[src] + uPos[src], 0.0f, 1.0f);
        float slope = 1.0f;
        for (int s = 0; s < 2; ++s) {
            const float amount = std::clamp(in.amount[s] + in.warpSpread * t.x[src] + uAmt[s][src], 0.0f, 1.0f);
            t.d[s][l] = osc::warpDerive(o.mode[s], amount);
            slope *= osc::warpSlope(o.mode[s], t.d[s][l]);
        }
        t.slope[l] = std::min(slope, 64.0f);
        t.stackSemis[l] = stackOffset(o.stack, src, count);
        if (l >= count)
            t.extraSemis[l] = t.extraSemis[src];
        const float detune = detuneLaw(o.law, t.x[src]) * in.spreadSemis * 0.5f + t.extraSemis[l];
        t.ratio[l] = std::exp2((detune + t.stackSemis[l]) / 12.0f);
    }
    t.spreadSemis = in.spreadSemis;
    ++t.epoch;
}

struct OscVoice {
    LaneSet main;
    LaneSet fade;     // keeps playing the previous table while a new one fades in
    int fadeLeft = 0; // samples of that crossfade still to go
    float ratio[kMaxLanes] = {}; // pitch ratio per lane when the detune is random per note
    float randomDetune[kMaxLanes] = {};
    const LaneTable* ratioTable = nullptr;
    std::uint32_t ratioEpoch = 0;
    SourceGain gain;
    SendRamp send;
    Ramp1 shape, pulseWidth; // Classic engine, as this voice hears them
    dsp::DecimatorState decimatorL, decimatorR;
    // This voice's own controls and lane values, used while a per-voice route moves the oscillator.
    OscTargets t;
    LaneTable lanes;
    LaneInputs lanesFor;
    std::uint32_t lanesStructure = 0;
};

// --- Sub and Texture ---------------------------------------------------------------------------

struct SubTargets {
    double tuneSemis = 0.0, levelGain = 0.0, pan = 0.0;
    float balance = 0.0f;
};

struct SubShared {
    bool on = false, direct = false, turnedOn = false;
    SubShape shape = SubShape::Sine;
    SubTargets t;
    int oversample = 1;
};

struct SubVoice {
    double phase = 0.0, inc = 0.0, incStep = 0.0, incTarget = 0.0; // inc in cycles per rendered sample
    SourceGain gain;
    SendRamp send;
    dsp::DecimatorState decimator;
};

struct TextureTargets {
    double tuneSemis = 0.0, levelGain = 0.0, pan = 0.0;
    float balance = 0.0f, start = 0.0f;
};

struct TextureShared {
    bool on = false, direct = false, loop = true, keytrack = false, turnedOn = false;
    float randomStart = 0.0f;
    TextureTargets t;
    int oversample = 1;
    const SampleData* sample = nullptr;
    const SampleData* oldSample = nullptr;
    int fadeLeft = 0;
};

struct TexturePlay {
    double position = 0.0; // in frames of the sample
    bool finished = false;
};

struct TextureVoice {
    TexturePlay main, fade;
    int fadeLeft = 0;
    double rate = 0.0, rateStep = 0.0, rateTarget = 0.0; // frames per rendered sample
    SourceGain gain;
    SendRamp send;
    dsp::DecimatorState decimatorL, decimatorR;
};

// --- Filters -----------------------------------------------------------------------------------

struct FilterTargets {
    float cutoff = 2000.0f, resonance = 0.0f, drive = 0.0f, character = 0.5f, stereoOffset = 0.0f, mix = 1.0f;
};

struct FilterShared {
    bool on = false, turnedOn = false, levelMode = false;
    int type = 1;
    float keytrack = 0.0f;
    FilterTargets t;
    // Routes that move the cutoff every sample.
    int audioCount = 0;
    std::uint8_t audioRoute[kMaxAudioRoutes] = {};
};

struct FilterVoice {
    flt::VoiceFilter filter;
    FilterTargets t;
    Ramp1 depth[kMaxAudioRoutes]; // octaves of cutoff per unit of each audio-rate source
    flt::Settings aimed;          // what the filter was last pointed at
    int aimedOversample = 0;
    bool running = false;
};

// --- Modulators --------------------------------------------------------------------------------

// A multi-segment envelope as it is handed to the audio thread; times become samples there,
// because tempo-linked ones follow the host.
struct EnvShapeData {
    int count = 0;
    double ms[kMaxEnvPoints] = {}, beats[kMaxEnvPoints] = {}, level[kMaxEnvPoints] = {}, curve[kMaxEnvPoints] = {};
    int sustainPoint = -1, loopStart = -1, loopEnd = -1;
};

struct EnvShared {
    EnvelopeTimes t;
    bool multi = false;
    bool used = false; // env 2..4: some route reads it
    MultiEnvelopeDef def;
    double syncBeats[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
};

struct LfoShared {
    LfoMode mode = LfoMode::Retrigger;
    bool sync = false, anchor = false, global = false, audioRate = false;
    float keytrack = 0.0f;
    double beats = 1.0;    // length of one cycle when synced
    LfoParams p;           // as every voice without its own rate hears it
    const LfoTable* table = nullptr;
    bool used = false;      // some route reads it
    bool audioUsed = false; // a filter cutoff reads it every sample
    // The shared instance (scope "global"), and where a free-running LFO is at the next tick.
    LfoState state;
    float value = 0.0f;
    float buf[kInterval] = {};
    double freePhase = 0.0;
};

struct FluxShared {
    FluxType type = FluxType::Orbit;
    bool global = true, used = false;
    double cyclesPerSample = 0.0;
    FluxState state;
    float value = 0.0f;
};

// --- The matrix --------------------------------------------------------------------------------

// What setRoutes() hands to the audio thread: ids already resolved.
struct RouteDef {
    std::int16_t source = -1, via = -1, dest = -1;
    bool viaInvert = false, active = false;
    Polarity polarity = Polarity::Bi;
    ModCurve curve = ModCurve::Linear;
    std::int16_t drawn = -1; // index into RouteTable::curves
};
struct RouteTable {
    RouteDef route[kNumModRoutes];
    std::vector<std::array<float, kModCurveSize + 1>> curves;
};

// A route the audio thread evaluates every control tick.
struct ActiveRoute {
    std::int16_t source = 0, via = -1, slot = 0, amount = 0;
    bool viaInvert = false;
    Polarity polarity = Polarity::Bi;
    ModCurve curve = ModCurve::Linear;
    const float* drawn = nullptr;
};

// --- A voice -----------------------------------------------------------------------------------

struct Voice {
    bool active = false;
    bool done = false; // envelope finished; freed at the next tick, once the last ramp has reached zero
    int note = kFixedPitchNote, channel = 0;
    double velocity = 0.0;
    // Pitch: the key position glides; `pitch` is what the tuning makes of it.
    double keyFrom = kFixedPitchNote, keyTo = kFixedPitchNote, keyNow = kFixedPitchNote;
    double glideT = 1.0, glideSamples = 0.0;
    double pitch = kFixedPitchNote;
    // Amp.
    double envTarget = 0.0, envStep = 0.0; // envelope 1 at the next tick and its per-sample slope
    float amp[2] = {0.0f, 0.0f}, ampStep[2] = {0.0f, 0.0f};
    // Modulators and the values they have at the next tick.
    Envelope env[kNumEnvs];
    MultiEnvelope menv[kNumEnvs];
    EnvelopeTimes envT[kNumEnvs]; // this voice's own times, while a per-voice route moves them
    float envValue[kNumEnvs] = {};
    double envRate = 1.0; // envelope variance: how fast this note's envelopes run
    LfoState lfo[kNumLfos];
    LfoParams lfoP[kNumLfos];
    float lfoValue[kNumLfos] = {};
    float lfoBuf[kNumLfos][kInterval] = {}; // audio-rate LFOs: every sample up to the next tick
    FluxState flux[kNumFlux];
    float fluxValue[kNumFlux] = {};
    FluxState drift[4];
    float driftValue[4] = {};
    float driftAmount = 0.0f;
    float varCutoff = 0.0f, varPan = 0.0f, varLevel = 0.0f;
    float pressure = 0.0f, slide = 0.0f, polyPressure = 0.0f, noteBend = 0.0f; // smoothed
    float src[kNamedSources] = {};
    float off[kNumModRoutes] = {}; // sum of this voice's routes per per-voice destination
    // Voice-wide ramps.
    Ramp1 routing, warmth, noise;
    float warmState[2][2] = {}; // tilt filter per filter bus and channel
    std::uint32_t noiseState = 1;
    // Sources and filters.
    OscVoice osc[kOscCount];
    SubVoice sub;
    SubTargets subT;
    TextureVoice texture;
    TextureTargets textureT;
    FilterVoice filter[kNumFilters];
    // What each source last produced as a modulator: kHistory samples from the previous run, then
    // the current run.
    float mod[kNumSources][kHistory + kMaxRun] = {};
};

// Which source modulates which, and the order that follows from it.
struct Routing {
    int order[kNumSources] = {kSub, kTexture, 0, 1, 2};
    bool usedAsModulator[kNumSources] = {};
    bool delayed[kOscCount][2] = {}; // the modulator is evaluated later (or is the oscillator itself)
    bool loop = false;               // some oscillator needs a delayed modulator: render sample by sample
};

// Registry positions of the parameters the engine listens to.
struct Ids {
    int mode, polyphony, glide, glideCurve, glideMode, glideScaled, bendUp, bendDown, master, masterTune, reference;
    int quality, qualityOffline, mpe, mpeBendRange, followerSource, followerAttack, followerRelease;
    int steal, priority, velocitySens, filterRouting, drift, varCutoff, varEnv, varPan, varLevel, warmth, noiseFloor;
    struct Osc {
        int on, engine, position, octave, semi, fine, coarse, level, pan, phase, phaseMode, phaseRandom, keytrack;
        int shape, pulseWidth, warpMode[2], warpAmount[2], warpSource[2];
        int voices, detune, range, blend, width, positionSpread, warpSpread, law, stack, unisonPhase;
        int balance, direct;
    } osc[kOscCount];
    struct {
        int on, shape, octave, coarse, fine, level, pan, direct, balance;
    } sub;
    struct {
        int on, mode, start, randomStart, keytrack, pitch, fine, level, pan, direct, balance;
    } texture;
    struct {
        int on, type, cutoff, resonance, drive, character, stereoOffset, keytrack, mix, mixMode;
    } filter[kNumFilters];
    struct {
        int delay, attack, hold, decay, sustain, release, attackCurve, decayCurve, releaseCurve, mode;
    } env[kNumEnvs];
    struct {
        int mode, rate, sync, division, anchor, phase, delay, rise, smooth, polarity, scope, keytrack, audioRate;
    } lfo[kNumLfos];
    struct {
        int type, rate, sync, division, scope;
    } flux[kNumFlux];
    // Milestone 4: output, rack routing and play.
    int dcFilter, limiter, fxOff, midiOut, midiThru, fxBRouting, fxBalance[3];
    struct {
        int on, mode, octaves, rate, gate, swing, latch, restart, steps, pattern;
    } arp;
    struct {
        int memory, scaleLock, key, scale, lockMode, strum, strumDirection;
    } chord;
    struct {
        int on, slot, trigger, latch, loop;
    } clip;

    Ids() {
        const auto& r = Registry::get();
        const auto at = [&](const std::string& id) { return r.indexOf(id); };
        mode = at("global.mode");
        polyphony = at("global.polyphony");
        glide = at("global.glide");
        glideCurve = at("global.glide_curve");
        glideMode = at("global.glide_mode");
        glideScaled = at("global.glide_scaled");
        bendUp = at("global.bend_up");
        bendDown = at("global.bend_down");
        master = at("global.master");
        masterTune = at("global.master_tune");
        reference = at("global.reference");
        quality = at("global.quality");
        qualityOffline = at("global.quality_offline");
        mpe = at("global.mpe");
        mpeBendRange = at("global.mpe_bend_range");
        followerSource = at("global.follower.source");
        followerAttack = at("global.follower.attack");
        followerRelease = at("global.follower.release");
        steal = at("voice.steal");
        priority = at("voice.priority");
        velocitySens = at("voice.velocity_sens");
        filterRouting = at("voice.filter_routing");
        drift = at("voice.drift");
        varCutoff = at("voice.variance.cutoff");
        varEnv = at("voice.variance.env");
        varPan = at("voice.variance.pan");
        varLevel = at("voice.variance.level");
        warmth = at("voice.warmth");
        noiseFloor = at("voice.noise_floor");
        for (int i = 0; i < kOscCount; ++i) {
            const std::string p = "osc" + std::to_string(i + 1) + ".";
            Osc& o = osc[i];
            o.on = at(p + "on");
            o.engine = at(p + "engine");
            o.position = at(p + "position");
            o.octave = at(p + "octave");
            o.semi = at(p + "semi");
            o.fine = at(p + "fine");
            o.coarse = at(p + "coarse");
            o.level = at(p + "level");
            o.pan = at(p + "pan");
            o.phase = at(p + "phase");
            o.phaseMode = at(p + "phase_mode");
            o.phaseRandom = at(p + "phase_random");
            o.keytrack = at(p + "keytrack");
            o.shape = at(p + "classic.shape");
            o.pulseWidth = at(p + "classic.pulse_width");
            for (int s = 0; s < 2; ++s) {
                const std::string w = p + (s == 0 ? "warp_a." : "warp_b.");
                o.warpMode[s] = at(w + "mode");
                o.warpAmount[s] = at(w + "amount");
                o.warpSource[s] = at(w + "source");
            }
            o.voices = at(p + "unison.voices");
            o.detune = at(p + "unison.detune");
            o.range = at(p + "unison.range");
            o.blend = at(p + "unison.blend");
            o.width = at(p + "unison.width");
            o.positionSpread = at(p + "unison.position_spread");
            o.warpSpread = at(p + "unison.warp_spread");
            o.law = at(p + "unison.law");
            o.stack = at(p + "unison.stack");
            o.unisonPhase = at(p + "unison.phase");
            o.balance = at(p + "sends.filter_balance");
            o.direct = at(p + "sends.direct");
        }
        sub.on = at("sub.on");
        sub.shape = at("sub.shape");
        sub.octave = at("sub.octave");
        sub.coarse = at("sub.coarse");
        sub.fine = at("sub.fine");
        sub.level = at("sub.level");
        sub.pan = at("sub.pan");
        sub.direct = at("sub.direct");
        sub.balance = at("sub.sends.filter_balance");
        texture.on = at("texture.on");
        texture.mode = at("texture.mode");
        texture.start = at("texture.start");
        texture.randomStart = at("texture.random_start");
        texture.keytrack = at("texture.keytrack");
        texture.pitch = at("texture.pitch");
        texture.fine = at("texture.fine");
        texture.level = at("texture.level");
        texture.pan = at("texture.pan");
        texture.direct = at("texture.direct");
        texture.balance = at("texture.sends.filter_balance");
        for (int i = 0; i < kNumFilters; ++i) {
            const std::string p = "filter" + std::to_string(i + 1) + ".";
            auto& f = filter[i];
            f.on = at(p + "on");
            f.type = at(p + "type");
            f.cutoff = at(p + "cutoff");
            f.resonance = at(p + "resonance");
            f.drive = at(p + "drive");
            f.character = at(p + "character");
            f.stereoOffset = at(p + "stereo_offset");
            f.keytrack = at(p + "keytrack");
            f.mix = at(p + "mix");
            f.mixMode = at(p + "mix_mode");
        }
        for (int i = 0; i < kNumEnvs; ++i) {
            const std::string p = "env" + std::to_string(i + 1) + ".";
            auto& e = env[i];
            e.delay = at(p + "delay");
            e.attack = at(p + "attack");
            e.hold = at(p + "hold");
            e.decay = at(p + "decay");
            e.sustain = at(p + "sustain");
            e.release = at(p + "release");
            e.attackCurve = at(p + "attack_curve");
            e.decayCurve = at(p + "decay_curve");
            e.releaseCurve = at(p + "release_curve");
            e.mode = at(p + "mode"); // -1 for envelope 1
        }
        for (int i = 0; i < kNumLfos; ++i) {
            const std::string p = "lfo" + std::to_string(i + 1) + ".";
            auto& l = lfo[i];
            l.mode = at(p + "mode");
            l.rate = at(p + "rate");
            l.sync = at(p + "sync");
            l.division = at(p + "division");
            l.anchor = at(p + "anchor");
            l.phase = at(p + "phase");
            l.delay = at(p + "delay");
            l.rise = at(p + "rise");
            l.smooth = at(p + "smooth");
            l.polarity = at(p + "polarity");
            l.scope = at(p + "scope");
            l.keytrack = at(p + "keytrack");
            l.audioRate = at(p + "audio_rate");
        }
        for (int i = 0; i < kNumFlux; ++i) {
            const std::string p = "flux" + std::to_string(i + 1) + ".";
            auto& f = flux[i];
            f.type = at(p + "type");
            f.rate = at(p + "rate");
            f.sync = at(p + "sync");
            f.division = at(p + "division");
            f.scope = at(p + "scope");
        }
        dcFilter = at("global.dc_filter");
        limiter = at("global.limiter");
        fxOff = at("global.fx_off");
        midiOut = at("global.midi_out");
        midiThru = at("global.midi_thru");
        fxBRouting = at("fx.b_routing");
        fxBalance[0] = at("fx.filter1_balance");
        fxBalance[1] = at("fx.filter2_balance");
        fxBalance[2] = at("fx.direct_balance");
        arp.on = at("arp.on");
        arp.mode = at("arp.mode");
        arp.octaves = at("arp.octaves");
        arp.rate = at("arp.rate");
        arp.gate = at("arp.gate");
        arp.swing = at("arp.swing");
        arp.latch = at("arp.latch");
        arp.restart = at("arp.restart");
        arp.steps = at("arp.steps");
        arp.pattern = at("arp.pattern");
        chord.memory = at("chord.memory");
        chord.scaleLock = at("chord.scale_lock");
        chord.key = at("chord.key");
        chord.scale = at("chord.scale");
        chord.lockMode = at("chord.lock_mode");
        chord.strum = at("chord.strum");
        chord.strumDirection = at("chord.strum_direction");
        clip.on = at("clip.on");
        clip.slot = at("clip.slot");
        clip.trigger = at("clip.trigger");
        clip.latch = at("clip.latch");
        clip.loop = at("clip.loop");
    }
};
