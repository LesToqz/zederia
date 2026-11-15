// One voice: starting it, moving its modulators from one control tick to the next, and pointing
// its sources, filters and amp at where they should be by then. Included inside Engine::Impl.
//
// Every modulator holds its value at the next control tick, and everything that sounds moves
// there in a straight line, so nothing waits a tick and nothing steps.

bool hasOwn(int module) const { return ((polyMask >> module) & 1u) != 0; }
bool lfoOwn(int k) const { return hasOwn(kModLfo + k) || lfo[k].keytrack != 0.0f; }

double velocityGain(const Voice& v) const { return 1.0 - velocitySens + velocitySens * v.velocity; }

double bendSemis(const Voice& v) const {
    const double wheel = gsrc[kSrcPitchBend];
    return (wheel >= 0.0 ? wheel * bendUp : wheel * bendDown) + v.noteBend * mpeBendRange;
}

// Cycles per sample at the base rate for a pitch in semitones.
double pitchInc(double note) const { return std::min(noteToHz(note, referenceHz) / sampleRate, 0.49); }

// --- Aiming --------------------------------------------------------------------------------------

// Points every lane's ramps at the values wanted at the next control tick.
void aimLanes(LaneSet& s, const OscShared& o, const TableData& table, const LaneTable& t, const float* ratio,
              double baseInc, bool snap) {
    osc::Lanes& L = s.lanes;
    L.count = o.count;
    const float maxFrame = static_cast<float>(table.frames() - 1);
    const auto rate = static_cast<float>(o.oversample);
    const float perStep = 1.0f / (static_cast<float>(samplesToTick) * rate);
    const auto base = static_cast<float>(baseInc);
    const auto ramp = [&](float& now, float& step, float& target, float v) {
        target = v;
        if (snap)
            now = v;
        step = (v - now) * perStep;
    };
    for (int l = 0; l < o.padded; ++l) {
        ramp(L.inc[l], L.incStep[l], s.incT[l], std::min(base * ratio[l], 0.49f) / rate);
        float frame = t.pos[l] * maxFrame;
        if (table.stepped())
            frame = std::floor(frame + 0.5f);
        if (table.stepped() && !snap) { // a staircase on purpose: no glide between frames
            s.fposT[l] = L.fpos[l] = frame;
            L.fposStep[l] = 0.0f;
        } else {
            ramp(L.fpos[l], L.fposStep[l], s.fposT[l], frame);
        }
        ramp(L.dA[l], L.dAStep[l], s.dAT[l], t.d[0][l]);
        ramp(L.dB[l], L.dBStep[l], s.dBT[l], t.d[1][l]);
        ramp(L.gL[l], L.gLStep[l], s.gLT[l], t.gL[l]);
        ramp(L.gR[l], L.gRStep[l], s.gRT[l], t.gR[l]);
        ramp(L.gM[l], L.gMStep[l], s.gMT[l], t.gM[l]);

        // The band-limited level for the highest pitch this lane reaches before the next tick.
        const float inc = std::max(L.inc[l], s.incT[l]) * rate * t.slope[l];
        const int level = TableData::levelFor(inc);
        if (snap) {
            s.level[l] = s.levelNext[l] = level;
            setLaneLevel(s, l, table, level, false);
            setLaneLevel(s, l, table, level, true);
            L.xf[l] = L.xfStep[l] = 0.0f;
        } else if (level != s.level[l] && s.levelNext[l] == s.level[l]) {
            s.levelNext[l] = level;
            setLaneLevel(s, l, table, level, true);
            L.xf[l] = 0.0f;
            L.xfStep[l] = perStep;
            s.levelFade = true;
        }
    }
    if (snap)
        s.levelFade = false;
}

// At a control tick: every ramp has arrived; level fades are complete.
static void settleLanes(LaneSet& s, int padded) {
    osc::Lanes& L = s.lanes;
    for (int l = 0; l < padded; ++l) {
        L.inc[l] = s.incT[l];
        L.fpos[l] = s.fposT[l];
        L.dA[l] = s.dAT[l];
        L.dB[l] = s.dBT[l];
        L.gL[l] = s.gLT[l];
        L.gR[l] = s.gRT[l];
        L.gM[l] = s.gMT[l];
        if (s.levelNext[l] != s.level[l]) {
            s.level[l] = s.levelNext[l];
            L.tabOff[l] = L.tabOff2[l];
            L.tabStride[l] = L.tabStride2[l];
            L.tabSize[l] = L.tabSize2[l];
        }
        L.xf[l] = L.xfStep[l] = 0.0f;
    }
    s.levelFade = false;
}

