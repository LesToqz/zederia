// Parameters, modulation sources and the matrix. Included inside Engine::Impl.
//
// A parameter passes through three stages on the audio thread:
//   p   the value last written by anyone (plain units)
//   nb  the same after smoothing, normalized 0..1 (continuous parameters only)
//   pe  nb plus the routes that are the same for every voice, back in plain units
// Routes with a per-voice end add a per-voice offset on top (Voice::off), read through vval().
// Everything the engine computes from parameters (derive()) reads pe, so smoothing and modulation
// reach every control the same way, and a parameter nobody moves or modulates costs nothing.

// --- Parameter values --------------------------------------------------------------------------

double value(int index) const { return pe[static_cast<std::size_t>(index)]; }
bool flag(int index) const { return p[static_cast<std::size_t>(index)] >= 0.5; }
int choice(int index) const { return static_cast<int>(p[static_cast<std::size_t>(index)]); }

// A parameter as one voice hears it.
double vval(const Voice& v, int index) const {
    const int slot = polySlotOf[static_cast<std::size_t>(index)];
    if (slot < 0)
        return pe[static_cast<std::size_t>(index)];
    const int g = globSlotOf[static_cast<std::size_t>(index)];
    const double n = nb[static_cast<std::size_t>(index)] + (g >= 0 ? gOff[g] : 0.0f) + v.off[slot];
    return defOf(index).fromNormalized(std::clamp(n, 0.0, 1.0));
}

// Recomputes pe for one parameter from its smoothed base and its global routes.
void applyParam(int index) {
    const auto i = static_cast<std::size_t>(index);
    const int g = globSlotOf[i];
    if (g < 0 && moving[i] == 0) {
        pe[i] = p[i];
        return;
    }
    pe[i] = defOf(index).fromNormalized(std::clamp(nb[i] + (g >= 0 ? gOff[g] : 0.0f), 0.0, 1.0));
}

// A value arrived from the parameter store.
void setParam(int index, double v) {
    const auto i = static_cast<std::size_t>(index);
    p[i] = v;
    dirty = true;
    if (isFloat[i] == 0) {
        pe[i] = v;
        return;
    }
    nt[i] = defOf(index).toNormalized(v);
    if (moving[i] == 0) {
        moving[i] = 1;
        movingList[static_cast<std::size_t>(movingCount++)] = index;
    }
}

// After a reset: every parameter is where it should be, with no glide.
void snapParams() {
    for (int index = 0; index < reg.size(); ++index) {
        const auto i = static_cast<std::size_t>(index);
        if (isFloat[i] != 0)
            nb[i] = nt[i] = defOf(index).toNormalized(p[i]);
        moving[i] = 0;
        pe[i] = p[i];
    }
    movingCount = 0;
    for (int n = 0; n < globDestCount; ++n)
        applyParam(globDest[n]);
}

// One control-rate step of every parameter that is on its way to a new value.
void smoothParams() {
    int kept = 0;
    for (int n = 0; n < movingCount; ++n) {
        const int index = movingList[static_cast<std::size_t>(n)];
        const auto i = static_cast<std::size_t>(index);
        double next = nb[i] + (nt[i] - nb[i]) * smoothing;
        if (std::abs(nt[i] - next) < 1.0e-5) {
            next = nt[i];
            moving[i] = 0;
        } else {
            movingList[static_cast<std::size_t>(kept++)] = index;
        }
        nb[i] = next;
        applyParam(index);
    }
    movingCount = kept;
}

// --- Sources -----------------------------------------------------------------------------------

static bool isAudioTap(int source) { return source >= kSrcAudio && source < kSrcAudio + kNumSources; }
bool isAudioRate(int source) const {
    return isAudioTap(source) || (source >= kSrcLfo && source < kSrcLfo + kNumLfos && lfo[source - kSrcLfo].audioRate);
}

float srcValue(int index, const float* voiceSources) const {
    return index < kNamedSources && srcIsVoice[index] ? voiceSources[index] : gsrc[index];
}

float routeSignal(const ActiveRoute& r, const float* voiceSources) const {
    float signal = modShape(srcValue(r.source, voiceSources), srcBipolar[r.source], r.polarity, r.curve, r.drawn);
    if (r.via >= 0)
        signal *= modVia(srcValue(r.via, voiceSources), srcBipolar[r.via], r.viaInvert);
    return signal * static_cast<float>(pe[static_cast<std::size_t>(r.amount)]);
}

