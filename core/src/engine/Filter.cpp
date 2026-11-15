#include "zederia/engine/Filter.h"

#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace zed::flt {
namespace {

constexpr float kMaxG = 31.8f;        // tan(0.49 pi): the highest corner a section is given
constexpr float kStateLimit = 512.0f; // far above any musical level: a safety net, not a sound
constexpr float kFadeStep = 1.0f / 32.0f;
constexpr int kAllpassStages = 6;
constexpr int kMaxCoreSamples = VoiceFilter::kMaxFrames * VoiceFilter::kMaxOversample;

// Family-specific slots of p_.
constexpr int kFeedback = 0, kHardness = 1, kInGain = 2, kOutGain = 3, kExtra = 4;

// A smooth saturator that reaches exactly +/-1 at +/-3 and has unit slope at zero.
inline float sat(float x) noexcept {
    x = x < -3.0f ? -3.0f : (x > 3.0f ? 3.0f : x);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// sin(2 pi t), any t (error below 1e-6).
inline float sineTurn(float t) noexcept {
    float u = t - std::floor(t + 0.5f);
    const float sign = u < 0.0f ? -1.0f : 1.0f;
    u = std::abs(u);
    if (u > 0.25f)
        u = 0.5f - u;
    const float z = u * 6.2831853f, z2 = z * z;
    return sign * z * (1.0f + z2 * (-1.6666667e-1f + z2 * (8.3333333e-3f + z2 * (-1.9841270e-4f + z2 * 2.7557319e-6f))));
}

inline float finiteOr(float v, float limit) noexcept {
    if (v > -limit && v < limit)
        return v;
    return v >= limit ? limit : (v <= -limit ? -limit : 0.0f); // NaN falls through to zero
}

std::vector<TypeInfo> buildTypes() {
    std::vector<TypeInfo> t;
    const auto add = [&](Family f, int a, int b) {
        t.push_back({f, static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b)});
    };
    const auto tap = [](Tap x) { return static_cast<int>(x); };
    // The order is the registry's filterN.type list.
    for (int poles = 1; poles <= 4; ++poles) add(Family::Svf, tap(Tap::Lp), poles);
    for (int poles = 1; poles <= 4; ++poles) add(Family::Svf, tap(Tap::Hp), poles);
    for (const Tap x : {Tap::Bp, Tap::Notch, Tap::Peak})
        for (const int poles : {2, 4})
            add(Family::Svf, tap(x), poles);
    for (int poles = 1; poles <= 4; ++poles) add(Family::Ladder, 0, poles);
    const Tap pairs[12][2] = {{Tap::Lp, Tap::Hp},     {Tap::Lp, Tap::Bp},      {Tap::Lp, Tap::Peak},   {Tap::Lp, Tap::Notch},
                              {Tap::Hp, Tap::Bp},     {Tap::Hp, Tap::Peak},    {Tap::Hp, Tap::Notch},  {Tap::Bp, Tap::Peak},
                              {Tap::Bp, Tap::Notch},  {Tap::Peak, Tap::Peak},  {Tap::Peak, Tap::Notch}, {Tap::Notch, Tap::Notch}};
    for (const auto& p : pairs) add(Family::Dual, tap(p[0]), tap(p[1]));
    for (int variant = 0; variant < 4; ++variant)
        for (const int poles : {2, 4})
            add(Family::Morph, variant, poles);
    for (const Family f : {Family::Comb, Family::Flange, Family::Phase})
        for (int sign = 0; sign < 2; ++sign)
            for (int damping = 0; damping < 3; ++damping)
                add(f, sign, damping);
    for (int voicing = 0; voicing < 3; ++voicing) add(Family::Formant, voicing, 0);
    for (int kind = 0; kind < 3; ++kind) add(Family::Shelf, kind, 0);
    add(Family::RingMod, 0, 0);
    add(Family::SampleHold, 0, 0);
    add(Family::AllpassCluster, 0, 0);
    add(Family::Diffuser, 0, 0);
    add(Family::BandReject, 0, 0);
    add(Family::Howl, 0, 0);
    add(Family::Howl, 1, 0);
    add(Family::GritComb, 0, 0);
    return t;
}

const std::vector<TypeInfo>& types() {
    static const std::vector<TypeInfo> all = buildTypes();
    return all;
}

double qFromResonance(double resonance, double qMax) { return 0.70710678 * std::pow(qMax / 0.70710678, std::clamp(resonance, 0.0, 1.0)); }

void tapMix(Tap tap, double k, double m[3]) {
    (void)k;
    switch (tap) {
    case Tap::Lp: m[0] = 1; m[1] = 0; m[2] = 0; break;
    case Tap::Hp: m[0] = 0; m[1] = 0; m[2] = 1; break;
    case Tap::Bp: m[0] = 0; m[1] = 1; m[2] = 0; break;
    case Tap::Notch: m[0] = 1; m[1] = 0; m[2] = 1; break;
    case Tap::Peak: m[0] = 1; m[1] = 0; m[2] = -1; break;
    }
}

// Vowel formants in Hz (first three resonances of a, e, i, o, u) for three voicings, with the
// level of each resonance.
const double kVowels[3][5][3] = {
    {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}},
    {{850, 1220, 2810}, {610, 2330, 2990}, {310, 2790, 3310}, {590, 920, 2710}, {370, 950, 2670}},
    {{1030, 1370, 3170}, {690, 2610, 3570}, {370, 3200, 3730}, {680, 1060, 3180}, {430, 1170, 3260}},
};
const double kVowelLevels[3] = {1.0, 0.5, 0.25};

double semiQ(double q) { return std::sqrt(q * 0.70710678); } // per-stage Q of a four-pole pair

double dampingHz(const TypeInfo& info, const Settings& s, int ch, bool high) {
    const double c = std::clamp(static_cast<double>(s.character), 0.0, 1.0);
    if (info.b == 0)
        return 200.0 * std::pow(100.0, c); // low-pass damping: 200 Hz .. 20 kHz
    if (info.b == 1)
        return 20.0 * std::pow(250.0, c); // high-pass damping: 20 Hz .. 5 kHz
    const double width = 0.5 + 3.5 * c;   // band: octaves either side of the cutoff
    return s.cutoffHz[ch] * std::exp2(high ? -width : width);
}

double combFeedback(const TypeInfo& info, const Settings& s) {
    const double r = std::clamp(static_cast<double>(s.resonance), 0.0, 1.0);
    const double amount = (info.family == Family::Phase ? 0.9 : 0.98) * (1.0 - (1.0 - r) * (1.0 - r));
    return info.family != Family::GritComb && info.a == 1 ? -amount : amount;
}