void aimOsc(Voice& v, int i, bool snap) {
    OscShared& o = osc[i];
    OscVoice& ov = v.osc[i];
    const OscTargets* t = &o.t;
    const LaneTable* lanes = &o.lanes;
    if (hasOwn(kModOsc + i) || o.uniCount > 0) {
        // A per-voice route reaches this oscillator: the voice works out its own values.
        if (hasOwn(kModOsc + i))
            deriveOsc(i, [&](int index) { return vval(v, index); }, ov.t);
        else
            ov.t = o.t;
        t = &ov.t;
        if (o.uniCount > 0 || ov.lanesStructure != o.structure || !(ov.t.in == ov.lanesFor)) {
            UniTerm terms[kMaxUniTerms];
            const RouteTable* table = routes.current();
            for (int n = 0; n < o.uniCount; ++n) {
                const int route = o.uniRoute[n];
                const RouteDef& d = table->route[route];
                float gain = static_cast<float>(value(reg.modAmount(route))) * o.uniScale[n];
                if (d.via >= 0)
                    gain *= modVia(srcValue(d.via, v.src), srcBipolar[d.via], d.viaInvert);
                terms[n] = {o.uniDest[n], gain, d.polarity, d.curve,
                            d.drawn >= 0 ? table->curves[static_cast<std::size_t>(d.drawn)].data() : nullptr};
            }
            computeLaneTable(o, ov.t.in, terms, o.uniCount, ov.lanes);
            ov.lanesFor = ov.t.in;
            ov.lanesStructure = o.structure;
        }
        lanes = &ov.lanes;
    }
    const double note = (o.keytrack ? v.pitch : kFixedPitchNote) + t->tuneSemis + masterTuneSemis + bendSemis(v) +
                        v.driftValue[i] * v.driftAmount * kDriftSemis;
    const double base = pitchInc(note);
    const float* ratio = lanes->ratio;
    if (o.law == UnisonLaw::Random) {
        if (ov.ratioTable != lanes || ov.ratioEpoch != lanes->epoch) {
            for (int l = 0; l < kMaxLanes; ++l)
                ov.ratio[l] = std::exp2((ov.randomDetune[l < o.count ? l : 0] * lanes->spreadSemis * 0.5f +
                                         lanes->extraSemis[l] + lanes->stackSemis[l]) / 12.0f);
            ov.ratioTable = lanes;
            ov.ratioEpoch = lanes->epoch;
        }
        ratio = ov.ratio;
    }
    aimLanes(ov.main, o, *o.table, *lanes, ratio, base, snap);
    if (ov.fadeLeft > 0 && o.oldTable != nullptr)
        aimLanes(ov.fade, o, *o.oldTable, *lanes, ratio, base, snap);
    float left = 0.0f, right = 0.0f;
    balance(t->pan, t->levelGain, left, right);
    ov.gain.aim(left, right, samplesToTick, snap);
    ov.send.aim(t->send, samplesToTick, snap);
    ov.shape.aim(t->shape, samplesToTick, snap);
    ov.pulseWidth.aim(t->pulseWidth, samplesToTick, snap);
}

void aimSub(Voice& v, bool snap) {
    const SubTargets* t = &sub.t;
    if (hasOwn(kModSub)) {
        deriveSub([&](int index) { return vval(v, index); }, v.subT);
        t = &v.subT;
    }
    const double note = v.pitch + t->tuneSemis + masterTuneSemis + bendSemis(v) + v.driftValue[3] * v.driftAmount * kDriftSemis;
    v.sub.incTarget = pitchInc(note) / sub.oversample;
    if (snap)
        v.sub.inc = v.sub.incTarget;
    v.sub.incStep = (v.sub.incTarget - v.sub.inc) / (samplesToTick * sub.oversample);
    float left = 0.0f, right = 0.0f;
    balance(t->pan, t->levelGain * std::sqrt(0.5), left, right); // a mono source: -3 dB per side at the centre
    v.sub.gain.aim(left, right, samplesToTick, snap);
    const float send[3] = {sub.direct ? 0.0f : 1.0f - t->balance, sub.direct ? 0.0f : t->balance, sub.direct ? 1.0f : 0.0f};
    v.sub.send.aim(send, samplesToTick, snap);
}