void markUsed(int source) {
    if (source >= kSrcEnv && source < kSrcEnv + kNumEnvs)
        env[source - kSrcEnv].used = true;
    else if (source >= kSrcLfo && source < kSrcLfo + kNumLfos)
        lfo[source - kSrcLfo].used = true;
    else if (source >= kSrcFlux && source < kSrcFlux + kNumFlux)
        flux[source - kSrcFlux].used = true;
    else if (isAudioTap(source))
        tapped[source - kSrcAudio] = true;
    else if (source == kSrcFollower)
        followerUsed = true;
    else if (source >= kSrcCc && ccUsedCount < 128) {
        bool listed = false;
        for (int n = 0; n < ccUsedCount; ++n)
            listed = listed || ccUsed[n] == source - kSrcCc;
        if (!listed) {
            ccUsed[ccUsedCount++] = static_cast<std::uint8_t>(source - kSrcCc);
        }
    }
}

// Adds up the routes every voice shares and moves their destinations: all of them after the
// lists were rebuilt, otherwise only the ones whose sum changed.
void evalGlobalRoutes(bool all) {
    if (globCount == 0)
        return;
    float sum[kNumModRoutes];
    std::fill(sum, sum + globDestCount, 0.0f);
    for (int n = 0; n < globCount; ++n)
        sum[globRoutes[n].slot] += routeSignal(globRoutes[n], leadSrc);
    for (int n = 0; n < globDestCount; ++n) {
        if (all || sum[n] != gOff[n]) {
            gOff[n] = sum[n];
            applyParam(globDest[n]);
            derivedDirty = true;
        }
    }
}

