// The effects rack (plan 3.8) and the output stage. Included inside Engine::Impl.
//
// Layouts arrive as RackPlans. A plan names the effect instance at every position; an instance
// that continues keeps its state. Edits are click-free because an effect at zero presence is
// transparent (its slot passes its input unchanged):
//   - a new effect enters at zero presence and fades in;
//   - a removed effect fades out, then leaves;
//   - an effect whose input would change (it moves, or what runs before it changes) fades out,
//     moves while it is transparent, and fades back in. Everything else keeps running untouched.
// While a plan waits for its fades, the effects keep their parameter values: the store may
// already hold the new layout's values at the old positions.

const ParamDef& defOf(int index) const { return *defs[static_cast<std::size_t>(index)]; }

int poolIndexOf(std::uint32_t uid) const {
    for (int i = 0; i < kFxPool; ++i)
        if (pool[i].effect != nullptr && pool[i].uid == uid)
            return i;
    return -1;
}

int freePoolEntry() const {
    for (int i = 0; i < kFxPool; ++i)
        if (pool[i].effect == nullptr)
            return i;
    return -1;
}

bool rackFrozen() const {
    return staged != nullptr || bSwitching || rackPending.load(std::memory_order_relaxed) != nullptr;
}

void retireEffect(fx::Effect* effect) {
    if (effect != nullptr && !fxBin.put(effect) && fxHeldCount < kFxPool)
        fxHeld[fxHeldCount++] = effect;
}

void retryRetire() {
    for (int i = 0; i < fxHeldCount;) {
        if (fxBin.put(fxHeld[i]))
            fxHeld[i] = fxHeld[--fxHeldCount];
        else
            ++i;
    }
    if (planHeld != nullptr && planBin.put(planHeld))
        planHeld = nullptr;
}

bool movesFaded() const {
    for (const FxInstance& x : pool)
        if (x.effect != nullptr && (x.moving || x.leaving) && x.g > 0.0f)
            return false;
    return true;
}

bool laneSilentPresence(int lane) const {
    for (int n = 0; n < chainCount[lane]; ++n)
        if (pool[chain[lane][n]].g > 0.0f)
            return false;
    return true;
}

// At the start of a block, before the parameter store is drained.
void rackBlockStart() {
    retryRetire();
    if (bSwitching && laneSilentPresence(1))
        finishBSwitch();
    if (staged != nullptr && movesFaded())
        applyStaged();
    if (staged == nullptr && !bSwitching && planHeld == nullptr)
        if (RackPlan* plan = rackPending.exchange(nullptr, std::memory_order_acq_rel))
            stagePlan(plan);
}

void stagePlan(RackPlan* plan) {
    staged = plan;
    bool inPlan[kFxPool] = {};
    for (int k = 0; k < kRackSlots; ++k) {
        if (plan->uid[k] == 0 || plan->fresh[k] != nullptr)
            continue;
        const int e = poolIndexOf(plan->uid[k]);
        if (e >= 0)
            inPlan[e] = true;
    }
    // In each lane, the continuing effects that keep their order relative to each other stay;
    // the others have to move.
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        std::uint32_t before[kNumFxSlots], after[kNumFxSlots];
        int countBefore = 0, countAfter = 0, entry[kNumFxSlots];
        for (int n = 0; n < chainCount[lane]; ++n) {
            const int e = chain[lane][n];
            if (inPlan[e]) {
                entry[countBefore] = e;
                before[countBefore++] = pool[e].uid;
            }
        }
        for (int pos = 0; pos < plan->count[lane]; ++pos) {
            const int k = lane * kNumFxSlots + pos;
            if (plan->uid[k] != 0 && plan->fresh[k] == nullptr)
                after[countAfter++] = plan->uid[k];
        }
        bool keep[kNumFxSlots];
        commonOrder(before, countBefore, after, countAfter, keep);
        for (int n = 0; n < countBefore; ++n)
            if (!keep[n])
                pool[entry[n]].moving = true;
    }
    bool fadeFirst = false;
    for (int e = 0; e < kFxPool; ++e) {
        FxInstance& x = pool[e];
        if (x.effect == nullptr)
            continue;
        if (!inPlan[e])
            x.leaving = true;
        if (x.leaving || x.moving) {
            x.gTarget = 0.0f;
            fadeFirst = fadeFirst || x.g > 0.0f;
        }
    }
    if (!fadeFirst)
        applyStaged();
}