void aimTexture(Voice& v, bool snap) {
    const TextureTargets* t = &texture.t;
    if (hasOwn(kModTexture)) {
        deriveTexture([&](int index) { return vval(v, index); }, v.textureT);
        t = &v.textureT;
    }
    const SampleData& sample = *texture.sample;
    const double semis = t->tuneSemis + masterTuneSemis + bendSemis(v) + (texture.keytrack ? v.pitch - kFixedPitchNote : 0.0);
    const double rate = std::min(sample.sampleRate / sampleRate * std::exp2(semis / 12.0), 64.0) / texture.oversample;
    v.texture.rateTarget = rate;
    if (snap)
        v.texture.rate = rate;
    v.texture.rateStep = (rate - v.texture.rate) / (samplesToTick * texture.oversample);
    float left = 0.0f, right = 0.0f;
    balance(t->pan, t->levelGain, left, right);
    v.texture.gain.aim(left, right, samplesToTick, snap);
    const float send[3] = {texture.direct ? 0.0f : 1.0f - t->balance, texture.direct ? 0.0f : t->balance,
                           texture.direct ? 1.0f : 0.0f};
    v.texture.send.aim(send, samplesToTick, snap);
}

void aimFilter(Voice& v, int f, bool snap) {
    const FilterShared& fs = filter[f];
    FilterVoice& fv = v.filter[f];
    if (!fs.on) {
        fv.running = false;
        return;
    }
    const FilterTargets* t = &fs.t;
    if (hasOwn(kModFilter + f)) {
        deriveFilter(f, [&](int index) { return vval(v, index); }, fv.t);
        t = &fv.t;
    }
    // Key tracking pivots on middle C; the variance offset was drawn when the note started.
    const double octaves = fs.keytrack * (v.pitch + bendSemis(v) - kFixedPitchNote) / 12.0 +
                           v.varCutoff * value(ids.varCutoff) * kVarianceCutoffOct;
    const double centre = t->cutoff * (octaves != 0.0 ? std::exp2(octaves) : 1.0);
    const double apart = t->stereoOffset != 0.0f ? std::exp2(static_cast<double>(t->stereoOffset * kStereoOffsetOct)) : 1.0;
    flt::Settings s;
    s.type = fs.type;
    s.cutoffHz[0] = static_cast<float>(std::clamp(centre / apart, 4.0, 30000.0));
    s.cutoffHz[1] = static_cast<float>(std::clamp(centre * apart, 4.0, 30000.0));
    s.resonance = t->resonance;
    s.drive = t->drive;
    s.character = t->character;
    s.mix = t->mix;
    s.levelMode = fs.levelMode;
    const bool fresh = snap || !fv.running;
    if (!fv.running) {
        fv.filter.reset();
        fv.running = true;
    }
    // A filter whose settings did not change keeps the ramps it has: at a tick they have arrived
    // and stay put, and in between they are already on their way to the same place.
    if (fresh || !(s == fv.aimed) || oversample != fv.aimedOversample) {
        fv.filter.aim(s, samplesToTick, oversample, fresh);
        fv.aimed = s;
        fv.aimedOversample = oversample;
    }
    const RouteTable* table = routes.current();
    for (int n = 0; n < fs.audioCount; ++n) {
        const int route = fs.audioRoute[n];
        const RouteDef& d = table->route[route];
        float depth = static_cast<float>(value(reg.modAmount(route))) * cutoffOctaves;
        if (d.via >= 0)
            depth *= modVia(srcValue(d.via, v.src), srcBipolar[d.via], d.viaInvert);
        fv.depth[n].aim(depth, samplesToTick, fresh);
    }
    if (&v == leadVoice())
        viewSettings[f] = s;
}

