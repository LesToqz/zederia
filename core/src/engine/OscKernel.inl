// The oscillator kernel, written once against the SIMD wrapper. Each including file defines
// ZED_KERNEL_SIMD (SimdScalar, SimdSse2 or SimdAvx2) and gets its own copy in an unnamed
// namespace, so code compiled for AVX2 can never be picked up by a processor without it.
//
// Every arithmetic step is the same in all three copies and no step depends on how many lanes
// run together. The lanes are added up in a fixed tree (lane i with lane i + 8, then 4 apart,
// then 2, then 1), which each copy reproduces, so the results are bit-identical.

namespace {

using S = ZED_KERNEL_SIMD;
using F = S::F;
using I = S::I;
constexpr int W = S::width;
constexpr int kAccumulators = 8 / W;

inline F k(float v) { return S::set1(v); }

inline F floorF(F x) {
    const F t = S::toFloat(S::toInt(x));
    return S::sub(t, S::select(S::lt(x, t), k(1.0f), k(0.0f)));
}
inline F fracF(F x) { return S::sub(x, floorF(x)); }

// sin(2 pi t) for any t, from an odd polynomial on a quarter turn (error below 1e-7).
inline F sinTurns(F t) {
    const F u = S::sub(t, floorF(S::add(t, k(0.5f)))); // -0.5 .. 0.5
    const F a = S::abs(u);
    const F folded = S::select(S::lt(k(0.25f), a), S::sub(k(0.5f), a), a); // 0 .. 0.25
    const F z = S::mul(folded, k(6.28318530717958647692f));
    const F z2 = S::mul(z, z);
    F poly = k(-2.50521083854417187751e-8f);                // -1/11!
    poly = S::add(S::mul(poly, z2), k(2.75573192239858906526e-6f));  // 1/9!
    poly = S::add(S::mul(poly, z2), k(-1.98412698412698412698e-4f)); // -1/7!
    poly = S::add(S::mul(poly, z2), k(8.33333333333333333333e-3f));  // 1/5!
    poly = S::add(S::mul(poly, z2), k(-1.66666666666666666667e-1f)); // -1/3!
    poly = S::add(S::mul(poly, z2), k(1.0f));
    const F value = S::mul(poly, z);
    return S::select(S::lt(u, k(0.0f)), S::sub(k(0.0f), value), value);
}

inline F curveRead(const float* curve, F x) {
    const F at = S::mul(S::min(S::max(x, k(0.0f)), k(1.0f)), k(static_cast<float>(zed::osc::kCurvePoints)));
    const I i = S::toInt(at);
    const F t = S::sub(at, S::toFloat(i));
    const F a = S::gather(curve, i);
    const F b = S::gather(curve + 1, i);
    return S::add(a, S::mul(t, S::sub(b, a)));
}

// p * (1 + c) / (1 + c p): a curve from 0 to 1 that is steep at 0 for c > 0. Its mirror image
// p / (1 + c - c p) is steep at 1.
inline F easeOut(F p, F c) { return S::div(S::mul(p, S::add(k(1.0f), c)), S::add(k(1.0f), S::mul(c, p))); }
inline F easeIn(F p, F c) { return S::div(p, S::sub(S::add(k(1.0f), c), S::mul(c, p))); }

// One warp slot's phase stage. `p` is in 0..1 on entry and on exit.
inline F warpPhaseV(zed::osc::WarpMode mode, F p, F d, F m, const float* curve, F& gain) {
    using zed::osc::WarpMode;
    const F zero = k(0.0f), one = k(1.0f), half = k(0.5f);
    switch (mode) {
    case WarpMode::Sync:
        return fracF(S::mul(p, d));
    case WarpMode::SoftSync: {
        F window = S::mul(k(4.0f), S::mul(p, S::sub(one, p)));
        window = S::mul(window, window);
        const F depth = S::min(one, S::mul(k(2.0f), S::sub(d, one)));
        gain = S::mul(gain, S::add(S::sub(one, depth), S::mul(depth, window)));
        return fracF(S::mul(p, d));
    }
    case WarpMode::BendPlus:
    case WarpMode::BendMinus:
    case WarpMode::BendBoth: {
        const F u = S::sub(S::add(p, p), one); // -1 .. 1 around the middle of the cycle
        F a = S::abs(u);
        if (mode == WarpMode::BendPlus) {
            a = easeOut(a, d);
        } else if (mode == WarpMode::BendMinus) {
            a = easeIn(a, d);
        } else {
            a = easeOut(a, S::max(d, zero));
            a = easeIn(a, S::max(S::sub(zero, d), zero));
        }
        const F half_a = S::mul(half, a);
        return S::select(S::lt(u, zero), S::sub(half, half_a), S::add(half, half_a));
    }
    case WarpMode::PulseWidth:
        return S::min(S::mul(p, d), one);
    case WarpMode::SkewPlus:
        return easeOut(p, d);
    case WarpMode::SkewMinus:
        return easeIn(p, d);
    case WarpMode::SkewBoth:
        return easeIn(easeOut(p, S::max(d, zero)), S::max(S::sub(zero, d), zero));
    case WarpMode::Mirror: {
        const F q = S::mul(p, d);
        return S::select(S::lt(one, q), S::sub(k(2.0f), q), q);
    }
    case WarpMode::Flip:
        gain = S::mul(gain, S::select(S::lt(p, d), one, k(-1.0f)));
        return p;
    case WarpMode::Quantize:
        return S::div(floorF(S::mul(p, d)), d);
    case WarpMode::CurveFull:
        return S::add(p, S::mul(d, S::sub(curveRead(curve, p), p)));
    case WarpMode::CurveHalf: {
        const F base = S::select(S::lt(p, half), zero, half);
        const F shaped = S::add(base, S::mul(half, curveRead(curve, S::mul(S::sub(p, base), k(2.0f)))));
        return S::add(p, S::mul(d, S::sub(shaped, p)));
    }
    case WarpMode::CurveSine: {
        // A sine-shaped ease from 0 to 1, averaged with the drawn curve.
        const F ease = S::sub(half, S::mul(half, sinTurns(S::sub(k(0.25f), S::mul(half, p)))));
        const F shaped = S::mul(half, S::add(curveRead(curve, p), ease));
        return S::add(p, S::mul(d, S::sub(shaped, p)));
    }
    case WarpMode::CurveQuad: {
        const F q = S::mul(p, k(4.0f));
        const F base = S::min(floorF(q), k(3.0f));
        const F shaped = S::mul(k(0.25f), S::add(base, curveRead(curve, S::sub(q, base))));
        return S::add(p, S::mul(d, S::sub(shaped, p)));
    }
    case WarpMode::Pm:
        return fracF(S::add(p, S::mul(d, m)));
    default:
        return p;
    }
}

// One warp slot's amplitude stage.
inline F warpAmplitudeV(zed::osc::WarpMode mode, F y, F d, F m) {
    using zed::osc::WarpMode;
    const F one = k(1.0f);
    switch (mode) {
    case WarpMode::Fold: { // a triangle of period 4 that is the identity between -1 and 1
        const F t = fracF(S::mul(S::add(S::mul(y, d), one), k(0.25f)));
        return S::sub(one, S::abs(S::sub(S::mul(t, k(4.0f)), k(2.0f))));
    }
    case WarpMode::Drive:
        return S::div(S::mul(y, S::add(one, d)), S::add(one, S::mul(d, S::abs(y))));
    case WarpMode::Am:
        return S::mul(y, S::add(one, S::mul(d, m)));
    case WarpMode::Rm:
        return S::mul(y, S::add(S::sub(one, d), S::mul(d, m)));
    default:
        return y;
    }
}

// Correction for a step of +2 at phase 0 (two-sample polynomial).
inline F stepCorrection(F t, F dt, F inverseDt) {
    const F one = k(1.0f);
    const F x1 = S::mul(t, inverseDt);
    const F after = S::sub(S::sub(S::add(x1, x1), S::mul(x1, x1)), one);
    const F x2 = S::mul(S::sub(t, one), inverseDt);
    const F before = S::add(S::add(S::mul(x2, x2), S::add(x2, x2)), one);
    return S::select(S::lt(t, dt), after, S::select(S::lt(S::sub(one, dt), t), before, k(0.0f)));
}

// Correction for a corner at phase 0, for a unit change of slope per sample.
inline F cornerCorrection(F t, F dt, F inverseDt) {
    const F one = k(1.0f);
    const F a = S::sub(one, S::mul(t, inverseDt));
    const F after = S::mul(S::mul(a, a), S::mul(a, k(1.0f / 6.0f)));
    const F b = S::add(S::mul(S::sub(t, one), inverseDt), one);
    const F before = S::mul(S::mul(b, b), S::mul(b, k(1.0f / 6.0f)));
    return S::select(S::lt(t, dt), after, S::select(S::lt(S::sub(one, dt), t), before, k(0.0f)));
}

struct ClassicMix {
    float saw, square, triangle, sine;
};

inline ClassicMix classicMix(float shape) {
    const float s = (shape < 0.0f ? 0.0f : shape > 1.0f ? 1.0f : shape) * 3.0f;
    const auto tent = [](float x) { return x >= 1.0f ? 0.0f : x <= -1.0f ? 0.0f : 1.0f - (x < 0.0f ? -x : x); };
    return {tent(s), tent(s - 1.0f), tent(s - 2.0f), s > 2.0f ? s - 2.0f : 0.0f};
}

// The Classic engine: sawtooth, square, triangle and sine with polynomial step and corner
// correction. `p` is the phase, `inc` the phase increment per sample.
inline F classicWave(F p, F inc, const ClassicMix& mix, float pulseWidth) {
    const F one = k(1.0f), zero = k(0.0f);
    const F dt = S::min(S::max(inc, k(1.0e-7f)), k(0.25f));
    const F inverseDt = S::div(one, dt);
    F out = zero;
    if (mix.saw > 0.0f || mix.square > 0.0f) {
        const F edge = stepCorrection(p, dt, inverseDt);
        if (mix.saw > 0.0f) {
            // A falling ramp, so its fundamental is in phase with the other three shapes and
            // the morph between them never cancels.
            const F saw = S::add(S::sub(one, S::add(p, p)), edge);
            out = S::add(out, S::mul(saw, k(mix.saw)));
        }
        if (mix.square > 0.0f) {
            const F width = k(pulseWidth);
            const F naive = S::select(S::lt(p, width), one, k(-1.0f));
            const F fall = fracF(S::add(S::sub(p, width), one));
            const F square = S::sub(S::add(naive, edge), stepCorrection(fall, dt, inverseDt));
            out = S::add(out, S::mul(square, k(mix.square)));
        }
    }
    if (mix.triangle > 0.0f) {
        // In phase with the sine: zero at 0, peak at a quarter cycle.
        const F q = fracF(S::add(p, k(0.25f)));
        const F naive = S::sub(one, S::mul(k(4.0f), S::abs(S::sub(q, k(0.5f)))));
        const F low = cornerCorrection(q, dt, inverseDt);                              // slope -4 -> +4
        const F high = cornerCorrection(fracF(S::add(q, k(0.5f))), dt, inverseDt);     // slope +4 -> -4
        const F triangle = S::add(naive, S::mul(S::mul(k(8.0f), dt), S::sub(low, high)));
        out = S::add(out, S::mul(triangle, k(mix.triangle)));
    }
    if (mix.sine > 0.0f)
        out = S::add(out, S::mul(sinTurns(p), k(mix.sine)));
    return out;
}

// Reads the wavetable at phase `p`, blending the frame at `frame` with the next by `blend`.
inline F tableRead(const float* table, F p, F frame, F blend, F off, F stride, F size) {
    const F at = S::mul(p, size);
    const I index = S::toInt(at);
    const F whole = S::toFloat(index);
    const F t = S::sub(at, whole);
    const F u = S::sub(k(1.0f), t);
    // Quintic B-spline weights for the samples at -2 .. +3.
    const F t2 = S::mul(t, t), u2 = S::mul(u, u);
    const F scale = k(1.0f / 120.0f);
    const auto mid = [&](F v) {
        F r = S::sub(k(5.0f), S::mul(k(5.0f), v));
        r = S::add(k(10.0f), S::mul(v, r));
        r = S::add(k(10.0f), S::mul(v, r));
        r = S::add(k(5.0f), S::mul(v, r));
        r = S::add(k(1.0f), S::mul(v, r));
        return S::mul(r, scale);
    };
    const auto inner = [&](F v) {
        F r = S::add(k(-20.0f), S::mul(k(10.0f), v));
        r = S::add(k(-20.0f), S::mul(v, r));
        r = S::add(k(20.0f), S::mul(v, r));
        r = S::add(k(50.0f), S::mul(v, r));
        r = S::add(k(26.0f), S::mul(v, r));
        return S::mul(r, scale);
    };
    const F w0 = S::mul(S::mul(S::mul(u2, u2), u), scale);
    const F w1 = mid(u);
    const F w2 = inner(u);
    const F w3 = inner(t);
    const F w4 = mid(t);
    const F w5 = S::mul(S::mul(S::mul(t2, t2), t), scale);

    // Sample indices are whole numbers below 2^24, exact in floats.
    const F baseF = S::add(S::add(off, S::mul(frame, stride)), whole);
    const I a = S::toInt(baseF);
    const I b = S::toInt(S::add(baseF, stride));
    const auto tap = [&](int n) {
        const F x = S::gather(table + n, a);
        const F y = S::gather(table + n, b);
        return S::add(x, S::mul(blend, S::sub(y, x)));
    };
    F sum = S::mul(w0, tap(0));
    sum = S::add(sum, S::mul(w1, tap(1)));
    sum = S::add(sum, S::mul(w2, tap(2)));
    sum = S::add(sum, S::mul(w3, tap(3)));
    sum = S::add(sum, S::mul(w4, tap(4)));
    sum = S::add(sum, S::mul(w5, tap(5)));
    return sum;
}

void renderImpl(zed::osc::Lanes& L, const zed::osc::Block& B) {
    using zed::osc::WarpMode;
    const int samples = B.samples;
    const int vectors = (L.count + W - 1) / W;
    const bool wantR = B.outR != nullptr, wantM = B.outM != nullptr;
    const ClassicMix mix = classicMix(B.shape);
    const bool modulatedA = B.modA != nullptr, modulatedB = B.modB != nullptr;

    F accL[kAccumulators][zed::osc::kMaxRun];
    F accR[kAccumulators][zed::osc::kMaxRun];
    F accM[kAccumulators][zed::osc::kMaxRun];

    for (int v = 0; v < vectors; ++v) {
        const int o = v * W;
        const int slot = v % kAccumulators;
        const bool first = v < kAccumulators;

        I phase = S::loadI(L.phase + o);
        F inc = S::load(L.inc + o);
        F fpos = S::load(L.fpos + o);
        F dA = S::load(L.dA + o), dB = S::load(L.dB + o);
        F gL = S::load(L.gL + o), gR = S::load(L.gR + o), gM = S::load(L.gM + o);
        F xf = S::load(L.xf + o);
        const F incStep = S::load(L.incStep + o), fposStep = S::load(L.fposStep + o);
        const F dAStep = S::load(L.dAStep + o), dBStep = S::load(L.dBStep + o);
        const F gLStep = S::load(L.gLStep + o), gRStep = S::load(L.gRStep + o), gMStep = S::load(L.gMStep + o);
        const F xfStep = S::load(L.xfStep + o);
        const F off = S::load(L.tabOff + o), stride = S::load(L.tabStride + o), size = S::load(L.tabSize + o);
        const F off2 = S::load(L.tabOff2 + o), stride2 = S::load(L.tabStride2 + o), size2 = S::load(L.tabSize2 + o);
        const F maxFrame = k(B.maxFrame);

        for (int s = 0; s < samples; ++s) {
            F p = S::mul(S::toFloat(S::shiftRight(phase, 8)), k(1.0f / 16777216.0f));
            F gain = k(1.0f);
            const F mA = k(modulatedA ? B.modA[s] : 0.0f);
            const F mB = k(modulatedB ? B.modB[s] : 0.0f);
            if (B.modeA != WarpMode::Off)
                p = warpPhaseV(B.modeA, p, dA, mA, B.curveA, gain);
            if (B.modeB != WarpMode::Off)
                p = warpPhaseV(B.modeB, p, dB, mB, B.curveB, gain);

            F y;
            if (B.classic) {
                y = classicWave(p, inc, mix, B.pulseWidth);
            } else {
                const F fp = S::min(S::max(fpos, k(0.0f)), maxFrame);
                const F frame = S::toFloat(S::toInt(fp));
                const F blend = S::sub(fp, frame);
                y = tableRead(B.table, p, frame, blend, off, stride, size);
                if (B.levelFade) {
                    const F other = tableRead(B.table, p, frame, blend, off2, stride2, size2);
                    y = S::add(y, S::mul(xf, S::sub(other, y)));
                }
            }
            y = S::mul(y, gain);
            if (B.modeA != WarpMode::Off)
                y = warpAmplitudeV(B.modeA, y, dA, mA);
            if (B.modeB != WarpMode::Off)
                y = warpAmplitudeV(B.modeB, y, dB, mB);

            const F left = S::mul(y, gL);
            accL[slot][s] = first ? left : S::add(accL[slot][s], left);
            if (wantR) {
                const F right = S::mul(y, gR);
                accR[slot][s] = first ? right : S::add(accR[slot][s], right);
            }
            if (wantM) {
                const F mono = S::mul(y, gM);
                accM[slot][s] = first ? mono : S::add(accM[slot][s], mono);
            }

            phase = S::addI(phase, S::toInt(S::mul(inc, k(4294967296.0f))));
            inc = S::add(inc, incStep);
            fpos = S::add(fpos, fposStep);
            dA = S::add(dA, dAStep);
            dB = S::add(dB, dBStep);
            gL = S::add(gL, gLStep);
            gR = S::add(gR, gRStep);
            gM = S::add(gM, gMStep);
            xf = S::add(xf, xfStep);
        }

        S::storeI(L.phase + o, phase);
        S::store(L.inc + o, inc);
        S::store(L.fpos + o, fpos);
        S::store(L.dA + o, dA);
        S::store(L.dB + o, dB);
        S::store(L.gL + o, gL);
        S::store(L.gR + o, gR);
        S::store(L.gM + o, gM);
        S::store(L.xf + o, xf);
    }

    // Add the lanes up in the fixed tree.
    const int used = vectors < kAccumulators ? vectors : kAccumulators;
    const auto reduce = [&](F acc[][zed::osc::kMaxRun], float* out) {
        for (int s = 0; s < samples; ++s) {
            float t[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            for (int a = 0; a < used; ++a)
                S::toArray(acc[a][s], t + a * W);
            out[s] = ((t[0] + t[4]) + (t[2] + t[6])) + ((t[1] + t[5]) + (t[3] + t[7]));
        }
    };
    reduce(accL, B.outL);
    if (wantR)
        reduce(accR, B.outR);
    if (wantM)
        reduce(accM, B.outM);
}

} // namespace