void applyStaged() {
    RackPlan& plan = *staged;
    bool inPlan[kFxPool] = {};
    int next[kNumFxLanes][kNumFxSlots];
    int count[kNumFxLanes] = {};
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        for (int pos = 0; pos < plan.count[lane]; ++pos) {
            const int k = lane * kNumFxSlots + pos;
            int e = -1;
            if (plan.fresh[k] != nullptr) {
                e = freePoolEntry();
                if (e < 0)
                    continue; // cannot happen: the pool holds twice the rack
                pool[e] = FxInstance{};
                pool[e].effect = plan.fresh[k].release();
                pool[e].uid = plan.uid[k];
                pool[e].type = plan.type[k];
            } else {
                e = poolIndexOf(plan.uid[k]);
            }
            if (e < 0)
                continue;
            inPlan[e] = true;
            FxInstance& x = pool[e];
            x.lane = lane;
            x.pos = count[lane];
            x.params = static_cast<int>(reg.effectTypes()[static_cast<std::size_t>(x.type)].params.size());
            x.moving = x.leaving = false;
            x.gTarget = plan.values[k][0] >= 0.5 ? 1.0f : 0.0f;
            if (snapSmoothing)
                x.g = x.gTarget; // the first block after a reset: the rack is in place, not fading in
            x.mix.aim(static_cast<float>(plan.values[k][1]), true);
            x.needsSet = x.snap = true;
            x.effect->setCurves(plan.hasCurve[k][0] ? plan.curves[k][0] : nullptr,
                                plan.hasCurve[k][1] ? plan.curves[k][1] : nullptr);
            next[lane][count[lane]++] = e;
        }
    }
    for (int e = 0; e < kFxPool; ++e) {
        if (pool[e].effect != nullptr && !inPlan[e]) {
            retireEffect(pool[e].effect);
            pool[e] = FxInstance{};
        }
    }
    // The positional parameters now belong to the new layout: their definitions and values.
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        chainCount[lane] = count[lane];
        for (int pos = 0; pos < kNumFxSlots; ++pos) {
            chain[lane][pos] = pos < count[lane] ? next[lane][pos] : -1;
            const int k = lane * kNumFxSlots + pos;
            const int type = pos < count[lane] ? pool[next[lane][pos]].type : -1;
            const auto* params = type >= 0 ? &reg.effectTypes()[static_cast<std::size_t>(type)].params : nullptr;
            const int base = reg.fxSlotOn(lane, pos);
            for (int f = 0; f < kSlotFields; ++f) {
                const int index = base + f;
                const auto i = static_cast<std::size_t>(index);
                const ParamDef* d = &reg.at(index);
                if (params != nullptr && f >= 2 && f - 2 < static_cast<int>(params->size()))
                    d = &(*params)[static_cast<std::size_t>(f - 2)];
                defs[i] = d;
                isFloat[i] = d->type == ParamType::Float ? 1 : 0;
                const double v = plan.values[k][f];
                p[i] = pe[i] = v;
                if (isFloat[i] != 0)
                    nb[i] = nt[i] = d->toNormalized(v);
                moving[i] = 0;
                if (globSlotOf[i] >= 0)
                    applyParam(index);
            }
        }
    }
    dirty = true;
    planHeld = staged;
    staged = nullptr;
    retryRetire();
}

// Lane B between parallel and series: its effects fade out, the routing switches while they are
// transparent (then both routings give the same output), and they fade back in.
void requestBRouting(int wanted) {
    bRoutingWanted = wanted;
    if (bSwitching || wanted == bRouting)
        return;
    if (laneSilentPresence(1)) {
        bRouting = wanted;
        return;
    }
    bSwitching = true;
    for (int n = 0; n < chainCount[1]; ++n) {
        pool[chain[1][n]].moving = true;
        pool[chain[1][n]].gTarget = 0.0f;
    }
}

void finishBSwitch() {
    bRouting = bRoutingWanted;
    bSwitching = false;
    for (int n = 0; n < chainCount[1]; ++n) {
        FxInstance& x = pool[chain[1][n]];
        x.moving = false;
        x.gTarget = flag(reg.fxSlotOn(1, x.pos)) ? 1.0f : 0.0f;
    }
}