double ladderFeedback(const Settings& s) { return 4.2 * std::clamp(static_cast<double>(s.resonance), 0.0, 1.0); }
double driveGain(const Settings& s) { return 1.0 + 15.0 * static_cast<double>(s.drive) * s.drive; }
bool usesDelay(Family f) { return f == Family::Comb || f == Family::Flange || f == Family::Diffuser || f == Family::GritComb; }
bool usesDriveStage(Family f) { return f != Family::Ladder && f != Family::Howl && f != Family::RingMod && f != Family::GritComb; }

double combDelay(const Settings& s, int ch, double sampleRate) {
    const int length = delayLengthFor(sampleRate);
    return std::clamp(sampleRate / std::max(static_cast<double>(s.cutoffHz[ch]), 1.0), 3.0, static_cast<double>(length - 4));
}

const double kDiffuserRatios[4] = {1.0, 1.31, 1.73, 2.17};
double diffuserDelay(const Settings& s, int ch, int index, double sampleRate) {
    const int segment = delayLengthFor(sampleRate) / 4;
    const double base = sampleRate / std::max(static_cast<double>(s.cutoffHz[ch]), 1.0) * 0.5;
    const double spread = 0.25 + 1.5 * std::clamp(static_cast<double>(s.character), 0.0, 1.0);
    return std::clamp(base * (1.0 + (kDiffuserRatios[index] - 1.0) * spread), 2.0, static_cast<double>(segment - 3));
}
double diffuserCoefficient(const Settings& s) { return 0.2 + 0.6 * std::clamp(static_cast<double>(s.resonance), 0.0, 1.0); }

} // namespace

int typeCount() noexcept { return static_cast<int>(types().size()); }

const TypeInfo& typeInfo(int type) noexcept {
    const auto& all = types();
    return all[static_cast<std::size_t>(type >= 0 && type < static_cast<int>(all.size()) ? type : 0)];
}

bool hasLinearResponse(int type) noexcept {
    const Family f = typeInfo(type).family;
    return f != Family::RingMod && f != Family::SampleHold && f != Family::GritComb;
}

bool hasNonlinearCore(int type) noexcept {
    const Family f = typeInfo(type).family;
    return f == Family::Ladder || f == Family::Howl || f == Family::RingMod;
}

int delayLengthFor(double sampleRate) noexcept {
    int length = 256;
    while (length < static_cast<int>(sampleRate / 20.0) + 8)
        length *= 2;
    return length;
}

bool designLinear(const Settings& s, LinearDesign& out) noexcept {
    const TypeInfo& info = typeInfo(s.type);
    const double res = std::clamp(static_cast<double>(s.resonance), 0.0, 1.0);
    const double c = std::clamp(static_cast<double>(s.character), 0.0, 1.0);
    out = LinearDesign{};
    const auto section = [&](int poles, double scale, double k) -> StageDesign& {
        StageDesign& st = out.stage[out.count++];
        st.poles = poles;
        st.hz[0] = static_cast<double>(s.cutoffHz[0]) * scale;
        st.hz[1] = static_cast<double>(s.cutoffHz[1]) * scale;
        st.k = k;
        return st;
    };
    switch (info.family) {
    case Family::Svf: {
        const auto tap = static_cast<Tap>(info.a);
        const double q = qFromResonance(res, 40.0);
        if (info.b == 1) {
            tapMix(tap, 1.0, section(1, 1.0, 1.0).m);
        } else if (info.b == 4) {
            const double k = 1.0 / semiQ(q);
            tapMix(tap, k, section(2, 1.0, k).m);
            tapMix(tap, k, section(2, 1.0, k).m);
        } else {
            tapMix(tap, 1.0 / q, section(2, 1.0, 1.0 / q).m);
            if (info.b == 3)
                tapMix(tap, 1.0, section(1, 1.0, 1.0).m);
        }
        // Body below 50 %: the level thins as the resonance rises, the way a ladder's does.
        const double thin = c < 0.5 ? 1.0 - 0.7 * (1.0 - 2.0 * c) * res : 1.0;
        for (double& m : out.stage[out.count - 1].m)
            m *= thin;
        return true;
    }
    case Family::Dual: {
        // The two sections share the resonance, so that together they peak about as high as one
        // section alone would.
        const double k = 1.0 / semiQ(qFromResonance(res, 40.0));
        tapMix(static_cast<Tap>(info.a), k, section(2, 1.0, k).m);
        tapMix(static_cast<Tap>(info.b), k, section(2, std::exp2((c - 0.5) * 8.0), k).m); // +/- 4 octaves
        return true;
    }
    case Family::Morph: {
        static const Tap sets[4][3] = {{Tap::Lp, Tap::Bp, Tap::Hp},
                                       {Tap::Lp, Tap::Peak, Tap::Hp},
                                       {Tap::Lp, Tap::Notch, Tap::Hp},
                                       {Tap::Bp, Tap::Peak, Tap::Notch}};
        const double q = qFromResonance(res, 40.0);
        const double k = 1.0 / (info.b == 4 ? semiQ(q) : q);
        const int from = c < 0.5 ? 0 : 1;
        const double t = c < 0.5 ? c * 2.0 : (c - 0.5) * 2.0;
        double a[3], b[3];
        tapMix(sets[info.a & 3][from], k, a);
        tapMix(sets[info.a & 3][from + 1], k, b);
        for (int n = 0; n < (info.b == 4 ? 2 : 1); ++n) {
            StageDesign& st = section(2, 1.0, k);
            for (int i = 0; i < 3; ++i)
                st.m[i] = a[i] + (b[i] - a[i]) * t;
        }
        return true;
    }
    case Family::Formant: {
        out.parallel = true;
        const double k = 1.0 / (4.0 * std::pow(8.0, res));
        const double shift = std::exp2((c - 0.5) * 2.0);
        for (int f = 0; f < 3; ++f) {
            StageDesign& st = section(2, 1.0, k);
            for (int ch = 0; ch < 2; ++ch) {
                // The cutoff picks the vowel: its range, on a logarithmic scale, runs a e i o u.
                const double pos = std::clamp(std::log2(std::max(static_cast<double>(s.cutoffHz[ch]), 8.0) / 8.0) / std::log2(2750.0), 0.0, 1.0) * 4.0;
                const int v = std::min(static_cast<int>(pos), 3);
                const double t = pos - v;
                const double lo = kVowels[info.a % 3][v][f], hi = kVowels[info.a % 3][v + 1][f];
                st.hz[ch] = lo * std::pow(hi / lo, t) * shift;
            }
            st.m[0] = 0.0;
            st.m[1] = k * kVowelLevels[f];
            st.m[2] = 0.0;
        }
        return true;
    }
    case Family::Shelf: {
        const double a = std::pow(10.0, (c - 0.5) * 36.0 / 40.0); // +/- 18 dB
        if (info.a == 2) {
            const double k = 1.0 / (0.5 * std::pow(40.0, res) * a);
            StageDesign& st = section(2, 1.0, k);
            st.m[0] = 1.0;
            st.m[1] = k * a * a;
            st.m[2] = 1.0;
        } else {
            const double k = 1.0 / (0.70710678 * std::pow(8.0, res));
            StageDesign& st = section(2, 1.0, k);
            st.gScale = info.a == 0 ? 1.0 / std::sqrt(a) : std::sqrt(a);
            st.m[0] = info.a == 0 ? a * a : 1.0;
            st.m[1] = k * a;
            st.m[2] = info.a == 0 ? 1.0 : a * a;
        }
        return true;
    }
    case Family::AllpassCluster: {
        const double k = 1.0 / (0.5 * std::pow(20.0, res));
        for (int i = 0; i < kMaxStages; ++i) {
            StageDesign& st = section(2, std::exp2(c * 2.0 * (i - 2.5) / 2.5), k);
            st.m[0] = 1.0;
            st.m[1] = -k;
            st.m[2] = 1.0;
        }
        return true;
    }
    case Family::BandReject: {
        out.parallel = true;
        const double k = 1.0 / qFromResonance(res, 10.0);
        const double width = 0.25 + 2.75 * c;
        tapMix(Tap::Lp, k, section(2, std::exp2(-width), k).m);
        section(2, std::exp2(width), k).m[0] = 0.0; // the high side enters inverted: at the centre the two
        out.stage[1].m[2] = -1.0;                   // halves then cancel instead of adding up
        return true;
    }
    case Family::Howl: {
        const double k = std::max(1.2 - 1.5 * res, 0.02); // the audio path goes below zero; the display stops short
        tapMix(info.a == 0 ? Tap::Lp : Tap::Bp, k, section(2, 1.0, k).m);
        return true;
    }
    case Family::SampleHold: { // the smoothing filter after the hold
        const double k = 1.0 / qFromResonance(res, 10.0);
        tapMix(Tap::Lp, k, section(2, 2.0, k).m);
        return true;
    }
    default:
        return false;
    }
}

