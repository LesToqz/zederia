    // --- Audio --------------------------------------------------------------------------------
    // Included inside Engine::Impl.

    static void mixToBuses(float bus[3][2][kInterval], const float* left, const float* right, int frames, SourceGain& gain,
                           SendRamp& send) {
        float gl = gain.now[0], gr = gain.now[1];
        float share[3] = {send.now[0], send.now[1], send.now[2]};
        for (int s = 0; s < frames; ++s) {
            const float l = left[s] * gl, r = right[s] * gr;
            gl += gain.step[0];
            gr += gain.step[1];
            for (int b = 0; b < 3; ++b) {
                bus[b][0][s] += l * share[b];
                bus[b][1][s] += r * share[b];
                share[b] += send.step[b];
            }
        }
        gain.now[0] = gl;
        gain.now[1] = gr;
        for (int b = 0; b < 3; ++b)
            send.now[b] = share[b];
    }

    // Keeps the end of a modulator's run for consumers that read it one base sample late.
    static void keepHistory(float* mod, int samples, int rate) {
        for (int k = 0; k < rate; ++k)
            mod[kHistory - rate + k] = mod[kHistory + samples - rate + k];
    }

    const float* modulatorFor(const Voice& v, int i, int slot) const {
        const OscShared& o = osc[i];
        if (!osc::warpUsesModulator(o.mode[slot]))
            return nullptr;
        const int source = o.modSource[slot];
        const bool sounding = source < kOscCount ? osc[source].on : source == kSub ? sub.on : texture.on;
        if (!sounding)
            return kZeros;
        return v.mod[source] + (routing.delayed[i][slot] ? kHistory - o.oversample : kHistory);
    }

    void renderOsc(Voice& v, int i, int frames, float bus[3][2][kInterval]) {
        OscShared& o = osc[i];
        OscVoice& ov = v.osc[i];
        const bool modulator = routing.usedAsModulator[i] || tapped[i];
        if (ov.gain.silent() && !modulator) { // inaudible and nobody listens: only keep time
            const int steps = frames * o.oversample;
            for (int l = 0; l < o.padded; ++l)
                ov.main.lanes.phase[l] += static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(static_cast<double>(ov.main.lanes.inc[l]) * 4294967296.0 * steps));
            ov.fadeLeft = std::max(ov.fadeLeft - frames, 0);
            return;
        }
        const int rate = o.oversample;
        const int samples = frames * rate;
        const bool stereo = o.count > 1;
        alignas(32) float left[kMaxRun], right[kMaxRun];
        float* mod = modulator ? v.mod[i] + kHistory : nullptr;

        osc::Block block;
        block.samples = samples;
        block.table = o.table->data();
        block.maxFrame = static_cast<float>(o.table->frames() - 1);
        block.levelFade = ov.main.levelFade;
        block.classic = o.classic;
        block.shape = ov.shape.now;
        block.pulseWidth = ov.pulseWidth.now;
        block.modeA = o.mode[0];
        block.modeB = o.mode[1];
        block.modA = modulatorFor(v, i, 0);
        block.modB = modulatorFor(v, i, 1);
        block.curveA = o.curve[0];
        block.curveB = o.curve[1];
        block.outL = left;
        block.outR = stereo ? right : nullptr;
        block.outM = mod;
        if (o.classic && (ov.shape.step != 0.0f || ov.pulseWidth.step != 0.0f)) {
            // The Classic engine takes its shape once per call. While the shape is moving it is
            // rendered a sample at a time, so the shape moves as smoothly as everything else.
            const float* modA = block.modA;
            const float* modB = block.modB;
            for (int at = 0; at < frames; ++at) {
                block.samples = rate;
                block.shape = ov.shape.now;
                block.pulseWidth = ov.pulseWidth.now;
                block.modA = modA != nullptr ? modA + at * rate : nullptr;
                block.modB = modB != nullptr ? modB + at * rate : nullptr;
                block.outL = left + at * rate;
                block.outR = stereo ? right + at * rate : nullptr;
                block.outM = mod != nullptr ? mod + at * rate : nullptr;
                o.render(ov.main.lanes, block);
                ov.shape.now += ov.shape.step;
                ov.pulseWidth.now += ov.pulseWidth.step;
            }
        } else {
            o.render(ov.main.lanes, block);
        }

        if (ov.fadeLeft > 0) {
            if (o.oldTable != nullptr && !o.classic) {
                // The previous table plays on from the same phases and fades out under the new one.
                alignas(32) float oldL[kMaxRun], oldR[kMaxRun], oldM[kMaxRun];
                block.table = o.oldTable->data();
                block.maxFrame = static_cast<float>(o.oldTable->frames() - 1);
                block.levelFade = ov.fade.levelFade;
                block.outL = oldL;
                block.outR = stereo ? oldR : nullptr;
                block.outM = modulator ? oldM : nullptr;
                o.render(ov.fade.lanes, block);
                float w = 1.0f - static_cast<float>(ov.fadeLeft) / kFade;
                const float step = 1.0f / (static_cast<float>(kFade) * static_cast<float>(rate));
                for (int s = 0; s < samples; ++s) {
                    const float mix = std::min(w, 1.0f);
                    left[s] = oldL[s] + mix * (left[s] - oldL[s]);
                    if (stereo)
                        right[s] = oldR[s] + mix * (right[s] - oldR[s]);
                    if (modulator)
                        mod[s] = oldM[s] + mix * (mod[s] - oldM[s]);
                    w += step;
                }
            }
            ov.fadeLeft = std::max(ov.fadeLeft - frames, 0);
        }
        if (modulator)
            keepHistory(v.mod[i], samples, rate);
        if (rate > 1) {
            dsp::decimate(*decimator, ov.decimatorL, left, frames, rate);
            if (stereo)
                dsp::decimate(*decimator, ov.decimatorR, right, frames, rate);
        }
        mixToBuses(bus, left, stereo ? right : left, frames, ov.gain, ov.send);
    }

    void renderSub(Voice& v, int frames, float bus[3][2][kInterval]) {
        SubVoice& sv = v.sub;
        const bool modulator = routing.usedAsModulator[kSub] || tapped[kSub];
        const int rate = sub.oversample;
        const int samples = frames * rate;
        float wave[kMaxRun];
        double phase = sv.phase, inc = sv.inc;
        for (int s = 0; s < samples; ++s) {
            wave[s] = subWave(sub.shape, static_cast<float>(phase), static_cast<float>(inc));
            phase += inc;
            if (phase >= 1.0)
                phase -= 1.0;
            inc += sv.incStep;
        }
        sv.phase = phase;
        sv.inc = inc;
        if (modulator) {
            std::copy(wave, wave + samples, v.mod[kSub] + kHistory);
            keepHistory(v.mod[kSub], samples, rate);
        }
        if (rate > 1)
            dsp::decimate(*decimator, sv.decimator, wave, frames, rate);
        mixToBuses(bus, wave, wave, frames, sv.gain, sv.send);
    }

    void playTexture(const SampleData& sample, TexturePlay& play, double rate, double rateStep, int samples, float* left,
                     float* right) const {
        const int frames = sample.frames;
        const bool loop = texture.loop;
        const int loopStart = loop && sample.hasLoop ? sample.loopStart : 0;
        const int loopEnd = loop && sample.hasLoop ? sample.loopEnd : frames;
        const double loopLength = loopEnd - loopStart;
        double position = play.position;
        for (int s = 0; s < samples; ++s) {
            if (play.finished || frames < 2) {
                left[s] = right[s] = 0.0f;
                continue;
            }
            const int i = static_cast<int>(position);
            const auto t = static_cast<float>(position - i);
            // Four-point interpolation through the neighbouring samples.
            const auto read = [&](const std::vector<float>& data) {
                const float a = sampleAt(data, i - 1, frames, loop, loopStart, loopEnd);
                const float b = sampleAt(data, i, frames, loop, loopStart, loopEnd);
                const float c = sampleAt(data, i + 1, frames, loop, loopStart, loopEnd);
                const float d = sampleAt(data, i + 2, frames, loop, loopStart, loopEnd);
                return b + 0.5f * t * (c - a + t * (2.0f * a - 5.0f * b + 4.0f * c - d + t * (3.0f * (b - c) + d - a)));
            };
            left[s] = read(sample.left);
            right[s] = read(sample.right);
            position += rate;
            rate += rateStep;
            if (loop) {
                if (position >= loopEnd && loopLength >= 2.0)
                    position -= loopLength * std::floor((position - loopStart) / loopLength);
            } else if (position >= frames - 1) {
                play.finished = true;
            }
        }
        play.position = position;
    }

    void renderTexture(Voice& v, int frames, float bus[3][2][kInterval]) {
        TextureVoice& tv = v.texture;
        const bool modulator = routing.usedAsModulator[kTexture] || tapped[kTexture];
        const int rate = texture.oversample;
        const int samples = frames * rate;
        float left[kMaxRun], right[kMaxRun];
        playTexture(*texture.sample, tv.main, tv.rate, tv.rateStep, samples, left, right);
        if (tv.fadeLeft > 0) {
            if (texture.oldSample != nullptr) {
                float oldL[kMaxRun], oldR[kMaxRun];
                const double oldRate = tv.rate * texture.oldSample->sampleRate / texture.sample->sampleRate;
                playTexture(*texture.oldSample, tv.fade, oldRate, 0.0, samples, oldL, oldR);
                float w = 1.0f - static_cast<float>(tv.fadeLeft) / kFade;
                const float step = 1.0f / (static_cast<float>(kFade) * static_cast<float>(rate));
                for (int s = 0; s < samples; ++s) {
                    const float mix = std::min(w, 1.0f);
                    left[s] = oldL[s] + mix * (left[s] - oldL[s]);
                    right[s] = oldR[s] + mix * (right[s] - oldR[s]);
                    w += step;
                }
            }
            tv.fadeLeft = std::max(tv.fadeLeft - frames, 0);
        }
        tv.rate += tv.rateStep * samples;
        if (modulator) {
            float* mod = v.mod[kTexture] + kHistory;
            for (int s = 0; s < samples; ++s)
                mod[s] = 0.5f * (left[s] + right[s]);
            keepHistory(v.mod[kTexture], samples, rate);
        }
        if (rate > 1) {
            dsp::decimate(*decimator, tv.decimatorL, left, frames, rate);
            dsp::decimate(*decimator, tv.decimatorR, right, frames, rate);
        }
        mixToBuses(bus, left, right, frames, tv.gain, tv.send);
    }

    // Warmth (plan 3.6): a gentle saturation of what goes into the filters, with the top tilted
    // down a little. The curve is a cubic that meets its ceiling with zero slope, so it adds
    // third harmonics and little else.
    void warm(Voice& v, float bus[3][2][kInterval], int frames) {
        const float tilt = warmTilt;
        float end = v.warmth.now;
        for (int b = 0; b < 2; ++b) {
            for (int c = 0; c < 2; ++c) {
                float amount = v.warmth.now, low = v.warmState[b][c];
                float* x = bus[b][c];
                for (int s = 0; s < frames; ++s) {
                    const float gain = 1.0f + 3.0f * amount;
                    const float u = std::clamp(x[s] * gain, -1.5f, 1.5f);
                    const float shaped = (u - u * u * u * (4.0f / 27.0f)) / gain;
                    low += (shaped - low) * tilt;
                    x[s] = shaped - 0.35f * amount * (shaped - low);
                    amount += v.warmth.step;
                }
                v.warmState[b][c] = low;
                end = amount;
            }
        }
        v.warmth.now = end;
    }

    // Noise floor (plan 3.6): a low bed of white noise into the first filter, different on each side.
    static void addNoise(Voice& v, float bus[3][2][kInterval], int frames) {
        std::uint32_t state = v.noiseState;
        float gain = v.noise.now;
        for (int s = 0; s < frames; ++s) {
            for (int c = 0; c < 2; ++c) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                bus[0][c][s] += gain * (static_cast<float>(state >> 8) * (2.0f / 16777216.0f) - 1.0f);
            }
            gain += v.noise.step;
        }
        v.noiseState = state;
        v.noise.now = gain;
    }

    // The audio-rate movement of a filter's cutoff, in octaves per sample; null when no route asks
    // for one. `tickPos` is how far into the control interval the run starts.
    const float* cutoffMod(Voice& v, int f, int tickPos, int frames, float* out) {
        const FilterShared& fs = filter[f];
        if (fs.audioCount == 0)
            return nullptr;
        std::fill(out, out + frames, 0.0f);
        const RouteTable* table = routes.current();
        for (int n = 0; n < fs.audioCount; ++n) {
            const RouteDef& d = table->route[fs.audioRoute[n]];
            const float* drawn = d.drawn >= 0 ? table->curves[static_cast<std::size_t>(d.drawn)].data() : nullptr;
            const float* signal = kZeros;
            int stride = 0;
            if (isAudioTap(d.source)) {
                const int j = d.source - kSrcAudio;
                const bool sounding = j < kOscCount ? osc[j].on : j == kSub ? sub.on : texture.on;
                if (sounding) {
                    signal = v.mod[j] + kHistory;
                    stride = j < kOscCount ? osc[j].oversample : j == kSub ? sub.oversample : texture.oversample;
                }
            } else {
                const int k = d.source - kSrcLfo;
                signal = (lfo[k].global ? lfo[k].buf : v.lfoBuf[k]) + tickPos;
                stride = 1;
            }
            Ramp1& depth = v.filter[f].depth[n];
            float amount = depth.now;
            const bool bipolar = srcBipolar[d.source];
            for (int s = 0; s < frames; ++s) {
                out[s] += amount * modShape(signal[s * stride], bipolar, d.polarity, d.curve, drawn);
                amount += depth.step;
            }
            depth.now = amount;
        }
        return out;
    }

    // Adds the voice to the engine's buses: filter 1's parallel share, filter 2 and direct apart
    // when `split`, all into out[0] otherwise.
    void renderVoice(Voice& v, float (&out)[3][2][kInterval], int frames, bool split) {
        const int tickPos = kInterval - samplesToTick;
        int done = 0;
        while (done < frames) {
            // A modulation loop needs each source's previous sample: go one sample at a time.
            const int run = routing.loop ? 1 : frames - done;
            float bus[3][2][kInterval] = {}; // to filter 1, to filter 2, direct
            const auto sourcesStart = profNow();
            for (const int source : routing.order) {
                if (source < kOscCount) {
                    if (osc[source].on)
                        renderOsc(v, source, run, bus);
                } else if (source == kSub) {
                    if (sub.on)
                        renderSub(v, run, bus);
                } else if (texture.on) {
                    renderTexture(v, run, bus);
                }
            }
            if (v.warmth.now != 0.0f || v.warmth.target != 0.0f)
                warm(v, bus, run);
            if (v.noise.now != 0.0f || v.noise.target != 0.0f)
                addNoise(v, bus, run);
            profAdd(kProfSources, sourcesStart);
            const auto filtersStart = profNow();

            // The two filter slots: in series at 0 %, in parallel at 100 % (plan 3.5).
            float movement[kInterval];
            if (v.filter[0].running)
                v.filter[0].filter.process(bus[0][0], bus[0][1], run, cutoffMod(v, 0, tickPos + done, run, movement), *decimator);
            const float routingStart = v.routing.now;
            for (int c = 0; c < 2; ++c) {
                float parallel = routingStart;
                for (int s = 0; s < run; ++s) {
                    bus[1][c][s] += (1.0f - parallel) * bus[0][c][s];
                    parallel += v.routing.step;
                }
            }
            if (v.filter[1].running)
                v.filter[1].filter.process(bus[1][0], bus[1][1], run, cutoffMod(v, 1, tickPos + done, run, movement), *decimator);
            float ampL = v.amp[0], ampR = v.amp[1], parallel = routingStart;
            if (split) {
                for (int s = 0; s < run; ++s) {
                    out[0][0][done + s] += parallel * bus[0][0][s] * ampL;
                    out[0][1][done + s] += parallel * bus[0][1][s] * ampR;
                    out[1][0][done + s] += bus[1][0][s] * ampL;
                    out[1][1][done + s] += bus[1][1][s] * ampR;
                    out[2][0][done + s] += bus[2][0][s] * ampL;
                    out[2][1][done + s] += bus[2][1][s] * ampR;
                    ampL += v.ampStep[0];
                    ampR += v.ampStep[1];
                    parallel += v.routing.step;
                }
            } else {
                for (int s = 0; s < run; ++s) {
                    out[0][0][done + s] += (parallel * bus[0][0][s] + bus[1][0][s] + bus[2][0][s]) * ampL;
                    out[0][1][done + s] += (parallel * bus[0][1][s] + bus[1][1][s] + bus[2][1][s]) * ampR;
                    ampL += v.ampStep[0];
                    ampR += v.ampStep[1];
                    parallel += v.routing.step;
                }
            }
            v.amp[0] = ampL;
            v.amp[1] = ampR;
            v.routing.now = parallel;
            profAdd(kProfFilters, filtersStart);
            done += run;
        }
    }

    void render(float* left, float* right, int frames) {
        float out[3][2][kInterval];
        const bool split = rackSplit;
        for (int b = 0; b < (split ? 3 : 1); ++b) {
            std::fill(out[b][0], out[b][0] + frames, 0.0f);
            std::fill(out[b][1], out[b][1] + frames, 0.0f);
        }
        if (activeCount > 0)
            for (auto& v : voices)
                if (v.active)
                    renderVoice(v, out, frames, split);
        if (duckState != 0) {
            // A change of quality: the voices fade out, the rates switch while they are silent, and
            // after a short wait for the new filters to settle they fade back in.
            constexpr float step = 1.0f / 256.0f;
            const float target = duckState == 1 ? 0.0f : 1.0f;
            float duck = qualityDuck;
            for (int s = 0; s < frames; ++s) {
                duck = duck < target ? std::min(duck + step, target) : std::max(duck - step, target);
                const float g = std::max(duck, 0.0f);
                for (int b = 0; b < (split ? 3 : 1); ++b) {
                    out[b][0][s] *= g;
                    out[b][1][s] *= g;
                }
            }
            qualityDuck = duck;
        }
        if (followerUsed) {
            // The envelope follower hears the voices before the effects, or the side chain.
            float level = followerLevel;
            for (int i = 0; i < frames; ++i) {
                float x;
                if (followerSidechain) {
                    x = std::max(sidechain[0] != nullptr ? std::abs(sidechain[0][i]) : 0.0f,
                                 sidechain[1] != nullptr ? std::abs(sidechain[1][i]) : 0.0f);
                } else {
                    float l = out[0][0][i], r = out[0][1][i];
                    if (split) {
                        l += out[1][0][i] + out[2][0][i];
                        r += out[1][1][i] + out[2][1][i];
                    }
                    x = std::max(std::abs(l), std::abs(r));
                }
                level += (x - level) * (x > level ? followerAttack : followerRelease);
            }
            followerLevel = level;
        }
        runRack(out, split, left, right, frames);
        outputStage(left, right, frames);
        if (duckState == 1 && (qualityDuck <= 0.0f || activeCount == 0)) {
            appliedQuality = pendingQuality >= 0 ? pendingQuality : appliedQuality;
            pendingQuality = -1;
            duckState = 2;
            qualityDuck = -0.25f; // 64 samples of silence while the new oversampling filters settle
            refresh();           // the voices switch rates now, while they are silent
        } else if (duckState == 2 && qualityDuck >= 1.0f) {
            duckState = 0;
        }
        for (auto& o : osc)
            o.fadeLeft = std::max(o.fadeLeft - frames, 0);
        texture.fadeLeft = std::max(texture.fadeLeft - frames, 0);
    }