void aimAmp(Voice& v, bool snap) {
    double level = v.envTarget * velocityGain(v);
    const double levelVariance = value(ids.varLevel);
    if (levelVariance > 0.0)
        level *= std::pow(10.0, v.varLevel * levelVariance * kVarianceLevelDb / 20.0);
    float target[2] = {static_cast<float>(level), static_cast<float>(level)};
    const double panVariance = value(ids.varPan);
    if (panVariance > 0.0)
        balance(v.varPan * panVariance, level, target[0], target[1]);
    for (int c = 0; c < 2; ++c) {
        if (snap)
            v.amp[c] = target[c];
        v.ampStep[c] = (target[c] - v.amp[c]) / static_cast<float>(samplesToTick);
    }
}

// This voice's own envelope times and LFO settings, for the modulators a per-voice route reaches
// (and for LFOs whose rate follows the keyboard).
void ownModulators(Voice& v) {
    const auto get = [&](int index) { return vval(v, index); };
    for (int k = 0; k < kNumEnvs; ++k)
        if (hasOwn(kModEnv + k))
            deriveEnv(k, get, v.envT[k]);
    for (int k = 0; k < kNumLfos; ++k) {
        const LfoShared& l = lfo[k];
        if (!l.used || l.global || !lfoOwn(k))
            continue;
        deriveLfo(k, get, v.lfoP[k]);
        if (l.keytrack != 0.0f)
            v.lfoP[k].cyclesPerSample = std::min(v.lfoP[k].cyclesPerSample * std::exp2(l.keytrack * (v.keyNow - kFixedPitchNote) / 12.0),
                                                 (l.audioRate ? kMaxAudioLfoHz : kMaxLfoHz) / sampleRate);
    }
}

// Points everything that sounds at the next control tick. `snap` jumps there (a new note);
// `snapSources` jumps only the sources (their oversampling changed).
void aimVoice(Voice& v, bool snap, bool snapSources) {
    v.pitch = tuningNow->pitchOf(v.keyNow);
    v.driftAmount = static_cast<float>(vval(v, ids.drift));
    for (int i = 0; i < kOscCount; ++i)
        if (osc[i].on)
            aimOsc(v, i, snap || snapSources);
    if (sub.on)
        aimSub(v, snap || snapSources);
    if (texture.on)
        aimTexture(v, snap || snapSources);
    for (int f = 0; f < kNumFilters; ++f)
        aimFilter(v, f, snap);
    aimAmp(v, snap);
    v.routing.aim(static_cast<float>(vval(v, ids.filterRouting)), samplesToTick, snap);
    v.warmth.aim(static_cast<float>(vval(v, ids.warmth)), samplesToTick, snap);
    const auto noise = static_cast<float>(vval(v, ids.noiseFloor));
    v.noise.aim(noise * noise * kNoiseFloorGain, samplesToTick, snap);
    ownModulators(v);
}

// At a control tick: every ramp has arrived.
void settleVoice(Voice& v) {
    for (int i = 0; i < kOscCount; ++i) {
        if (!osc[i].on)
            continue;
        settleLanes(v.osc[i].main, osc[i].padded);
        if (v.osc[i].fadeLeft > 0)
            settleLanes(v.osc[i].fade, osc[i].padded);
        v.osc[i].gain.settle();
        v.osc[i].send.settle();
        v.osc[i].shape.settle();
        v.osc[i].pulseWidth.settle();
    }
    v.sub.inc = v.sub.incTarget;
    v.sub.gain.settle();
    v.sub.send.settle();
    v.texture.rate = v.texture.rateTarget;
    v.texture.gain.settle();
    v.texture.send.settle();
    for (int f = 0; f < kNumFilters; ++f) {
        if (!v.filter[f].running)
            continue;
        v.filter[f].filter.settle();
        for (int n = 0; n < filter[f].audioCount; ++n)
            v.filter[f].depth[n].settle();
    }
    v.routing.settle();
    v.warmth.settle();
    v.noise.settle();
}