std::complex<double> response(const Settings& s, int channel, double sampleRate, double hz, int oversample) noexcept {
    using C = std::complex<double>;
    const TypeInfo& info = typeInfo(s.type);
    const int ch = channel == 1 ? 1 : 0;
    const double rate = sampleRate * (hasNonlinearCore(s.type) ? std::clamp(oversample, 1, 8) : 1); // the rate the core is designed at
    const double w = std::tan(kPi * std::min(hz, 0.4999 * rate) / rate);
    const auto gOf = [&](double corner, double scale) { return std::tan(kPi * std::min(corner, 0.49 * rate) / rate) * scale; };
    const auto onePoleLow = [&](double corner) { return C(1.0) / (C(1.0) + C(0.0, w / gOf(corner, 1.0))); };
    const C unit = std::exp(C(0.0, -2.0 * kPi * hz / sampleRate)); // one sample of delay

    C wet(1.0);
    LinearDesign design;
    switch (info.family) {
    case Family::Ladder: {
        const C pole = onePoleLow(s.cutoffHz[ch]);
        const double k = std::min(ladderFeedback(s), 3.95);
        C tap(1.0);
        for (int i = 0; i < std::clamp(static_cast<int>(info.b), 1, 4); ++i)
            tap *= pole;
        wet = tap * (driveGain(s) * (1.0 + 0.4 * k)) / (C(1.0) + k * pole * pole * pole * pole);
        break;
    }
    case Family::Comb:
    case Family::Flange:
    case Family::Phase: {
        C damping(1.0);
        if (info.b == 1 || info.b == 2)
            damping *= C(1.0) - onePoleLow(dampingHz(info, s, ch, true));
        if (info.b == 0 || info.b == 2)
            damping *= onePoleLow(dampingHz(info, s, ch, false));
        const double fb = combFeedback(info, s);
        const double sign = info.a == 1 ? -1.0 : 1.0;
        if (info.family == Family::Phase) {
            const C sn(0.0, w / gOf(s.cutoffHz[ch], 1.0));
            C chain(1.0);
            for (int i = 0; i < kAllpassStages; ++i)
                chain *= (C(1.0) - sn) / (C(1.0) + sn);
            wet = 0.5 * (C(1.0) + sign * chain / (C(1.0) - fb * damping * chain));
        } else {
            const C delay = std::pow(unit, combDelay(s, ch, sampleRate));
            const C loop = C(1.0) / (C(1.0) - fb * damping * delay);
            wet = info.family == Family::Comb ? loop * (1.0 - 0.7 * std::abs(fb)) : 0.5 * (C(1.0) + sign * damping * delay * loop);
        }
        break;
    }
    case Family::Diffuser: {
        const double c = diffuserCoefficient(s);
        for (int i = 0; i < 4; ++i) {
            const C delay = std::pow(unit, diffuserDelay(s, ch, i, sampleRate));
            wet *= (delay - c) / (C(1.0) - c * delay);
        }
        break;
    }
    case Family::RingMod:
    case Family::GritComb:
        break;
    default:
        if (designLinear(s, design)) {
            C total = design.parallel ? C(0.0) : C(1.0);
            for (int i = 0; i < design.count; ++i) {
                const StageDesign& st = design.stage[i];
                const C sn(0.0, w / gOf(st.hz[ch], st.gScale));
                const C h = st.poles == 1 ? (st.m[0] + st.m[2] * sn) / (C(1.0) + sn)
                                          : (st.m[0] + st.m[1] * sn + st.m[2] * sn * sn) / (sn * sn + st.k * sn + C(1.0));
                total = design.parallel ? total + h : total * h;
            }
            wet = total;
        }
        break;
    }
    if (usesDriveStage(info.family) && s.drive > 0.0f)
        wet *= std::sqrt(driveGain(s));
    if (info.family == Family::Svf && s.character > 0.5f) {
        const double amount = (static_cast<double>(s.character) - 0.5) * 2.0;
        wet *= 1.0 + amount * 3.0 * amount;
    }
    const double mix = std::clamp(static_cast<double>(s.mix), 0.0, 1.0);
    return wet * mix + (s.levelMode ? 0.0 : 1.0 - mix);
}

// --- VoiceFilter ---------------------------------------------------------------------------------