// Hands an instance its parameter values (in its own units).
void syncInstance(FxInstance& x) {
    const int base = reg.fxSlotParam(x.lane, x.pos, 0);
    for (int k = 0; k < x.params; ++k)
        x.cached[k] = pe[static_cast<std::size_t>(base + k)];
    x.effect->set(x.cached, fxCtx, x.snap);
    x.needsSet = x.snap = false;
}

// At each control tick: switches, mixes and parameter values for the next interval.
void rackTick() {
    const fx::Context ctx{sampleRate, bpm, oversample, oversample == 1, decimator};
    const bool ctxChanged = !(ctx == fxCtx);
    fxCtx = ctx;
    const bool frozen = rackFrozen();
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        for (int n = 0; n < chainCount[lane]; ++n) {
            FxInstance& x = pool[chain[lane][n]];
            idleCheck(x);
            if (frozen || x.moving || x.leaving)
                continue;
            const int on = reg.fxSlotOn(lane, x.pos);
            const float target = flag(on) ? 1.0f : 0.0f;
            if (target > 0.0f && x.gTarget <= 0.0f && x.g <= 0.0f)
                x.needsClear = true; // switched back on: it starts clean, without an old tail
            x.gTarget = target;
            if (x.idle) // asleep: its values are read again when it wakes
                continue;
            x.mix.aim(static_cast<float>(pe[static_cast<std::size_t>(on + 1)]), false);
            bool changed = x.needsSet || ctxChanged;
            for (int k = 0; k < x.params; ++k) {
                const double v = pe[static_cast<std::size_t>(on + 2 + k)];
                if (v != x.cached[k]) {
                    x.cached[k] = v;
                    changed = true;
                }
            }
            if (!changed)
                continue;
            if (x.g > 0.0f || x.gTarget > 0.0f) {
                x.effect->set(x.cached, fxCtx, x.snap);
                x.needsSet = x.snap = false;
            } else {
                x.needsSet = true;
            }
        }
    }
    const double balanceTarget[3] = {value(ids.fxBalance[0]), value(ids.fxBalance[1]), value(ids.fxBalance[2])};
    for (int b = 0; b < 3; ++b)
        sendRamp[b].aim(static_cast<float>(balanceTarget[b]), false);
    rackSplit = !(sendRamp[0].now == sendRamp[1].now && sendRamp[1].now == sendRamp[2].now &&
                  sendRamp[0].target == sendRamp[1].target && sendRamp[1].target == sendRamp[2].target);
}

void rackNoteOn() {
    for (int lane = 0; lane < kNumFxLanes; ++lane)
        for (int n = 0; n < chainCount[lane]; ++n)
            pool[chain[lane][n]].effect->noteOn();
}

// One effect on one run, in place. Idling is decided at control ticks (on the fixed grid), and an
// idle effect wakes at the exact sample where its input becomes audible: neither depends on the
// host's block size, so renders stay identical for any block size.
void runInstance(FxInstance& x, float* left, float* right, int frames) {
    if (x.g <= 0.0f && x.gTarget <= 0.0f) {
        if (!x.moving && !x.leaving)
            x.needsClear = true; // switched off: it starts clean when it comes back
        x.ran = false;
        return;
    }
    int start = 0;
    if (x.idle) {
        while (start < frames && std::abs(left[start]) <= fx::kSilent && std::abs(right[start]) <= fx::kSilent)
            ++start;
        if (start == frames && x.g >= x.gTarget) {
            x.ran = false;
            return;
        }
        x.idle = false;
        x.silent = 0;
        x.peakIn = x.peakOut = 0.0f;
        x.needsSet = true; // values may have moved while it slept
        x.mix.aim(static_cast<float>(pe[static_cast<std::size_t>(reg.fxSlotMix(x.lane, x.pos))]), false);
    }
    if (x.needsClear) {
        x.effect->clear();
        x.needsClear = false;
        x.needsSet = x.snap = true;
    }
    if (x.needsSet)
        syncInstance(x);
    left += start;
    right += start;
    frames -= start;
    float wetL[kInterval], wetR[kInterval], presence[kInterval];
    float g = x.g, peakIn = x.peakIn;
    for (int s = 0; s < frames; ++s) {
        g = g < x.gTarget ? std::min(g + fxFadeStep, x.gTarget) : std::max(g - fxFadeStep, x.gTarget);
        presence[s] = g;
        peakIn = std::max(peakIn, std::max(std::abs(left[s]), std::abs(right[s])));
        wetL[s] = left[s] * g;
        wetR[s] = right[s] * g;
    }
    x.g = g;
    x.peakIn = peakIn;
    const auto fxStart = profNow();
    x.effect->process(wetL, wetR, frames);
    profAdd(kProfFx + std::clamp(x.type, 0, 12), fxStart);
    float peakOut = x.peakOut;
    for (int s = 0; s < frames; ++s) {
        const float m = x.mix.next() * presence[s];
        peakOut = std::max(peakOut, std::max(std::abs(wetL[s]), std::abs(wetR[s])));
        left[s] += m * (wetL[s] - left[s]);
        right[s] += m * (wetR[s] - right[s]);
    }
    x.peakOut = peakOut;
    x.ran = true;
}