// A source's oversampling changed: its decimators start clean.
void resetRates(Voice& v) {
    for (auto& o : v.osc) {
        o.decimatorL.reset();
        o.decimatorR.reset();
    }
    v.sub.decimator.reset();
    v.texture.decimatorL.reset();
    v.texture.decimatorR.reset();
}

// --- Modulators ----------------------------------------------------------------------------------

double envAdvance(Voice& v, int k, double samples) {
    if (env[k].multi)
        return v.menv[k].advance(samples, env[k].def);
    return v.env[k].advance(samples, hasOwn(kModEnv + k) ? v.envT[k] : env[k].t);
}

// Envelope 1 at this exact sample: its ramp runs backwards from the target.
double envNow(const Voice& v) const { return v.envTarget - v.envStep * samplesToTick; }

// Moves the envelopes `samples` on. Envelope 1 ramps from v.envTarget, so a caller that changes
// its stage in the middle of a control interval first sets v.envTarget to the level right now.
void advanceEnvs(Voice& v, double samples) {
    const double from = v.envTarget;
    const double step = samples * v.envRate;
    v.envTarget = envAdvance(v, 0, step);
    v.envStep = (v.envTarget - from) / samples;
    v.envValue[0] = static_cast<float>(v.envTarget);
    v.done = false;
    for (int k = 1; k < kNumEnvs; ++k)
        if (env[k].used)
            v.envValue[k] = static_cast<float>(envAdvance(v, k, step));
}

void triggerEnvs(Voice& v, double from) {
    v.env[0].trigger(from);
    for (int k = 1; k < kNumEnvs; ++k) {
        if (env[k].multi)
            v.menv[k].trigger(v.envValue[k]);
        else
            v.env[k].trigger(v.envValue[k]);
    }
}

void releaseEnvs(Voice& v, double from) {
    v.env[0].release(from);
    for (int k = 1; k < kNumEnvs; ++k) {
        if (env[k].multi)
            v.menv[k].release(env[k].def);
        else
            v.env[k].release(v.envValue[k]);
    }
}

void advanceVoiceLfo(Voice& v, int k, double samples) {
    const LfoShared& l = lfo[k];
    v.lfoValue[k] = advanceLfo(v.lfo[k], lfoOwn(k) ? v.lfoP[k] : l.p, l, samples, v.lfoBuf[k]);
}

void advanceLfos(Voice& v, double samples) {
    for (int k = 0; k < kNumLfos; ++k)
        if (lfo[k].used && !lfo[k].global)
            advanceVoiceLfo(v, k, samples);
}

// Starts a per-voice LFO for a new note. A free-running one joins the phase it would have had
// if it had never stopped, with no delay or fade-in.
void triggerVoiceLfo(Voice& v, int k) {
    const LfoShared& l = lfo[k];
    if (l.mode == LfoMode::Free) {
        lfoTrigger(v.lfo[k], l.freePhase - l.p.cyclesPerSample * samplesToTick);
        v.lfo[k].age = 1.0e12;
    } else {
        lfoTrigger(v.lfo[k], 0.0);
    }
}

// The shared LFOs that restart with every note.
void retriggerSharedLfos() {
    for (int k = 0; k < kNumLfos; ++k) {
        LfoShared& l = lfo[k];
        if (!l.used || !l.global || l.mode == LfoMode::Free)
            continue;
        lfoTrigger(l.state, 0.0);
        l.value = advanceLfo(l.state, l.p, l, samplesToTick, l.buf);
        gsrc[kSrcLfo + k] = l.value;
    }
}