void VoiceFilter::prepare(double sampleRate, float* delayMemory, int delayLength) noexcept {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const bool usable = delayMemory != nullptr && delayLength >= 16 && (delayLength & (delayLength - 1)) == 0;
    memory_[0] = usable ? delayMemory : nullptr;
    memory_[1] = usable ? delayMemory + delayLength : nullptr;
    memoryLength_ = usable ? delayLength : 0;
    memoryMask_ = usable ? delayLength - 1 : 0;
    type_ = -1;
    reset();
}

void VoiceFilter::reset() noexcept {
    std::memset(s1_, 0, sizeof s1_);
    std::memset(s2_, 0, sizeof s2_);
    std::memset(dampState_, 0, sizeof dampState_);
    for (int ch = 0; ch < 2; ++ch) {
        phase_[ch] = held_[ch] = heldBefore_[ch] = 0.0f;
        writeIndex_[ch] = 0;
        driveRs_[ch] = bodyRs_[ch] = coreRs_[ch] = dryRs_[ch] = Resampler{};
        if (memory_[ch] != nullptr && type_ >= 0 && usesDelay(typeInfo(type_).family))
            std::memset(memory_[ch], 0, sizeof(float) * static_cast<std::size_t>(memoryLength_));
    }
    driveFade_ = bodyFade_ = 0.0f;
}

void VoiceFilter::settle() noexcept {
    for (int st = 0; st < kMaxStages; ++st) {
        g_[st][0].settle();
        g_[st][1].settle();
        k_[st].settle();
        for (auto& m : m_[st])
            m.settle();
    }
    for (auto& p : p_)
        p.settle();
    for (int ch = 0; ch < 2; ++ch) {
        dampLp_[ch].settle();
        dampHp_[ch].settle();
    }
    dry_.settle();
    wet_.settle();
    driveGain_.settle();
    driveAmount_.settle();
    bodyGain_.settle();
    bodyAmount_.settle();
}

void VoiceFilter::aimChain(const Settings& s, float perStep, bool snap) noexcept {
    LinearDesign design;
    if (!designLinear(s, design)) {
        stages_ = 0;
        return;
    }
    stages_ = design.count;
    parallel_ = design.parallel;
    const double rate = sampleRate_ * coreRate_;
    for (int st = 0; st < design.count; ++st) {
        const StageDesign& d = design.stage[st];
        poles_[st] = d.poles;
        for (int ch = 0; ch < 2; ++ch) {
            const double g = std::tan(kPi * std::clamp(d.hz[ch], 1.0, 0.49 * rate) / rate) * d.gScale;
            g_[st][ch].aim(static_cast<float>(std::min(g, static_cast<double>(kMaxG))), perStep, snap);
        }
        k_[st].aim(static_cast<float>(d.k), perStep, snap);
        for (int i = 0; i < 3; ++i)
            m_[st][i].aim(static_cast<float>(d.m[i]), perStep, snap);
    }
}

void VoiceFilter::aim(const Settings& in, int samples, int oversample, bool snap) noexcept {
    Settings s = in;
    s.type = s.type >= 0 && s.type < typeCount() ? s.type : 0;
    oversample = oversample >= 8 ? 8 : oversample >= 4 ? 4 : oversample >= 2 ? 2 : 1;
    const TypeInfo& info = typeInfo(s.type);
    const int coreRate = hasNonlinearCore(s.type) ? oversample : 1;
    if (s.type != type_ || oversample != oversample_) {
        // Another type (or another rate) shares none of the old states' meaning: start clean.
        type_ = s.type;
        info_ = info;
        oversample_ = oversample;
        coreRate_ = coreRate;
        reset();
        snap = true;
    }
    const float perStep = 1.0f / static_cast<float>(std::max(samples, 1));
    const double rate = sampleRate_ * coreRate_;
    const double res = std::clamp(static_cast<double>(s.resonance), 0.0, 1.0);
    const double c = std::clamp(static_cast<double>(s.character), 0.0, 1.0);
    const auto setP = [&](int index, double value) { p_[index].aim(static_cast<float>(value), perStep, snap); };
    const auto cornerG = [&](double hz) { return std::min(std::tan(kPi * std::clamp(hz, 1.0, 0.49 * rate) / rate), static_cast<double>(kMaxG)); };
    const auto setDamping = [&](bool lp, bool hp) {
        dampUseLp_ = lp;
        dampUseHp_ = hp;
        for (int ch = 0; ch < 2; ++ch) {
            const double gl = cornerG(dampingHz(info, s, ch, false)), gh = cornerG(dampingHz(info, s, ch, true));
            dampLp_[ch].aim(static_cast<float>(gl / (1.0 + gl)), perStep, snap);
            dampHp_[ch].aim(static_cast<float>(gh / (1.0 + gh)), perStep, snap);
        }
    };

    stages_ = 0;
    parallel_ = false;
    switch (info.family) {
    case Family::Ladder:
        for (int ch = 0; ch < 2; ++ch)
            g_[0][ch].aim(static_cast<float>(cornerG(s.cutoffHz[ch])), perStep, snap);
        setP(kFeedback, ladderFeedback(s));
        setP(kHardness, 0.25 * std::pow(16.0, c)); // heat
        setP(kInGain, driveGain(s) * (1.0 + 0.4 * ladderFeedback(s)));
        break;
    case Family::Howl:
        aimChain(s, perStep, snap);
        k_[0].aim(static_cast<float>(1.2 - 1.5 * res), perStep, snap); // negative damping at the top: it howls
        setP(kHardness, 1.0 + 3.0 * static_cast<double>(s.drive));
        setP(kExtra, (c - 0.5) * 1.2);
        setP(kInGain, driveGain(s));
        break;
    case Family::RingMod:
        for (int ch = 0; ch < 2; ++ch)
            g_[0][ch].aim(static_cast<float>(std::min(static_cast<double>(s.cutoffHz[ch]) / rate, 0.45)), perStep, snap);
        setP(kHardness, 1.0 + 7.0 * c); // carrier shape: sine toward square
        setP(kFeedback, res * 0.5);     // the input bends the carrier's phase
        setP(kInGain, driveGain(s));
        setP(kExtra, s.drive > 0.0f ? 1.0 : 0.0);
        break;
    case Family::SampleHold:
        aimChain(s, perStep, snap);
        for (int ch = 0; ch < 2; ++ch)
            dampLp_[ch].aim(static_cast<float>(std::min(static_cast<double>(s.cutoffHz[ch]) / sampleRate_, 1.0)), perStep, snap);
        setP(kExtra, c);
        break;
    case Family::Comb:
    case Family::Flange:
    case Family::GritComb: {
        for (int ch = 0; ch < 2; ++ch)
            g_[0][ch].aim(static_cast<float>(combDelay(s, ch, sampleRate_)), perStep, snap);
        const double fb = combFeedback(info, s);
        setP(kFeedback, fb);
        setP(kOutGain, 1.0 - 0.7 * std::abs(fb));
        if (info.family == Family::GritComb) {
            setP(kExtra, std::exp2(1.0 - (12.0 - 10.0 * c))); // quantizer step: 12 bits down to 2
            setP(kHardness, 1.0 + 3.0 * static_cast<double>(s.drive));
            dampUseLp_ = true;
            dampUseHp_ = false;
            for (int ch = 0; ch < 2; ++ch) {
                const double g = cornerG(6000.0);
                dampLp_[ch].aim(static_cast<float>(g / (1.0 + g)), perStep, snap);
            }
        } else {
            setDamping(info.b == 0 || info.b == 2, info.b == 1 || info.b == 2);
        }
        break;
    }
    case Family::Phase:
        for (int ch = 0; ch < 2; ++ch)
            g_[0][ch].aim(static_cast<float>(cornerG(s.cutoffHz[ch])), perStep, snap);
        setP(kFeedback, combFeedback(info, s));
        setDamping(info.b == 0 || info.b == 2, info.b == 1 || info.b == 2);
        break;
    case Family::Diffuser:
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 4; ++i)
                g_[i][ch].aim(static_cast<float>(diffuserDelay(s, ch, i, sampleRate_)), perStep, snap);
        setP(kFeedback, diffuserCoefficient(s));
        break;
    default:
        aimChain(s, perStep, snap);
        break;
    }

    const float mix = std::clamp(s.mix, 0.0f, 1.0f);
    dry_.aim(s.levelMode ? 0.0f : 1.0f - mix, perStep, snap);
    wet_.aim(mix, perStep, snap);
    driveWanted_ = usesDriveStage(info.family) && s.drive > 0.0f;
    driveGain_.aim(static_cast<float>(driveGain(s)), perStep, snap);
    driveAmount_.aim(1.0f, perStep, true);
    bodyWanted_ = info.family == Family::Svf && s.character > 0.5f;
    const float body = bodyWanted_ ? (s.character - 0.5f) * 2.0f : 0.0f;
    bodyGain_.aim(1.0f + 3.0f * body, perStep, snap);
    bodyAmount_.aim(body, perStep, snap);
    if (snap) {
        driveFade_ = driveWanted_ ? 1.0f : 0.0f;
        bodyFade_ = bodyWanted_ ? 1.0f : 0.0f;
    }
}