// Sorts the routes into the ones every voice shares, the per-voice ones, the audio-rate ones onto
// filter cutoffs and the unison-index ones, and notes which modulators anything listens to.
void classify() {
    for (int n = 0; n < polyDestCount; ++n) {
        polySlotOf[static_cast<std::size_t>(polyDest[n])] = -1;
        live[static_cast<std::size_t>(polyDest[n])].store(kNotModulated, std::memory_order_relaxed);
    }
    for (int n = 0; n < globDestCount; ++n) {
        globSlotOf[static_cast<std::size_t>(globDest[n])] = -1;
        gOff[n] = 0.0f;
        live[static_cast<std::size_t>(globDest[n])].store(kNotModulated, std::memory_order_relaxed);
        applyParam(globDest[n]);
    }
    polyCount = globCount = polyDestCount = globDestCount = 0;
    polyMask = 0;
    ccUsedCount = 0;
    followerUsed = false;
    for (int k = 0; k < kNumEnvs; ++k)
        env[k].used = k == 0;
    for (auto& l : lfo)
        l.used = l.audioUsed = false;
    for (auto& f : flux)
        f.used = false;
    for (bool& t : tapped)
        t = false;
    for (auto& o : osc)
        o.uniCount = 0;
    for (auto& f : filter)
        f.audioCount = 0;

    for (int k = 0; k < kNumLfos; ++k) {
        srcIsVoice[kSrcLfo + k] = !lfo[k].global;
        srcBipolar[kSrcLfo + k] = !lfo[k].p.unipolar;
    }
    for (int k = 0; k < kNumFlux; ++k)
        srcIsVoice[kSrcFlux + k] = !flux[k].global;
    for (int m = 0; m < kNumMacros; ++m)
        srcBipolar[kSrcMacro + m] = flag(reg.macroBipolar(m));

    const RouteTable* table = routes.current();
    if (table == nullptr)
        return;
    for (int i = 0; i < kNumModRoutes; ++i) {
        const RouteDef& d = table->route[i];
        if (!d.active)
            continue;
        if (isFloat[static_cast<std::size_t>(d.dest)] == 0 && defs[static_cast<std::size_t>(d.dest)] != &reg.at(d.dest))
            continue; // an effect's switch or choice in a rack slot is not moved by routes
        markUsed(d.source);
        if (d.via >= 0)
            markUsed(d.via);
        const float* drawn = d.drawn >= 0 ? table->curves[static_cast<std::size_t>(d.drawn)].data() : nullptr;

        const int f = d.dest == ids.filter[0].cutoff ? 0 : d.dest == ids.filter[1].cutoff ? 1 : -1;
        if (f >= 0 && isAudioRate(d.source) && filter[f].audioCount < kMaxAudioRoutes) {
            filter[f].audioRoute[filter[f].audioCount++] = static_cast<std::uint8_t>(i);
            if (d.source >= kSrcLfo && d.source < kSrcLfo + kNumLfos)
                lfo[d.source - kSrcLfo].audioUsed = true;
            continue;
        }
        if (d.source == kSrcUnisonIndex) {
            // The unison index differs between the lanes of one oscillator, so it can only move
            // what exists per lane.
            for (int o = 0; o < kOscCount; ++o) {
                const Ids::Osc& id = ids.osc[o];
                UniDest dest = UniDest::Position;
                float scale = 0.0f;
                if (d.dest == id.position)
                    scale = 1.0f;
                else if (d.dest == id.warpAmount[0])
                    dest = UniDest::WarpA, scale = 1.0f;
                else if (d.dest == id.warpAmount[1])
                    dest = UniDest::WarpB, scale = 1.0f;
                else if (d.dest == id.pan)
                    dest = UniDest::Pan, scale = 2.0f;
                else if (d.dest == id.fine)
                    dest = UniDest::Semis, scale = 2.0f;
                else if (d.dest == id.coarse)
                    dest = UniDest::Semis, scale = 128.0f;
                if (scale > 0.0f && osc[o].uniCount < kMaxUniTerms) {
                    const int n = osc[o].uniCount++;
                    osc[o].uniRoute[n] = static_cast<std::uint8_t>(i);
                    osc[o].uniDest[n] = dest;
                    osc[o].uniScale[n] = scale;
                }
            }
            continue;
        }

        ActiveRoute a;
        a.source = d.source;
        a.via = d.via;
        a.amount = static_cast<std::int16_t>(reg.modAmount(i));
        a.viaInvert = d.viaInvert;
        a.polarity = d.polarity;
        a.curve = d.curve;
        a.drawn = drawn;
        const auto dest = static_cast<std::size_t>(d.dest);
        const bool voiceEnd = (d.source < kNamedSources && srcIsVoice[d.source]) ||
                              (d.via >= 0 && d.via < kNamedSources && srcIsVoice[d.via]);
        if (voiceEnd && moduleOf[dest] != kModOther) {
            if (polySlotOf[dest] < 0) {
                polySlotOf[dest] = static_cast<std::int16_t>(polyDestCount);
                polyDest[polyDestCount++] = d.dest;
            }
            a.slot = polySlotOf[dest];
            polyMask |= 1u << moduleOf[dest];
            polyRoutes[polyCount++] = a;
        } else {
            if (globSlotOf[dest] < 0) {
                globSlotOf[dest] = static_cast<std::int16_t>(globDestCount);
                gOff[globDestCount] = 0.0f;
                globDest[globDestCount++] = d.dest;
            }
            a.slot = globSlotOf[dest];
            globRoutes[globCount++] = a;
        }
    }
    evalGlobalRoutes(true); // the same sums as before for routes that did not change: nothing jumps
}

// --- Derived values ----------------------------------------------------------------------------
// Each group is derived through a getter, so the same code serves the shared values (the getter
// reads pe) and one voice's own values (the getter is vval for that voice).

template <class Get>
void deriveOsc(int i, Get&& get, OscTargets& t) const {
    const Ids::Osc& id = ids.osc[i];
    t.tuneSemis = get(id.octave) * 12.0 + get(id.semi) + get(id.coarse) + get(id.fine) / 100.0;
    t.levelGain = dbToGain(get(id.level));
    t.pan = get(id.pan);
    t.in.position = static_cast<float>(get(id.position));
    t.in.amount[0] = static_cast<float>(get(id.warpAmount[0]));
    t.in.amount[1] = static_cast<float>(get(id.warpAmount[1]));
    t.in.spreadSemis = static_cast<float>(get(id.detune) * get(id.range));
    t.in.blend = static_cast<float>(get(id.blend));
    t.in.width = static_cast<float>(get(id.width));
    t.in.posSpread = static_cast<float>(get(id.positionSpread));
    t.in.warpSpread = static_cast<float>(get(id.warpSpread));
    t.shape = static_cast<float>(get(id.shape));
    t.pulseWidth = static_cast<float>(std::clamp(get(id.pulseWidth), 0.02, 0.98));
    const auto direct = static_cast<float>(get(id.direct)), toSecond = static_cast<float>(get(id.balance));
    t.send[0] = (1.0f - direct) * (1.0f - toSecond);
    t.send[1] = (1.0f - direct) * toSecond;
    t.send[2] = direct;
}