// Glide, Flux, drift and the per-note controllers.
void advanceRest(Voice& v, double samples, bool snap) {
    if (v.glideT < 1.0) {
        v.glideT = v.glideSamples > 0.0 ? std::min(v.glideT + samples / v.glideSamples, 1.0) : 1.0;
        v.keyNow = v.glideT >= 1.0 ? v.keyTo
                                   : v.keyFrom + (v.keyTo - v.keyFrom) * curveValue(v.glideT, value(ids.glideCurve));
    }
    for (int k = 0; k < kNumFlux; ++k)
        if (flux[k].used && !flux[k].global)
            v.fluxValue[k] = fluxAdvance(v.flux[k], flux[k].type, flux[k].cyclesPerSample * samples);
    if (v.driftAmount > 0.0f) {
        static constexpr double hz[4] = {0.31, 0.23, 0.37, 0.27};
        for (int j = 0; j < 4; ++j)
            v.driftValue[j] = fluxAdvance(v.drift[j], FluxType::Smooth, hz[j] / sampleRate * samples);
    }
    const bool perNote = mpe && v.channel != 0;
    const float pressure = perNote ? channelPressure[v.channel] : 0.0f, slide = perNote ? channelSlide[v.channel] : 0.0f;
    const float bend = perNote ? channelBend[v.channel] : 0.0f, poly = polyPressure[v.note & 127];
    if (snap) {
        v.pressure = pressure;
        v.slide = slide;
        v.noteBend = bend;
        v.polyPressure = poly;
    } else {
        approach(v.pressure, pressure, controllerSmoothing);
        approach(v.slide, slide, controllerSmoothing);
        approach(v.noteBend, bend, controllerSmoothing);
        approach(v.polyPressure, poly, controllerSmoothing);
    }
}

// Reads the voice's sources and adds up its per-voice routes.
void evalVoice(Voice& v) {
    float* s = v.src;
    for (int k = 0; k < kNumEnvs; ++k)
        s[kSrcEnv + k] = v.envValue[k];
    for (int k = 0; k < kNumLfos; ++k)
        s[kSrcLfo + k] = v.lfoValue[k];
    for (int k = 0; k < kNumFlux; ++k)
        s[kSrcFlux + k] = v.fluxValue[k];
    s[kSrcNote] = static_cast<float>(std::clamp((v.keyNow - kFixedPitchNote) / 60.0, -1.0, 1.0));
    s[kSrcPolyAftertouch] = v.polyPressure;
    s[kSrcMpePressure] = v.pressure;
    s[kSrcMpeSlide] = v.slide;
    for (int j = 0; j < kNumSources; ++j)
        if (tapped[j])
            s[kSrcAudio + j] = v.mod[j][kHistory - 1]; // the source's most recent sample
    for (int n = 0; n < polyDestCount; ++n)
        v.off[n] = 0.0f;
    for (int n = 0; n < polyCount; ++n)
        v.off[polyRoutes[n].slot] += routeSignal(polyRoutes[n], s);
    if (&v == leadVoice())
        std::copy(s, s + kNamedSources, leadSrc);
}

// --- Notes ---------------------------------------------------------------------------------------

Voice* leadVoice() { return lastStarted >= 0 ? &voices[static_cast<std::size_t>(lastStarted)] : nullptr; }

// Where the pitch should travel: from `from` to `to`, in the glide time (per octave when scaled).
void setGlide(Voice& v, double from, double to) {
    double samples = value(ids.glide) * sampleRate / 1000.0;
    if (glideScaled)
        samples *= std::abs(to - from) / 12.0;
    const bool glides = samples > 0.0 && from != to && (!glideLegato || keysHeldBefore > 0);
    v.keyFrom = glides ? from : to;
    v.keyTo = to;
    v.keyNow = v.keyFrom;
    v.glideSamples = samples;
    v.glideT = glides ? 0.0 : 1.0;
    lastKey = to;
}

// The per-note values every new note draws.
void newNoteValues(Voice& v, float velocity) {
    v.velocity = velocity;
    v.channel = eventChannel;
    v.src[kSrcVelocity] = velocity;
    v.src[kSrcRandom1] = static_cast<float>(rng.uniform());
    v.src[kSrcRandom2] = static_cast<float>(rng.uniform());
    alternate = -alternate;
    v.src[kSrcAlternate] = alternate;
}

// The unison voice count or spread law changed while a note plays.
void relayoutVoice(Voice& v, int i) {
    OscVoice& ov = v.osc[i];
    for (int l = 0; l < kMaxLanes; ++l) {
        if (osc[i].law == UnisonLaw::Random)
            ov.randomDetune[l] = l == 0 && osc[i].count % 2 == 1 ? 0.0f : static_cast<float>(rng.uniform() * 2.0 - 1.0);
        if (l >= ov.main.lanes.count) // lanes that are new start somewhere of their own
            ov.main.lanes.phase[l] = toPhase(rng.uniform());
    }
    ov.ratioTable = nullptr;
    aimOsc(v, i, true);
}