// Moves every ramp on by the same additions the audio loops made, one per sample they rendered
// (`coreSteps` per frame for the values an oversampled core uses). Adding step by step, instead
// of multiplying, makes the result independent of how a control interval was cut into calls, so
// the engine's output does not depend on the host's block size.
void VoiceFilter::advanceRamps(int coreSteps, int frames) noexcept {
    const auto move = [frames](Ramp& r, int rate) {
        if (r.step == 0.0f)
            return;
        const float step = r.step / static_cast<float>(rate);
        float now = r.now;
        for (int i = 0; i < frames * rate; ++i)
            now += step;
        r.now = now;
    };
    const int used = stages_ > 0 ? stages_ : kMaxStages; // a section chain only moves the sections it has
    for (int st = 0; st < used; ++st) {
        move(g_[st][0], coreSteps);
        move(g_[st][1], coreSteps);
        move(k_[st], coreSteps);
        for (auto& m : m_[st])
            move(m, coreSteps);
    }
    for (auto& p : p_)
        move(p, coreSteps);
    for (int ch = 0; ch < 2; ++ch) {
        move(dampLp_[ch], 1);
        move(dampHp_[ch], 1);
    }
    move(dry_, 1);
    move(wet_, 1);
    move(driveGain_, oversample_);
    move(driveAmount_, oversample_);
    move(bodyGain_, oversample_);
    move(bodyAmount_, oversample_);
}

void VoiceFilter::guardStates() noexcept {
    const int used = stages_ > 0 ? stages_ : kMaxStages;
    for (int st = 0; st < used; ++st)
        for (int ch = 0; ch < 2; ++ch) {
            s1_[st][ch] = finiteOr(s1_[st][ch], kStateLimit);
            s2_[st][ch] = finiteOr(s2_[st][ch], kStateLimit);
        }
    for (int ch = 0; ch < 2; ++ch) {
        dampState_[ch][0] = finiteOr(dampState_[ch][0], kStateLimit);
        dampState_[ch][1] = finiteOr(dampState_[ch][1], kStateLimit);
        held_[ch] = finiteOr(held_[ch], kStateLimit);
        heldBefore_[ch] = finiteOr(heldBefore_[ch], kStateLimit);
    }
}

// --- Cores ---------------------------------------------------------------------------------------

void VoiceFilter::runChain(float* io, int ch, int frames, const float* modScale) noexcept {
    float input[kMaxFrames];
    if (parallel_) {
        std::copy(io, io + frames, input);
        std::fill(io, io + frames, 0.0f);
    }
    for (int st = 0; st < stages_; ++st) {
        const float* src = parallel_ ? input : io;
        float out[kMaxFrames];
        float g = g_[st][ch].now;
        const float gs = g_[st][ch].step;
        float m0 = m_[st][0].now, m1 = m_[st][1].now, m2 = m_[st][2].now;
        const float m0s = m_[st][0].step, m1s = m_[st][1].step, m2s = m_[st][2].step;
        float s1 = s1_[st][ch];
        if (poles_[st] == 2) {
            float k = k_[st].now;
            const float ks = k_[st].step;
            float s2 = s2_[st][ch];
            const bool steady = modScale == nullptr && gs == 0.0f && ks == 0.0f && m0s == 0.0f && m1s == 0.0f && m2s == 0.0f;
            if (steady) {
                const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
                for (int i = 0; i < frames; ++i) {
                    const float x = src[i];
                    const float v3 = x - s2;
                    const float v1 = a1 * s1 + a2 * v3;
                    const float v2 = s2 + a2 * s1 + a3 * v3;
                    s1 = 2.0f * v1 - s1;
                    s2 = 2.0f * v2 - s2;
                    out[i] = m0 * v2 + m1 * v1 + m2 * (x - k * v1 - v2);
                }
            } else {
                for (int i = 0; i < frames; ++i) {
                    const float ge = modScale != nullptr ? std::min(g * modScale[i], kMaxG) : g;
                    const float a1 = 1.0f / (1.0f + ge * (ge + k)), a2 = ge * a1, a3 = ge * a2;
                    const float x = src[i];
                    const float v3 = x - s2;
                    const float v1 = a1 * s1 + a2 * v3;
                    const float v2 = s2 + a2 * s1 + a3 * v3;
                    s1 = 2.0f * v1 - s1;
                    s2 = 2.0f * v2 - s2;
                    out[i] = m0 * v2 + m1 * v1 + m2 * (x - k * v1 - v2);
                    g += gs;
                    k += ks;
                    m0 += m0s;
                    m1 += m1s;
                    m2 += m2s;
                }
            }
            s2_[st][ch] = s2;
        } else {
            for (int i = 0; i < frames; ++i) {
                const float ge = modScale != nullptr ? std::min(g * modScale[i], kMaxG) : g;
                const float x = src[i];
                const float v = (x - s1) * ge / (1.0f + ge);
                const float lp = v + s1;
                s1 = lp + v;
                out[i] = m0 * lp + m2 * (x - lp);
                g += gs;
                m0 += m0s;
                m2 += m2s;
            }
        }
        s1_[st][ch] = s1;
        if (parallel_) {
            for (int i = 0; i < frames; ++i)
                io[i] += out[i];
        } else {
            std::copy(out, out + frames, io);
        }
    }
}