template <class Get>
void deriveSub(Get&& get, SubTargets& t) const {
    t.tuneSemis = get(ids.sub.octave) * 12.0 + get(ids.sub.coarse) + get(ids.sub.fine) / 100.0;
    t.levelGain = dbToGain(get(ids.sub.level));
    t.pan = get(ids.sub.pan);
    t.balance = static_cast<float>(get(ids.sub.balance));
}

template <class Get>
void deriveTexture(Get&& get, TextureTargets& t) const {
    t.tuneSemis = get(ids.texture.pitch) + get(ids.texture.fine) / 100.0;
    t.levelGain = dbToGain(get(ids.texture.level));
    t.pan = get(ids.texture.pan);
    t.balance = static_cast<float>(get(ids.texture.balance));
    t.start = static_cast<float>(get(ids.texture.start));
}

template <class Get>
void deriveFilter(int f, Get&& get, FilterTargets& t) const {
    const auto& id = ids.filter[f];
    t.cutoff = static_cast<float>(get(id.cutoff));
    t.resonance = static_cast<float>(get(id.resonance));
    t.drive = static_cast<float>(get(id.drive));
    t.character = static_cast<float>(get(id.character));
    t.stereoOffset = static_cast<float>(get(id.stereoOffset));
    t.mix = static_cast<float>(get(id.mix));
}

template <class Get>
void deriveEnv(int k, Get&& get, EnvelopeTimes& t) const {
    const auto& id = ids.env[k];
    const EnvShared& e = env[k];
    const double ms = sampleRate / 1000.0, beat = 60.0 / bpm * sampleRate;
    const auto time = [&](int segment, int index) {
        return e.syncBeats[segment] > 0.0 ? e.syncBeats[segment] * beat : get(index) * ms;
    };
    t.delay = time(0, id.delay);
    t.attack = time(1, id.attack);
    t.hold = time(2, id.hold);
    t.decay = time(3, id.decay);
    t.release = time(4, id.release);
    t.sustain = get(id.sustain);
    t.attackCurve = value(id.attackCurve);
    t.decayCurve = value(id.decayCurve);
    t.releaseCurve = value(id.releaseCurve);
}

template <class Get>
void deriveLfo(int k, Get&& get, LfoParams& out) const {
    const auto& id = ids.lfo[k];
    const LfoShared& l = lfo[k];
    const double ms = sampleRate / 1000.0;
    const double hz = l.sync ? bpm / (60.0 * l.beats) : get(id.rate);
    out.mode = l.mode;
    out.cyclesPerSample = std::min(hz, l.audioRate ? kMaxAudioLfoHz : kMaxLfoHz) / sampleRate;
    out.phaseOffset = get(id.phase) / 360.0;
    out.delaySamples = value(id.delay) * ms;
    out.riseSamples = value(id.rise) * ms;
    out.smooth = get(id.smooth);
    out.unipolar = l.p.unipolar;
}