// At a control tick: an effect whose input and output stayed silent for longer than it can hold
// sound goes idle and costs nothing until its input returns.
void idleCheck(FxInstance& x) {
    if (x.idle || !x.ran)
        return;
    if (x.peakIn <= fx::kSilent && x.peakOut <= fx::kSilent && x.effect->mayIdle() && x.g >= x.gTarget) {
        x.silent += kInterval;
        if (x.silent > x.effect->holdSamples() + kInterval) {
            x.idle = true;
            x.ran = false;
            x.silent = 0;
        }
    } else {
        x.silent = 0;
    }
    x.peakIn = x.peakOut = 0.0f;
}

// True when every effect is idle or switched off and no edit is under way.
bool rackAsleep() const {
    if (staged != nullptr || bSwitching || fxMix != fxMixTarget)
        return false;
    for (int lane = 0; lane < kNumFxLanes; ++lane)
        for (int n = 0; n < chainCount[lane]; ++n) {
            const FxInstance& x = pool[chain[lane][n]];
            if (!(x.idle || (x.g <= 0.0f && x.gTarget <= 0.0f)) || x.g != x.gTarget)
                return false;
        }
    return true;
}

void runLane(int lane, float* left, float* right, int frames) {
    for (int n = 0; n < chainCount[lane]; ++n)
        runInstance(pool[chain[lane][n]], left, right, frames);
}