void VoiceFilter::runLadder(float* io, int ch, int samples, const float* modScale) noexcept {
    const auto rate = static_cast<float>(coreRate_);
    float g = g_[0][ch].now, k = p_[kFeedback].now, h = p_[kHardness].now, gain = p_[kInGain].now;
    const float gs = g_[0][ch].step / rate, ks = p_[kFeedback].step / rate, hs = p_[kHardness].step / rate,
                gains = p_[kInGain].step / rate;
    float s[4] = {s1_[0][ch], s1_[1][ch], s1_[2][ch], s1_[3][ch]};
    const int tap = std::clamp(static_cast<int>(info_.b), 1, 4) - 1;
    for (int i = 0; i < samples; ++i) {
        const float ge = modScale != nullptr ? std::min(g * modScale[i / coreRate_], kMaxG) : g;
        const float G = ge / (1.0f + ge), R = 1.0f - G;
        const float x = io[i] * gain;
        // What the four poles hold right now, seen at the output, then the loop solved for this
        // sample as if it were linear; the saturator then acts on the true loop input.
        const float held = ((s[0] * R * G + s[1] * R) * G + s[2] * R) * G + s[3] * R;
        const float G4 = G * G * G * G;
        const float estimate = (G4 * x + held) / (1.0f + k * G4);
        float u = sat(h * (x - k * estimate)) / h;
        float y[4];
        for (int pole = 0; pole < 4; ++pole) {
            const float v = (u - s[pole]) * G;
            y[pole] = v + s[pole];
            s[pole] = y[pole] + v;
            u = y[pole];
        }
        io[i] = y[tap];
        g += gs;
        k += ks;
        h += hs;
        gain += gains;
    }
    for (int pole = 0; pole < 4; ++pole)
        s1_[pole][ch] = s[pole];
}

void VoiceFilter::runHowl(float* io, int ch, int samples, const float* modScale) noexcept {
    const auto rate = static_cast<float>(coreRate_);
    float g = g_[0][ch].now, k = k_[0].now, h = p_[kHardness].now, bias = p_[kExtra].now, gain = p_[kInGain].now;
    const float gs = g_[0][ch].step / rate, ks = k_[0].step / rate, hs = p_[kHardness].step / rate,
                biass = p_[kExtra].step / rate, gains = p_[kInGain].step / rate;
    float s1 = s1_[0][ch], s2 = s2_[0][ch];
    const bool band = info_.a == 1;
    for (int i = 0; i < samples; ++i) {
        const float ge = modScale != nullptr ? std::min(g * modScale[i / coreRate_], kMaxG) : g;
        const float a1 = 1.0f / (1.0f + ge * (ge + k)), a2 = ge * a1, a3 = ge * a2;
        const float x = io[i] * gain;
        const float v3 = x - s2;
        const float v1 = a1 * s1 + a2 * v3;
        const float v2 = s2 + a2 * s1 + a3 * v3;
        // The states are bent, lopsidedly when the bias is off centre: that is the howl.
        const float offset = sat(h * bias) / h;
        s1 = sat(h * (2.0f * v1 - s1 + bias)) / h - offset;
        s2 = sat(h * (2.0f * v2 - s2 + bias)) / h - offset;
        io[i] = 0.7f * (band ? v1 : v2);
        g += gs;
        k += ks;
        h += hs;
        bias += biass;
        gain += gains;
    }
    s1_[0][ch] = s1;
    s2_[0][ch] = s2;
}

void VoiceFilter::runRing(float* io, int ch, int samples, const float* modScale) noexcept {
    const auto rate = static_cast<float>(coreRate_);
    float inc = g_[0][ch].now, shape = p_[kHardness].now, fm = p_[kFeedback].now, gain = p_[kInGain].now, driven = p_[kExtra].now;
    const float incs = g_[0][ch].step / rate, shapes = p_[kHardness].step / rate, fms = p_[kFeedback].step / rate,
                gains = p_[kInGain].step / rate, drivens = p_[kExtra].step / rate;
    float phase = phase_[ch];
    for (int i = 0; i < samples; ++i) {
        float x = io[i];
        x += driven * (sat(gain * x) / std::sqrt(gain) - x);
        const float carrier = sat(shape * sineTurn(phase + fm * x)) / sat(shape);
        io[i] = x * carrier;
        phase += modScale != nullptr ? std::min(inc * modScale[i / coreRate_], 0.45f) : inc;
        phase -= std::floor(phase);
        inc += incs;
        shape += shapes;
        fm += fms;
        gain += gains;
        driven += drivens;
    }
    phase_[ch] = phase;
}

void VoiceFilter::runSampleHold(float* io, int ch, int frames, const float* modScale) noexcept {
    float inc = dampLp_[ch].now, smooth = p_[kExtra].now;
    const float incs = dampLp_[ch].step, smooths = p_[kExtra].step;
    float phase = phase_[ch], held = held_[ch], before = heldBefore_[ch];
    for (int i = 0; i < frames; ++i) {
        phase += modScale != nullptr ? std::min(inc * modScale[i], 1.0f) : inc;
        if (phase >= 1.0f) {
            phase -= std::floor(phase);
            before = held;
            held = io[i];
        }
        io[i] = held + smooth * (before + (held - before) * phase - held);
        inc += incs;
        smooth += smooths;
    }
    phase_[ch] = phase;
    held_[ch] = held;
    heldBefore_[ch] = before;
    runChain(io, ch, frames, modScale);
}