void startVoice(Voice& v, int slot, int note, float velocity) {
    const int rank = std::max(allocator.sounding() - 1, 0); // voices already sounding
    v.active = true;
    v.done = false;
    v.note = note;
    v.amp[0] = v.amp[1] = 0.0f;
    v.ampStep[0] = v.ampStep[1] = 0.0f;
    v.envTarget = v.envStep = 0.0;
    for (int k = 0; k < kNumEnvs; ++k) {
        v.env[k] = Envelope{};
        v.menv[k] = MultiEnvelope{};
        v.envValue[k] = 0.0f;
    }
    std::memset(v.mod, 0, sizeof v.mod);
    std::fill(v.off, v.off + kNumModRoutes, 0.0f);
    newNoteValues(v, velocity);
    v.src[kSrcReleaseVelocity] = 0.0f;
    v.src[kSrcVoiceIndex] = std::min(static_cast<float>(rank) / static_cast<float>(std::max(polyphony - 1, 1)), 1.0f);
    v.varCutoff = static_cast<float>(rng.uniform() * 2.0 - 1.0);
    v.varPan = static_cast<float>(rng.uniform() * 2.0 - 1.0);
    v.varLevel = static_cast<float>(rng.uniform() * 2.0 - 1.0);
    v.envRate = std::exp2(-(rng.uniform() * 2.0 - 1.0) * value(ids.varEnv));
    v.noiseState = static_cast<std::uint32_t>(rng.next()) | 1u;
    std::memset(v.warmState, 0, sizeof v.warmState);
    for (auto& f : v.filter)
        f.running = false;
    setGlide(v, lastKey >= 0.0 ? lastKey : note, note);

    const Voice* previous = lastStarted >= 0 && lastStarted != slot && voices[static_cast<std::size_t>(lastStarted)].active
                                ? &voices[static_cast<std::size_t>(lastStarted)]
                                : nullptr;
    for (int i = 0; i < kOscCount; ++i) {
        OscShared& o = osc[i];
        OscVoice& ov = v.osc[i];
        ov.fadeLeft = 0;
        ov.ratioTable = nullptr;
        ov.lanesStructure = 0;
        ov.decimatorL.reset();
        ov.decimatorR.reset();
        if (!o.on)
            continue;
        if (o.law == UnisonLaw::Random)
            for (int l = 0; l < kMaxLanes; ++l)
                ov.randomDetune[l] = l == 0 && o.count % 2 == 1 ? 0.0f : static_cast<float>(rng.uniform() * 2.0 - 1.0);
        std::uint32_t* phase = ov.main.lanes.phase;
        if (o.continuePhase) {
            // Carry on from where the previous note's oscillator is right now; with no
            // previous note the lanes simply keep running from where this voice left them.
            if (previous != nullptr)
                std::copy(previous->osc[i].main.lanes.phase, previous->osc[i].main.lanes.phase + kMaxLanes, phase);
        } else if (o.count > 1 && o.unisonPhase == UnisonPhase::Free) {
            // Free: the lanes keep running across notes.
        } else {
            const double base = o.startPhase + (o.phaseRandom > 0.0f ? o.phaseRandom * rng.uniform() : 0.0);
            phase[0] = toPhase(base);
            for (int l = 1; l < kMaxLanes; ++l) {
                const double offset = l >= o.count ? 0.0
                                      : o.unisonPhase == UnisonPhase::Consistent ? o.consistentPhase[l]
                                                                                 : rng.uniform();
                phase[l] = toPhase(base + offset);
            }
        }
    }
    v.sub.phase = 0.0;
    v.sub.decimator.reset();
    v.texture.fadeLeft = 0;
    v.texture.decimatorL.reset();
    v.texture.decimatorR.reset();
    v.texture.main.finished = false;
    lastStarted = slot;

    // Modulators as they are at this very sample.
    for (int k = 0; k < kNumFlux; ++k) {
        fluxReset(v.flux[k], rng.next());
        v.fluxValue[k] = flux[k].used && !flux[k].global ? fluxAdvance(v.flux[k], flux[k].type, 0.0) : 0.0f;
    }
    for (int j = 0; j < 4; ++j) {
        fluxReset(v.drift[j], rng.next());
        v.driftValue[j] = 0.0f;
    }
    v.driftAmount = 0.0f;
    advanceRest(v, 0.0, true);
    ownModulators(v);
    for (int k = 0; k < kNumLfos; ++k) {
        v.lfoValue[k] = 0.0f;
        if (lfo[k].used && !lfo[k].global) {
            triggerVoiceLfo(v, k);
            v.lfoValue[k] = lfoAdvance(v.lfo[k], lfoOwn(k) ? v.lfoP[k] : lfo[k].p, *lfo[k].table, 0.0);
        }
    }
    evalVoice(v);
    if (texture.on) {
        const double start = hasOwn(kModTexture) ? vval(v, ids.texture.start) : static_cast<double>(texture.t.start);
        const double random = texture.randomStart > 0.0f ? texture.randomStart * rng.uniform() : 0.0;
        v.texture.main.position = std::clamp(start + random * (1.0 - start), 0.0, 1.0) * std::max(texture.sample->frames - 1, 0);
    } else {
        v.texture.main.position = 0.0;
    }
    aimVoice(v, true, true);
}