// The voices' buses through the rack. bus[0..2]: filter 1's parallel share, filter 2, direct (or
// bus[0] alone holding their sum when the three are sent alike).
void runRack(float (&bus)[3][2][kInterval], bool split, float* left, float* right, int frames) {
    float dryL[kInterval], dryR[kInterval];
    for (int s = 0; s < frames; ++s) {
        dryL[s] = split ? bus[0][0][s] + bus[1][0][s] + bus[2][0][s] : bus[0][0][s];
        dryR[s] = split ? bus[0][1][s] + bus[1][1][s] + bus[2][1][s] : bus[0][1][s];
    }
    const bool rackEmpty = chainCount[0] == 0 && chainCount[1] == 0 && chainCount[2] == 0;
    const bool fxGone = fxMix <= 0.0f && fxMixTarget <= 0.0f;
    if (rackEmpty || fxGone || (activeCount == 0 && rackAsleep())) {
        outputSilent = activeCount == 0; // no voice and nothing ringing: the output is exact silence
        // Nothing to run: no effects, FX off, or no voice and every effect asleep (its input is
        // silence, so each would pass it unchanged).
        if (fxGone && !fxCleared) {
            for (FxInstance& x : pool)
                if (x.effect != nullptr) {
                    x.needsClear = true; // silent and transparent: they fade back in when FX return
                    x.g = 0.0f;
                }
            fxCleared = true;
        }
        for (int b = 0; b < 3; ++b)
            for (int s = 0; s < frames; ++s)
                sendRamp[b].next();
        std::copy(dryL, dryL + frames, left);
        std::copy(dryR, dryR + frames, right);
        return;
    }
    fxCleared = false;
    outputSilent = false;
    float aL[kInterval], aR[kInterval], bL[kInterval], bR[kInterval];
    for (int s = 0; s < frames; ++s) {
        const float b0 = sendRamp[0].next(), b1 = sendRamp[1].next(), b2 = sendRamp[2].next();
        if (split) {
            bL[s] = b0 * bus[0][0][s] + b1 * bus[1][0][s] + b2 * bus[2][0][s];
            bR[s] = b0 * bus[0][1][s] + b1 * bus[1][1][s] + b2 * bus[2][1][s];
        } else {
            bL[s] = b0 * dryL[s];
            bR[s] = b0 * dryR[s];
        }
        aL[s] = dryL[s] - bL[s];
        aR[s] = dryR[s] - bR[s];
    }
    runLane(0, aL, aR, frames);
    if (bRouting == 1) { // series: lane B after lane A
        for (int s = 0; s < frames; ++s) {
            bL[s] += aL[s];
            bR[s] += aR[s];
        }
        runLane(1, bL, bR, frames);
        std::copy(bL, bL + frames, left);
        std::copy(bR, bR + frames, right);
    } else {
        runLane(1, bL, bR, frames);
        for (int s = 0; s < frames; ++s) {
            left[s] = aL[s] + bL[s];
            right[s] = aR[s] + bR[s];
        }
    }
    runLane(2, left, right, frames);
    if (fxMix < 1.0f || fxMixTarget < 1.0f) { // FX off fades between the rack and the dry sum
        float w = fxMix;
        for (int s = 0; s < frames; ++s) {
            w = w < fxMixTarget ? std::min(w + fxFadeStep, fxMixTarget) : std::max(w - fxFadeStep, fxMixTarget);
            left[s] = dryL[s] + w * (left[s] - dryL[s]);
            right[s] = dryR[s] + w * (right[s] - dryR[s]);
        }
        fxMix = w;
    }
}

// After the rack: master volume, DC filter, output protection.
void outputStage(float* left, float* right, int frames) {
    const auto outputStart = profNow();
    struct Done {
        Impl& s;
        ProfClock::time_point t;
        ~Done() { s.profAdd(kProfOutput, t); }
    } done{*this, outputStart};
    if (outputSilent && dcX[0] == 0.0f && dcX[1] == 0.0f && dcY[0] == 0.0f && dcY[1] == 0.0f &&
        (protection == 0 || (protection == 2 && outLimiter.env == 0.0f))) {
        for (int s = 0; s < frames; ++s) // silence stays silence: only the master volume moves on
            master += masterStep;
        return;
    }
    for (int s = 0; s < frames; ++s) {
        const auto gain = static_cast<float>(master);
        left[s] *= gain;
        right[s] *= gain;
        master += masterStep;
    }
    if (dcOn) {
        float* io[2] = {left, right};
        for (int c = 0; c < 2; ++c) {
            float x1 = dcX[c], y1 = dcY[c];
            for (int s = 0; s < frames; ++s) {
                const float x = io[c][s];
                float y = x - x1 + dcCoef * y1;
                if (x == 0.0f && x1 == 0.0f && std::abs(y) < 1.0e-5f)
                    y = 0.0f; // the tail of a removed offset ends in true silence (below -100 dB)
                x1 = x;
                y1 = y;
                io[c][s] = y;
            }
            dcX[c] = x1;
            dcY[c] = y1;
        }
    }
    if (protection == 1) {
        float* io[2] = {left, right};
        for (int c = 0; c < 2; ++c) {
            if (clipFactor <= 1) {
                for (int s = 0; s < frames; ++s)
                    io[c][s] = softClip(io[c][s]);
                continue;
            }
            float up[kInterval * 8], scratch[kInterval * 8];
            dsp::interpolate(*clipDesign, clipUp[c], io[c], up, scratch, frames, clipFactor);
            for (int s = 0; s < frames * clipFactor; ++s)
                up[s] = softClip(up[s]);
            dsp::decimate(*clipDesign, clipDown[c], up, frames, clipFactor);
            for (int s = 0; s < frames; ++s)
                io[c][s] = std::clamp(up[s], -1.0f, 1.0f);
        }
    } else if (protection == 2) {
        outLimiter.process(left, right, frames, kLimiterCeiling);
    }
}