// The sample `delay` samples ago (3 <= delay <= length - 4), by third-order Lagrange interpolation.
float VoiceFilter::readDelay(int ch, float delay) const noexcept {
    const float position = static_cast<float>(writeIndex_[ch]) - delay + static_cast<float>(memoryLength_);
    const int whole = static_cast<int>(position);
    const float t = position - static_cast<float>(whole);
    const float* m = memory_[ch];
    const float a = m[(whole - 1) & memoryMask_], b = m[whole & memoryMask_], c = m[(whole + 1) & memoryMask_],
                d = m[(whole + 2) & memoryMask_];
    return a * (-t * (t - 1.0f) * (t - 2.0f) / 6.0f) + b * ((t + 1.0f) * (t - 1.0f) * (t - 2.0f) * 0.5f) +
           c * (-(t + 1.0f) * t * (t - 2.0f) * 0.5f) + d * ((t + 1.0f) * t * (t - 1.0f) / 6.0f);
}

void VoiceFilter::runComb(float* io, int ch, int frames, const float* modScale) noexcept {
    if (memory_[ch] == nullptr)
        return;
    float delay = g_[0][ch].now, fb = p_[kFeedback].now, outGain = p_[kOutGain].now;
    const float delays = g_[0][ch].step, fbs = p_[kFeedback].step, outGains = p_[kOutGain].step;
    float gl = dampLp_[ch].now, gh = dampHp_[ch].now;
    const float gls = dampLp_[ch].step, ghs = dampHp_[ch].step;
    float lowState = dampState_[ch][0], highState = dampState_[ch][1];
    const bool grit = info_.family == Family::GritComb, flange = info_.family == Family::Flange;
    float step = p_[kExtra].now, hard = p_[kHardness].now;
    const float steps = p_[kExtra].step, hards = p_[kHardness].step;
    const float sign = info_.a == 1 && !grit ? -1.0f : 1.0f;
    const auto maxDelay = static_cast<float>(memoryLength_ - 4);
    float* memory = memory_[ch];
    int write = writeIndex_[ch];
    for (int i = 0; i < frames; ++i) {
        writeIndex_[ch] = write;
        const float length = modScale != nullptr ? std::clamp(delay / modScale[i], 3.0f, maxDelay) : delay;
        float d = readDelay(ch, length);
        if (dampUseHp_) {
            const float v = (d - highState) * gh;
            const float lp = v + highState;
            highState = lp + v;
            d -= lp;
        }
        if (dampUseLp_) {
            const float v = (d - lowState) * gl;
            d = v + lowState;
            lowState = d + v;
        }
        if (grit) {
            d = sat(d * hard);
            d = step * std::floor(d / step + 0.5f);
        }
        const float x = io[i];
        const float w = finiteOr(x + fb * d, 64.0f);
        memory[write] = w;
        write = (write + 1) & memoryMask_;
        io[i] = flange ? 0.5f * (x + sign * d) : w * outGain;
        delay += delays;
        fb += fbs;
        outGain += outGains;
        gl += gls;
        gh += ghs;
        step += steps;
        hard += hards;
    }
    writeIndex_[ch] = write;
    dampState_[ch][0] = lowState;
    dampState_[ch][1] = highState;
}

void VoiceFilter::runPhaser(float* io, int ch, int frames, const float* modScale) noexcept {
    float g = g_[0][ch].now, fb = p_[kFeedback].now, gl = dampLp_[ch].now, gh = dampHp_[ch].now;
    const float gs = g_[0][ch].step, fbs = p_[kFeedback].step, gls = dampLp_[ch].step, ghs = dampHp_[ch].step;
    float s[kAllpassStages];
    for (int i = 0; i < kAllpassStages; ++i)
        s[i] = s1_[i][ch];
    float lowState = dampState_[ch][0], highState = dampState_[ch][1];
    for (int i = 0; i < frames; ++i) {
        const float ge = modScale != nullptr ? std::min(g * modScale[i], kMaxG) : g;
        const float G = ge / (1.0f + ge);
        // The all-pass states are kept divided by sqrt(g). In that form a stage neither gains nor
        // loses energy however fast the corner moves, so the loop cannot be pumped up by
        // audio-rate modulation: its gain stays below one.
        const float root = std::sqrt(ge);
        // Each all-pass, then the damping, is "gain times input plus what it holds". Composing
        // them gives the whole loop in that form, which is solved for this sample's loop input.
        const float alpha = 2.0f * G - 1.0f, hold = 2.0f * (1.0f - G) * root;
        float gainSoFar = 1.0f, heldSoFar = 0.0f;
        for (int n = 0; n < kAllpassStages; ++n) {
            gainSoFar *= alpha;
            heldSoFar = heldSoFar * alpha + hold * s[n];
        }
        if (dampUseHp_) {
            gainSoFar *= 1.0f - gh;
            heldSoFar = (heldSoFar - highState) * (1.0f - gh);
        }
        if (dampUseLp_) {
            gainSoFar *= gl;
            heldSoFar = heldSoFar * gl + (1.0f - gl) * lowState;
        }
        const float x = io[i];
        const float u = (x + fb * heldSoFar) / (1.0f - fb * gainSoFar);
        float y = u;
        for (int n = 0; n < kAllpassStages; ++n) {
            const float state = s[n] * root;
            const float v = (y - state) * G;
            const float lp = v + state;
            s[n] = (lp + v) / root;
            y = 2.0f * lp - y;
        }
        float damped = y;
        if (dampUseHp_) {
            const float v = (damped - highState) * gh;
            const float lp = v + highState;
            highState = lp + v;
            damped -= lp;
        }
        if (dampUseLp_) {
            const float v = (damped - lowState) * gl;
            damped = v + lowState;
            lowState = damped + v;
        }
        io[i] = 0.5f * (x + (info_.a == 1 ? -y : y));
        g += gs;
        fb += fbs;
        gl += gls;
        gh += ghs;
    }
    for (int i = 0; i < kAllpassStages; ++i)
        s1_[i][ch] = s[i];
    dampState_[ch][0] = lowState;
    dampState_[ch][1] = highState;
}