// Everything the voices share, from the current pe. Cheap enough to run at every control tick
// while something moves; it does not run at all otherwise.
void derive() {
    const auto get = [this](int index) { return pe[static_cast<std::size_t>(index)]; };
    velocitySens = value(ids.velocitySens);
    referenceHz = value(ids.reference);
    bendUp = value(ids.bendUp);
    bendDown = value(ids.bendDown);
    mpeBendRange = value(ids.mpeBendRange);
    masterTuneSemis = value(ids.masterTune) / 100.0;
    masterTarget = dbToGain(value(ids.master));
    const double ms = sampleRate / 1000.0;
    followerAttack = static_cast<float>(1.0 - std::exp(-1.0 / std::max(value(ids.followerAttack) * ms, 1.0)));
    followerRelease = static_cast<float>(1.0 - std::exp(-1.0 / std::max(value(ids.followerRelease) * ms, 1.0)));

    for (int i = 0; i < kOscCount; ++i) {
        OscShared& o = osc[i];
        deriveOsc(i, get, o.t);
        o.startPhase = static_cast<float>(value(ids.osc[i].phase) / 360.0);
        o.phaseRandom = static_cast<float>(value(ids.osc[i].phaseRandom));
        if (o.lanesDirty || !(o.t.in == o.lanesFor)) {
            computeLaneTable(o, o.t.in, nullptr, 0, o.lanes);
            o.lanesFor = o.t.in;
            o.lanesDirty = false;
        }
    }
    deriveSub(get, sub.t);
    deriveTexture(get, texture.t);
    texture.randomStart = static_cast<float>(value(ids.texture.randomStart));
    for (int f = 0; f < kNumFilters; ++f) {
        deriveFilter(f, get, filter[f].t);
        filter[f].keytrack = static_cast<float>(value(ids.filter[f].keytrack));
    }
    for (int k = 0; k < kNumEnvs; ++k) {
        EnvShared& e = env[k];
        deriveEnv(k, get, e.t);
        // The multi-segment shape in samples at the current tempo.
        const EnvShapeData* shape = envShapes[k].current();
        e.def.count = 0;
        if (e.multi && shape != nullptr) {
            const double beat = 60.0 / bpm * sampleRate;
            e.def.count = shape->count;
            for (int n = 0; n < shape->count; ++n) {
                e.def.points[n].samples = shape->beats[n] > 0.0 ? shape->beats[n] * beat : shape->ms[n] * ms;
                e.def.points[n].level = shape->level[n];
                e.def.points[n].curve = shape->curve[n];
            }
            e.def.sustainPoint = shape->sustainPoint;
            e.def.loopStart = shape->loopStart;
            e.def.loopEnd = shape->loopEnd;
        }
    }
    for (int k = 0; k < kNumLfos; ++k) {
        deriveLfo(k, get, lfo[k].p);
        lfo[k].keytrack = static_cast<float>(value(ids.lfo[k].keytrack));
    }
    for (int k = 0; k < kNumFlux; ++k) {
        const double hz = flag(ids.flux[k].sync)
                              ? bpm / (60.0 * divisionBeats[static_cast<std::size_t>(choice(ids.flux[k].division))])
                              : value(ids.flux[k].rate);
        flux[k].cyclesPerSample = hz / sampleRate;
    }
}

// --- The control-rate step of everything the voices share ----------------------------------------

// Moves an LFO `advance` samples on and returns its output there. An LFO that a filter cutoff
// reads every sample also fills the end of `buffer` with the samples on the way.
float advanceLfo(LfoState& state, const LfoParams& params, const LfoShared& l, double advance, float* buffer) const {
    if (l.sync && l.anchor && playing && (l.mode == LfoMode::Free || l.mode == LfoMode::Retrigger)) {
        // Locked to the song: the phase is where the song position says, whatever happened before.
        const double cycles = ppqTarget / l.beats;
        state.phase = cycles - std::floor(cycles) - advance * params.cyclesPerSample;
    }
    if (l.audioUsed && advance >= 1.0) {
        const int samples = std::min(static_cast<int>(advance), kInterval);
        lfoRender(state, params, *l.table, buffer + kInterval - samples, samples);
        return buffer[kInterval - 1];
    }
    return lfoAdvance(state, params, *l.table, advance);
}