// After something changed in the middle of a control interval: the voice's sources are read
// again and everything is pointed at the next tick once more.
void reaim(Voice& v) {
    evalVoice(v);
    aimVoice(v, false, false);
}

void applyActions() {
    for (int i = 0; i < actions.count; ++i) {
        const VoiceAction& a = actions.items[static_cast<std::size_t>(i)];
        Voice& v = voices[static_cast<std::size_t>(a.slot)];
        switch (a.kind) {
        case VoiceAction::Kind::Start: {
            const bool wasActive = v.active;
            startVoice(v, a.slot, a.note, a.velocity);
            triggerEnvs(v, 0.0);
            retriggerSharedLfos();
            advanceEnvs(v, samplesToTick);
            advanceLfos(v, samplesToTick);
            advanceRest(v, samplesToTick, true);
            reaim(v);
            activeCount += wasActive ? 0 : 1;
            break;
        }
        case VoiceAction::Kind::Retrigger: {
            if (!v.active)
                break;
            const double from = envNow(v);
            v.note = a.note;
            newNoteValues(v, a.velocity);
            setGlide(v, v.keyNow, a.note);
            v.envTarget = from;
            triggerEnvs(v, from);
            advanceEnvs(v, samplesToTick);
            for (int k = 0; k < kNumLfos; ++k) {
                if (lfo[k].used && !lfo[k].global && lfo[k].mode != LfoMode::Free) {
                    triggerVoiceLfo(v, k);
                    advanceVoiceLfo(v, k, samplesToTick);
                }
            }
            retriggerSharedLfos();
            lastStarted = a.slot;
            reaim(v);
            break;
        }
        case VoiceAction::Kind::Glide:
            if (v.active) {
                v.note = a.note;
                v.channel = eventChannel;
                setGlide(v, v.keyNow, a.note);
                reaim(v);
            }
            break;
        case VoiceAction::Kind::Release:
            if (v.active) {
                const double from = envNow(v);
                v.src[kSrcReleaseVelocity] = releaseVelocity;
                releaseEnvs(v, from);
                v.envTarget = from;
                advanceEnvs(v, samplesToTick);
                reaim(v);
            }
            break;
        case VoiceAction::Kind::Kill:
            if (v.active) {
                const double from = envNow(v);
                v.env[0].kill(from, kStealFadeSeconds * sampleRate);
                v.envTarget = from;
                advanceEnvs(v, samplesToTick);
                reaim(v);
            }
            break;
        }
    }
    actions.count = 0;
}

// The control tick of one voice.
void voiceTick(Voice& v, int slot) {
    settleVoice(v);
    advanceEnvs(v, kInterval);
    v.done = v.env[0].finished();
    advanceLfos(v, kInterval);
    advanceRest(v, kInterval, false);
    reaim(v);
    allocator.setLevel(slot, static_cast<float>(v.envTarget * velocityGain(v)));
}