void VoiceFilter::runDiffuser(float* io, int ch, int frames, const float* modScale) noexcept {
    if (memory_[ch] == nullptr)
        return;
    const int segment = memoryLength_ / 4, mask = segment - 1;
    const auto maxDelay = static_cast<float>(segment - 3);
    float c = p_[kFeedback].now;
    const float cs = p_[kFeedback].step;
    float lengths[4] = {g_[0][ch].now, g_[1][ch].now, g_[2][ch].now, g_[3][ch].now};
    int write = writeIndex_[ch] & mask;
    for (int i = 0; i < frames; ++i) {
        float x = io[i];
        for (int n = 0; n < 4; ++n) {
            float* m = memory_[ch] + n * segment;
            float length = lengths[n];
            lengths[n] += g_[n][ch].step;
            if (modScale != nullptr)
                length /= modScale[i];
            length = std::clamp(length, 2.0f, maxDelay);
            const float position = static_cast<float>(write) - length + static_cast<float>(segment);
            const int whole = static_cast<int>(position);
            const float t = position - static_cast<float>(whole);
            const float d = m[whole & mask] + t * (m[(whole + 1) & mask] - m[whole & mask]);
            const float v = finiteOr(x + c * d, 64.0f);
            m[write] = v;
            x = d - c * v;
        }
        io[i] = x;
        write = (write + 1) & mask;
        c += cs;
    }
    writeIndex_[ch] = write;
}

// A saturating stage around the core: up, bend, down, then faded against its own input so that
// switching it on or off while a note sounds does not click.
void VoiceFilter::runSaturator(float* left, float* right, int frames, Resampler* resamplers, Ramp& gain, Ramp& amount,
                               float& fade, bool wanted, const dsp::DecimatorDesign& hb) noexcept {
    if (!wanted && fade <= 0.0f)
        return;
    const int os = oversample_;
    if (wanted && fade <= 0.0f)
        resamplers[0] = resamplers[1] = Resampler{};
    const bool compensate = &gain == &driveGain_; // the drive stage keeps the level roughly level
    const float fadeStep = wanted ? kFadeStep : -kFadeStep;
    float* io[2] = {left, right};
    float fadeEnd = fade;
    for (int ch = 0; ch < 2; ++ch) {
        float work[kMaxCoreSamples], scratch[kMaxCoreSamples];
        if (os > 1)
            dsp::interpolate(hb, resamplers[ch].up, io[ch], work, scratch, frames, os);
        else
            std::copy(io[ch], io[ch] + frames, work);
        float g = gain.now, a = amount.now;
        const float gs = gain.step / static_cast<float>(os), as = amount.step / static_cast<float>(os);
        for (int i = 0; i < frames * os; ++i) {
            const float x = work[i];
            const float bent = compensate ? sat(g * x) / std::sqrt(g) : sat(g * x);
            work[i] = x + a * (bent - x);
            g += gs;
            a += as;
        }
        if (os > 1)
            dsp::decimate(hb, resamplers[ch].down, work, frames, os);
        float f = fade;
        for (int i = 0; i < frames; ++i) {
            io[ch][i] += f * (work[i] - io[ch][i]);
            f = std::clamp(f + fadeStep, 0.0f, 1.0f);
        }
        fadeEnd = f;
    }
    fade = fadeEnd;
}

void VoiceFilter::process(float* left, float* right, int frames, const float* cutoffMod,
                          const dsp::DecimatorDesign& hb) noexcept {
    if (frames <= 0 || type_ < 0)
        return;
    frames = std::min(frames, kMaxFrames);
    float* io[2] = {left, right};

    float scale[kMaxFrames];
    const float* modScale = nullptr;
    if (cutoffMod != nullptr) {
        for (int i = 0; i < frames; ++i)
            scale[i] = std::exp2(std::clamp(cutoffMod[i], -10.0f, 10.0f));
        modScale = scale;
    }

    const bool dryNeeded = dry_.now != 0.0f || dry_.target != 0.0f;
    float dry[2][kMaxFrames];
    if (dryNeeded) {
        std::copy(left, left + frames, dry[0]);
        std::copy(right, right + frames, dry[1]);
    }
    const bool delayedWet = oversample_ > 1 && (coreRate_ > 1 || driveWanted_ || driveFade_ > 0.0f || bodyWanted_ || bodyFade_ > 0.0f);

    runSaturator(left, right, frames, driveRs_, driveGain_, driveAmount_, driveFade_, driveWanted_, hb);

    for (int ch = 0; ch < 2; ++ch) {
        switch (info_.family) {
        case Family::Ladder:
        case Family::Howl:
        case Family::RingMod: {
            float work[kMaxCoreSamples], scratch[kMaxCoreSamples];
            const int samples = frames * coreRate_;
            if (coreRate_ > 1)
                dsp::interpolate(hb, coreRs_[ch].up, io[ch], work, scratch, frames, coreRate_);
            else
                std::copy(io[ch], io[ch] + frames, work);
            if (info_.family == Family::Ladder)
                runLadder(work, ch, samples, modScale);
            else if (info_.family == Family::Howl)
                runHowl(work, ch, samples, modScale);
            else
                runRing(work, ch, samples, modScale);
            if (coreRate_ > 1)
                dsp::decimate(hb, coreRs_[ch].down, work, frames, coreRate_);
            std::copy(work, work + frames, io[ch]);
            break;
        }
        case Family::SampleHold: runSampleHold(io[ch], ch, frames, modScale); break;
        case Family::Comb:
        case Family::Flange:
        case Family::GritComb: runComb(io[ch], ch, frames, modScale); break;
        case Family::Phase: runPhaser(io[ch], ch, frames, modScale); break;
        case Family::Diffuser: runDiffuser(io[ch], ch, frames, modScale); break;
        default: runChain(io[ch], ch, frames, modScale); break;
        }
    }

    runSaturator(left, right, frames, bodyRs_, bodyGain_, bodyAmount_, bodyFade_, bodyWanted_, hb);

    for (int ch = 0; ch < 2; ++ch) {
        float wetGain = wet_.now, dryGain = dry_.now;
        if (dryNeeded && delayedWet) {
            // The dry signal goes up and down through the same filters so that it stays in step
            // with the filtered one.
            float work[kMaxCoreSamples], scratch[kMaxCoreSamples];
            dsp::interpolate(hb, dryRs_[ch].up, dry[ch], work, scratch, frames, oversample_);
            dsp::decimate(hb, dryRs_[ch].down, work, frames, oversample_);
            std::copy(work, work + frames, dry[ch]);
        }
        for (int i = 0; i < frames; ++i) {
            io[ch][i] = io[ch][i] * wetGain + (dryNeeded ? dry[ch][i] * dryGain : 0.0f);
            wetGain += wet_.step;
            dryGain += dry_.step;
        }
    }
    advanceRamps(coreRate_, frames);
    guardStates();
}

} // namespace zed::flt