// `advance` is the distance to the moment the values are for: the next control tick.
void globalTick(double advance, bool snap) {
    if (movingCount > 0) {
        smoothParams();
        derivedDirty = true;
    }
    ppqTarget = ppqNow + (playing ? advance * bpm / (60.0 * sampleRate) : 0.0);

    // Controllers arrive in steps: a short glide takes the edges off.
    static constexpr int smoothed[] = {kSrcPitchBend, kSrcModWheel, kSrcAftertouch, kSrcExpression, kSrcBreath, kSrcSustain};
    for (const int index : smoothed) {
        if (snap)
            gsrc[index] = ctl[index];
        else
            approach(gsrc[index], ctl[index], controllerSmoothing);
    }
    for (int n = 0; n < ccUsedCount; ++n) {
        const int index = kSrcCc + ccUsed[n];
        if (snap)
            gsrc[index] = ctl[index];
        else
            approach(gsrc[index], ctl[index], controllerSmoothing);
    }
    for (int m = 0; m < kNumMacros; ++m) {
        const auto v = static_cast<float>(value(reg.macroValue(m)));
        gsrc[kSrcMacro + m] = srcBipolar[kSrcMacro + m] ? 2.0f * v - 1.0f : v;
    }
    gsrc[kSrcConstant] = 1.0f;
    const double bar = (ppqTarget - barStartPpq) / beatsPerBar;
    gsrc[kSrcBarPhase] = static_cast<float>(bar - std::floor(bar));
    gsrc[kSrcFollower] = std::min(followerLevel, 1.0f);

    for (int k = 0; k < kNumLfos; ++k) {
        LfoShared& l = lfo[k];
        if (!l.used)
            continue;
        l.freePhase += l.p.cyclesPerSample * advance;
        l.freePhase -= std::floor(l.freePhase);
        if (l.global) {
            l.value = advanceLfo(l.state, l.p, l, advance, l.buf);
            gsrc[kSrcLfo + k] = l.value;
        }
    }
    for (int k = 0; k < kNumFlux; ++k) {
        FluxShared& f = flux[k];
        if (f.used && f.global) {
            f.value = fluxAdvance(f.state, f.type, f.cyclesPerSample * advance);
            gsrc[kSrcFlux + k] = f.value;
        }
    }

    evalGlobalRoutes(false);
    if (derivedDirty) {
        derive();
        derivedDirty = false;
    }
}

// --- MIDI --------------------------------------------------------------------------------------

void handle(const MidiEvent& e) {
    actions.count = 0;
    const int channel = e.channel & 15;
    const bool perNote = mpe && channel != 0; // a member channel of the MPE zone
    switch (e.type) {
    case MidiEvent::Type::NoteOn:
        eventChannel = channel;
        keysHeldBefore = keysDown;
        if (!keyDown[e.number]) {
            keyDown[e.number] = true;
            ++keysDown;
        }
        polyPressure[e.number] = 0.0f;
        allocator.noteOn(e.number, e.value, actions);
        break;
    case MidiEvent::Type::NoteOff:
        if (keyDown[e.number]) {
            keyDown[e.number] = false;
            --keysDown;
        }
        eventChannel = channel;
        releaseVelocity = e.value;
        keysHeldBefore = keysDown; // a key still held takes over: that is a legato move
        allocator.noteOff(e.number, actions);
        break;
    case MidiEvent::Type::PitchBend: {
        const float bend = std::clamp(e.value, -1.0f, 1.0f);
        if (perNote)
            channelBend[channel] = bend;
        else
            ctl[kSrcPitchBend] = bend;
        break;
    }
    case MidiEvent::Type::Controller:
        if (e.number == 120 || e.number == 123) {
            std::fill(std::begin(keyDown), std::end(keyDown), false);
            keysDown = 0;
            if (e.number == 120)
                allocator.allSoundOff(actions);
            else
                allocator.allNotesOff(actions);
            break;
        }
        if (perNote && e.number == 74) {
            channelSlide[channel] = e.value;
            break;
        }
        ctl[kSrcCc + e.number] = e.value;
        {
            // A controller nobody listens to yet takes its value at once, so that a route added
            // later starts from where the controller is.
            bool listed = false;
            for (int n = 0; n < ccUsedCount; ++n)
                listed = listed || ccUsed[n] == e.number;
            if (!listed)
                gsrc[kSrcCc + e.number] = e.value;
        }
        if (e.number == 1)
            ctl[kSrcModWheel] = e.value;
        else if (e.number == 2)
            ctl[kSrcBreath] = e.value;
        else if (e.number == 11)
            ctl[kSrcExpression] = e.value;
        else if (e.number == 64) {
            ctl[kSrcSustain] = e.value >= 0.5f ? 1.0f : 0.0f;
            allocator.sustainPedal(e.value >= 0.5f, actions);
        }
        break;
    case MidiEvent::Type::ChannelPressure:
        if (perNote)
            channelPressure[channel] = e.value;
        else
            ctl[kSrcAftertouch] = e.value;
        break;
    case MidiEvent::Type::PolyPressure:
        polyPressure[e.number] = e.value;
        break;
    }
    applyActions();
}
