#include "zederia/analysis/Analysis.h"

#include "zederia/audio/WavFile.h"
#include "zederia/dsp/Fft.h"
#include "zederia/params/ValueText.h"
#include "zederia/util/Image.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

#include <nlohmann/json.hpp>

namespace zed::analysis {
namespace {

using Complex = std::complex<double>;

double toDb(double power) { return power <= 1e-20 ? -200.0 : 10.0 * std::log10(power); }
double ampDb(double amp) { return amp <= 1e-10 ? -200.0 : 20.0 * std::log10(amp); }
double round2(double v, double step) { return std::round(v / step) * step; }

std::vector<double> monoOf(const Audio& a) {
    const std::size_t n = std::min(a.left.size(), a.right.size());
    std::vector<double> m(n);
    for (std::size_t i = 0; i < n; ++i)
        m[i] = 0.5 * (static_cast<double>(a.left[i]) + static_cast<double>(a.right[i]));
    return m;
}

// RMS level over `window` samples every `hop` samples, linear amplitude.
std::vector<double> slidingEnvelope(const Audio& a, int window, int hop) {
    const std::size_t n = std::min(a.left.size(), a.right.size());
    std::vector<double> prefix(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i)
        prefix[i + 1] = prefix[i] + 0.5 * (static_cast<double>(a.left[i]) * a.left[i] + static_cast<double>(a.right[i]) * a.right[i]);
    std::vector<double> env;
    for (std::size_t start = 0; start < n; start += static_cast<std::size_t>(hop)) {
        const std::size_t end = std::min(n, start + static_cast<std::size_t>(window));
        env.push_back(std::sqrt((prefix[end] - prefix[start]) / static_cast<double>(std::max<std::size_t>(1, end - start))));
    }
    return env;
}

// RMS level per hop (window = hop), linear amplitude.
std::vector<double> envelope(const Audio& a, int hop) {
    const std::size_t n = std::min(a.left.size(), a.right.size());
    std::vector<double> env;
    env.reserve(n / static_cast<std::size_t>(hop) + 1);
    for (std::size_t start = 0; start < n; start += static_cast<std::size_t>(hop)) {
        const std::size_t end = std::min(n, start + static_cast<std::size_t>(hop));
        double sum = 0.0;
        for (std::size_t i = start; i < end; ++i)
            sum += 0.5 * (static_cast<double>(a.left[i]) * a.left[i] + static_cast<double>(a.right[i]) * a.right[i]);
        env.push_back(std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, end - start))));
    }
    return env;
}

// --- Loudness (ITU-R BS.1770): K-weighting, 400 ms blocks with 75 % overlap, two gates -----------

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

void kWeighting(double fs, Biquad& shelf, Biquad& highpass) {
    { // high shelf, +4 dB above about 1.7 kHz
        const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / fs), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }
    { // high-pass at about 38 Hz
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + k / q + k * k;
        highpass.b0 = 1.0 / a0;
        highpass.b1 = -2.0 / a0;
        highpass.b2 = 1.0 / a0;
        highpass.a1 = 2.0 * (k * k - 1.0) / a0;
        highpass.a2 = (1.0 - k / q + k * k) / a0;
    }
}

double integratedLoudness(const Audio& a) {
    const std::size_t n = std::min(a.left.size(), a.right.size());
    if (n == 0)
        return -200.0;
    std::vector<double> power(n);
    for (int ch = 0; ch < 2; ++ch) {
        Biquad s, h;
        kWeighting(a.sampleRate, s, h);
        const auto& src = ch == 0 ? a.left : a.right;
        for (std::size_t i = 0; i < n; ++i) {
            const double y = h.run(s.run(static_cast<double>(src[i])));
            power[i] += y * y;
        }
    }
    const auto block = static_cast<std::size_t>(std::max(1.0, std::round(0.4 * a.sampleRate)));
    const auto step = std::max<std::size_t>(1, block / 4);
    std::vector<double> z;
    if (n < block) {
        z.push_back(std::accumulate(power.begin(), power.end(), 0.0) / static_cast<double>(n));
    } else {
        std::vector<double> prefix(n + 1, 0.0);
        for (std::size_t i = 0; i < n; ++i)
            prefix[i + 1] = prefix[i] + power[i];
        for (std::size_t start = 0; start + block <= n; start += step)
            z.push_back((prefix[start + block] - prefix[start]) / static_cast<double>(block));
    }
    const auto lufs = [](double ms) { return ms <= 1e-20 ? -200.0 : -0.691 + 10.0 * std::log10(ms); };
    double sum = 0.0;
    int count = 0;
    for (const double v : z)
        if (lufs(v) > -70.0) {
            sum += v;
            ++count;
        }
    if (count == 0)
        return -200.0;
    const double relative = lufs(sum / count) - 10.0;
    sum = 0.0;
    count = 0;
    for (const double v : z)
        if (lufs(v) > -70.0 && lufs(v) > relative) {
            sum += v;
            ++count;
        }
    return count == 0 ? -200.0 : lufs(sum / count);
}

// --- Spectra ---------------------------------------------------------------------------------------

constexpr int kFft = 4096;

std::vector<double> hann(int size) {
    std::vector<double> w(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i)
        w[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / size);
    return w;
}

// Power spectrum (bins 0..size/2) of the frame starting at `start`; zero padded past the end.
void powerSpectrum(const std::vector<double>& x, std::size_t start, int size, const std::vector<double>& window,
                   std::vector<double>& out) {
    std::vector<Complex> buf(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i) {
        const std::size_t idx = start + static_cast<std::size_t>(i);
        buf[static_cast<std::size_t>(i)] = idx < x.size() ? x[idx] * window[static_cast<std::size_t>(i)] : 0.0;
    }
    dsp::Fft::forSize(size).forward(buf.data());
    out.resize(static_cast<std::size_t>(size / 2 + 1));
    for (int k = 0; k <= size / 2; ++k)
        out[static_cast<std::size_t>(k)] = std::norm(buf[static_cast<std::size_t>(k)]);
}

// Normalised difference function (YIN): the lag of its first dip below the threshold, refined.
double yinPeriod(const std::vector<double>& x, std::size_t start, int size, int minLag, int maxLag, double& confidence) {
    confidence = 0.0;
    if (maxLag >= size / 2 || minLag < 2)
        return 0.0;
    const int w = size / 2;
    // The cross term of d(tau): r(tau) = sum over j < w of x[j] x[j + tau], the first half-frame
    // against the frame, through the FFT (correlation = inverse of conj(A) B).
    const int n = dsp::nextPowerOfTwo(size * 2);
    std::vector<Complex> half(static_cast<std::size_t>(n), 0.0), buf(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < size; ++i) {
        const std::size_t idx = start + static_cast<std::size_t>(i);
        const double v = idx < x.size() ? x[idx] : 0.0;
        buf[static_cast<std::size_t>(i)] = v;
        if (i < w)
            half[static_cast<std::size_t>(i)] = v;
    }
    const auto& fft = dsp::Fft::forSize(n);
    fft.forward(half.data());
    fft.forward(buf.data());
    for (std::size_t k = 0; k < buf.size(); ++k)
        buf[k] = std::conj(half[k]) * buf[k];
    fft.inverse(buf.data());
    std::vector<double> energy(static_cast<std::size_t>(size + 1), 0.0);
    for (int i = 0; i < size; ++i) {
        const std::size_t idx = start + static_cast<std::size_t>(i);
        const double v = idx < x.size() ? x[idx] : 0.0;
        energy[static_cast<std::size_t>(i + 1)] = energy[static_cast<std::size_t>(i)] + v * v;
    }
    // d(tau) = sum over j < w of (x[j] - x[j + tau])^2 = e(0..w) + e(tau..tau+w) - 2 r(tau).
    std::vector<double> d(static_cast<std::size_t>(maxLag + 2), 0.0);
    const double e0 = energy[static_cast<std::size_t>(w)];
    for (int tau = 1; tau <= maxLag + 1; ++tau) {
        const double et = energy[static_cast<std::size_t>(tau + w)] - energy[static_cast<std::size_t>(tau)];
        d[static_cast<std::size_t>(tau)] = std::max(0.0, e0 + et - 2.0 * buf[static_cast<std::size_t>(tau)].real());
    }
    std::vector<double> cm(d.size(), 1.0);
    double running = 0.0;
    for (int tau = 1; tau <= maxLag + 1; ++tau) {
        running += d[static_cast<std::size_t>(tau)];
        cm[static_cast<std::size_t>(tau)] = running > 0.0 ? d[static_cast<std::size_t>(tau)] * tau / running : 1.0;
    }
    int best = -1;
    for (int tau = minLag; tau <= maxLag; ++tau) {
        if (cm[static_cast<std::size_t>(tau)] < 0.15) {
            while (tau + 1 <= maxLag && cm[static_cast<std::size_t>(tau + 1)] < cm[static_cast<std::size_t>(tau)])
                ++tau;
            best = tau;
            break;
        }
    }
    if (best < 0) { // no dip under the threshold: take the global minimum
        best = minLag;
        for (int tau = minLag; tau <= maxLag; ++tau)
            if (cm[static_cast<std::size_t>(tau)] < cm[static_cast<std::size_t>(best)])
                best = tau;
    }
    confidence = std::clamp(1.0 - cm[static_cast<std::size_t>(best)], 0.0, 1.0);
    const double a = cm[static_cast<std::size_t>(best - 1)], b = cm[static_cast<std::size_t>(best)],
                 c = cm[static_cast<std::size_t>(best + 1)];
    const double denom = a - 2.0 * b + c;
    const double shift = std::abs(denom) > 1e-12 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
    return best + shift;
}

double median(std::vector<double> v) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

// Mel band energies (triangular filters) of a power spectrum of an FFT of `size` points.
std::vector<double> melBands(const std::vector<double>& power, int size, double fs, int bands, double lo, double hi) {
    std::vector<double> out(static_cast<std::size_t>(bands), 0.0);
    const double mlo = hzToMel(lo), mhi = hzToMel(std::min(hi, fs * 0.5));
    for (int b = 0; b < bands; ++b) {
        const double f0 = melToHz(mlo + (mhi - mlo) * b / (bands + 1));
        const double f1 = melToHz(mlo + (mhi - mlo) * (b + 1) / (bands + 1));
        const double f2 = melToHz(mlo + (mhi - mlo) * (b + 2) / (bands + 1));
        double sum = 0.0, weight = 0.0;
        for (std::size_t k = 1; k < power.size(); ++k) {
            const double f = static_cast<double>(k) * fs / size;
            if (f <= f0 || f >= f2)
                continue;
            const double wgt = f < f1 ? (f - f0) / (f1 - f0) : (f2 - f) / (f2 - f1);
            sum += wgt * power[k];
            weight += wgt;
        }
        out[static_cast<std::size_t>(b)] = weight > 0.0 ? sum / weight : 0.0;
    }
    return out;
}

// Average power spectrum over the frames whose level is within `rangeDb` of the loudest.
std::vector<double> averageSpectrum(const std::vector<double>& mono, double rangeDb, int& framesUsed) {
    framesUsed = 0;
    const auto window = hann(kFft);
    const int hop = kFft / 2;
    std::vector<double> levels;
    for (std::size_t start = 0; start < mono.size(); start += static_cast<std::size_t>(hop)) {
        double sum = 0.0;
        const std::size_t end = std::min(mono.size(), start + static_cast<std::size_t>(kFft));
        for (std::size_t i = start; i < end; ++i)
            sum += mono[i] * mono[i];
        levels.push_back(toDb(sum / kFft));
    }
    const double top = levels.empty() ? -200.0 : *std::max_element(levels.begin(), levels.end());
    std::vector<double> avg(static_cast<std::size_t>(kFft / 2 + 1), 0.0), frame;
    for (std::size_t f = 0; f < levels.size(); ++f) {
        if (levels[f] < top - rangeDb || levels[f] < -120.0)
            continue;
        powerSpectrum(mono, f * static_cast<std::size_t>(hop), kFft, window, frame);
        for (std::size_t k = 0; k < avg.size(); ++k)
            avg[k] += frame[k];
        ++framesUsed;
    }
    if (framesUsed > 0)
        for (double& v : avg)
            v /= framesUsed;
    return avg;
}

double centroidOf(const std::vector<double>& power, double fs, int size) {
    double num = 0.0, den = 0.0;
    for (std::size_t k = 1; k < power.size(); ++k) {
        const double f = static_cast<double>(k) * fs / size;
        if (f < 20.0)
            continue;
        num += f * power[k];
        den += power[k];
    }
    return den > 0.0 ? num / den : 0.0;
}

// Detrended series -> the strongest rate between 0.2 and 15 Hz and its amplitude.
void dominantRate(std::vector<double> series, double rate, double& hz, double& amplitude) {
    hz = 0.0;
    amplitude = 0.0;
    const int n = static_cast<int>(series.size());
    if (n < 16 || rate <= 0.0)
        return;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; ++i) {
        sx += i;
        sy += series[static_cast<std::size_t>(i)];
        sxx += static_cast<double>(i) * i;
        sxy += i * series[static_cast<std::size_t>(i)];
    }
    const double slope = (n * sxy - sx * sy) / std::max(1e-12, n * sxx - sx * sx);
    const double icept = (sy - slope * sx) / n;
    const int size = dsp::nextPowerOfTwo(n * 4);
    std::vector<Complex> buf(static_cast<std::size_t>(size), 0.0);
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / std::max(1, n - 1));
        buf[static_cast<std::size_t>(i)] = (series[static_cast<std::size_t>(i)] - (icept + slope * i)) * w;
    }
    dsp::Fft::forSize(size).forward(buf.data());
    double best = 0.0;
    int bestK = -1;
    for (int k = 1; k < size / 2; ++k) {
        const double f = k * rate / size;
        if (f < 0.2 || f > 15.0)
            continue;
        const double m = std::abs(buf[static_cast<std::size_t>(k)]);
        if (m > best) {
            best = m;
            bestK = k;
        }
    }
    if (bestK < 0)
        return;
    hz = bestK * rate / size;
    amplitude = 4.0 * best / n; // peak amplitude of a sine through a Hann window (coherent gain 1/2)
}

} // namespace

bool loadAudioFile(std::span<const std::uint8_t> wavBytes, std::vector<float>& left, std::vector<float>& right,
                   double& sampleRate, std::string& error) {
    WavData wav;
    if (!decodeWav(wavBytes, wav, error))
        return false;
    if (wav.channelCount() == 0 || wav.frames() == 0) {
        error = "the file holds no audio";
        return false;
    }
    left = wav.channels[0];
    right = wav.channelCount() > 1 ? wav.channels[1] : wav.channels[0];
    sampleRate = wav.sampleRate;
    return true;
}

Descriptors analyze(const Audio& audio, const Options& options) {
    Descriptors d;
    const double fs = audio.sampleRate > 0.0 ? audio.sampleRate : 48000.0;
    const std::size_t n = std::min(audio.left.size(), audio.right.size());
    d.durationSeconds = static_cast<double>(n) / fs;
    if (n == 0)
        return d;

    // Level
    double peak = 0.0, sumSq = 0.0, sumMono = 0.0, sumL = 0, sumR = 0, sumLR = 0, sumMid = 0, sumSide = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double l = audio.left[i], r = audio.right[i];
        peak = std::max({peak, std::abs(l), std::abs(r)});
        sumSq += 0.5 * (l * l + r * r);
        sumMono += 0.5 * (l + r);
        sumL += l * l;
        sumR += r * r;
        sumLR += l * r;
        const double mid = 0.5 * (l + r), side = 0.5 * (l - r);
        sumMid += mid * mid;
        sumSide += side * side;
        d.clippedSamples += (std::abs(l) >= 0.999) + (std::abs(r) >= 0.999);
    }
    d.peakDb = ampDb(peak);
    d.rmsDb = toDb(sumSq / static_cast<double>(n));
    d.crestDb = peak > 0.0 ? d.peakDb - d.rmsDb : 0.0;
    d.dcOffset = sumMono / static_cast<double>(n);
    d.loudnessLufs = integratedLoudness(audio);
    d.width = sumMid > 1e-20 ? sumSide / sumMid : 0.0;
    d.correlation = sumL > 1e-20 && sumR > 1e-20 ? sumLR / std::sqrt(sumL * sumR) : 1.0;
    if (peak <= 1e-7) {
        d.tags = {"silent"};
        return d;
    }

    // Time: 1 ms windows every 0.25 ms for attack and decay, 10 ms for the tail and the onsets.
    const int hopFine = std::max(1, static_cast<int>(std::round(fs * 0.00025)));
    const auto fine = slidingEnvelope(audio, std::max(1, static_cast<int>(std::round(fs * 0.001))), hopFine);
    const std::size_t peakAt = static_cast<std::size_t>(std::max_element(fine.begin(), fine.end()) - fine.begin());
    const double envPeak = fine[peakAt];
    {
        std::size_t i10 = 0;
        while (i10 < peakAt && fine[i10] < 0.1 * envPeak)
            ++i10;
        std::size_t i90 = i10;
        while (i90 < peakAt && fine[i90] < 0.9 * envPeak)
            ++i90;
        d.attackMs = static_cast<double>(i90 - i10) * hopFine / fs * 1000.0;
    }
    const double noteOff = options.noteOffSeconds;
    const std::size_t offFine = noteOff >= 0.0 ? std::min(fine.size(), static_cast<std::size_t>(noteOff * fs / hopFine))
                                                : fine.size();
    {
        std::size_t i = peakAt;
        while (i < fine.size() && ampDb(fine[i]) > ampDb(envPeak) - 20.0)
            ++i;
        if (i < fine.size())
            d.decayMs = static_cast<double>(i - peakAt) * hopFine / fs * 1000.0;
        const double heldFor = static_cast<double>(std::max<std::size_t>(offFine, peakAt + 1) - peakAt) * hopFine / fs;
        // Level just before note-off (or at the end) relative to the peak.
        const std::size_t probe = offFine > peakAt + 2 ? offFine - 2 : peakAt;
        const double heldDb = ampDb(fine[std::min(probe, fine.size() - 1)]) - ampDb(envPeak);
        const double notePart = static_cast<double>(std::max<std::size_t>(offFine, 1)) * hopFine / fs;
        if (static_cast<double>(peakAt) * hopFine / fs > 0.5 * notePart && notePart > 0.2)
            d.decayShape = "swelling";
        else if (d.decayMs >= 0.0 && d.decayMs < 200.0)
            d.decayShape = "percussive";
        else if (heldDb < -20.0 || (d.decayMs >= 0.0 && static_cast<double>(peakAt) * hopFine / fs + d.decayMs / 1000.0 < notePart))
            d.decayShape = "decaying";
        else if (heldFor > 0.0)
            d.decayShape = "sustained";
    }
    const int hop10 = std::max(1, static_cast<int>(std::round(fs * 0.01)));
    const auto env10 = envelope(audio, hop10);
    if (noteOff >= 0.0) {
        const std::size_t off = static_cast<std::size_t>(noteOff * fs / hop10);
        if (off < env10.size()) {
            const double ref = ampDb(env10[off > 0 ? off - 1 : 0]);
            const double floor = std::max(ref - 60.0, -100.0);
            std::size_t last = off;
            for (std::size_t i = off; i < env10.size(); ++i)
                if (ampDb(env10[i]) > floor)
                    last = i + 1;
            d.tailComplete = last < env10.size();
            d.tailMs = static_cast<double>(last - off) * hop10 / fs * 1000.0;
        } else {
            d.tailMs = 0.0;
        }
    }
    {
        const double floor = ampDb(envPeak) - 50.0;
        int count = 0;
        std::size_t lastOnset = 0;
        bool any = false;
        const std::size_t look = 3; // a rise of 9 dB within 30 ms, at least 50 ms after the last onset
        for (std::size_t i = 0; i < env10.size(); ++i) {
            const double now = ampDb(env10[i]);
            const double before = i >= look ? ampDb(env10[i - look]) : -200.0; // silence before the file
            if (now > floor && now - before > 9.0 && (!any || i - lastOnset > 5)) {
                ++count;
                lastOnset = i;
                any = true;
            }
        }
        d.transients = std::max(1, count);
    }

    // Pitch: YIN on 4096-sample frames where the sound is within 20 dB of its peak.
    const auto mono = monoOf(audio);
    {
        const int size = fs > 60000.0 ? 8192 : 4096;
        const int minLag = std::max(2, static_cast<int>(fs / 2000.0)), maxLag = std::min(size / 2 - 2, static_cast<int>(fs / 30.0));
        std::vector<double> f0s;
        double confSum = 0.0;
        for (std::size_t start = 0; start + static_cast<std::size_t>(size) <= mono.size(); start += static_cast<std::size_t>(size / 2)) {
            double sum = 0.0;
            for (int i = 0; i < size; ++i)
                sum += mono[start + static_cast<std::size_t>(i)] * mono[start + static_cast<std::size_t>(i)];
            if (ampDb(std::sqrt(sum / size)) < ampDb(envPeak) - 20.0)
                continue;
            double conf = 0.0;
            const double period = yinPeriod(mono, start, size, minLag, maxLag, conf);
            if (period > 0.0 && conf > 0.85) {
                f0s.push_back(fs / period);
                confSum += conf;
            }
        }
        if (f0s.size() >= 1) {
            const double f0 = median(f0s);
            d.pitched = true;
            d.fundamentalHz = f0;
            d.pitchConfidence = confSum / static_cast<double>(f0s.size());
            const double midi = 69.0 + 12.0 * std::log2(f0 / 440.0);
            const int nearest = static_cast<int>(std::lround(midi));
            d.note = noteName(std::clamp(nearest, 0, 127));
            d.centsOff = (midi - nearest) * 100.0;
            double var = 0.0;
            for (const double f : f0s) {
                const double c = 1200.0 * std::log2(f / f0);
                var += c * c;
            }
            d.stabilityCents = std::sqrt(var / static_cast<double>(f0s.size()));
            // Several notes: the median is only a summary, and harmonic measures against it mean nothing.
            d.pitchVaries = d.stabilityCents > 100.0;
        }
    }

    // Spectrum
    int framesUsed = 0;
    const auto spec = averageSpectrum(mono, 40.0, framesUsed);
    if (framesUsed > 0) {
        double total = 0.0;
        for (std::size_t k = 1; k < spec.size(); ++k)
            if (static_cast<double>(k) * fs / kFft >= 20.0)
                total += spec[k];
        d.centroidHz = centroidOf(spec, fs, kFft);
        double acc = 0.0;
        for (std::size_t k = 1; k < spec.size(); ++k) {
            if (static_cast<double>(k) * fs / kFft < 20.0)
                continue;
            acc += spec[k];
            if (acc >= 0.85 * total) {
                d.rolloffHz = static_cast<double>(k) * fs / kFft;
                break;
            }
        }
        double logSum = 0.0, linSum = 0.0;
        int bins = 0;
        for (std::size_t k = 1; k < spec.size(); ++k) {
            const double f = static_cast<double>(k) * fs / kFft;
            if (f < 20.0 || f > std::min(20000.0, fs * 0.5))
                continue;
            logSum += std::log(spec[k] + 1e-24);
            linSum += spec[k] + 1e-24;
            ++bins;
        }
        d.flatness = bins > 0 ? std::exp(logSum / bins) / (linSum / bins) : 0.0;
        for (std::size_t b = 0; b < 6; ++b) {
            double e = 0.0;
            for (std::size_t k = 1; k < spec.size(); ++k) {
                const double f = static_cast<double>(k) * fs / kFft;
                if (f >= kBandEdges[b] && f < kBandEdges[b + 1])
                    e += spec[k];
            }
            d.bandShare[b] = total > 0.0 ? e / total : 0.0;
        }
        if (d.pitched && !d.pitchVaries) {
            const double binHz = fs / kFft;
            double harmonic = 0.0;
            double devSum = 0.0, devWeight = 0.0;
            for (int h = 1; h * d.fundamentalHz < std::min(fs * 0.5, 20000.0); ++h) {
                const double target = h * d.fundamentalHz;
                const double reach = std::max(2.0 * binHz, 0.03 * target);
                double best = 0.0, bestF = target;
                for (std::size_t k = 1; k < spec.size(); ++k) {
                    const double f = static_cast<double>(k) * binHz;
                    if (std::abs(f - target) > reach)
                        continue;
                    harmonic += spec[k];
                    if (spec[k] > best) {
                        best = spec[k];
                        bestF = f;
                    }
                }
                if (h >= 2 && h <= 12 && best > 0.0 && bestF > 0.0 && target > 4.0 * binHz) {
                    // Refine the partial's frequency with a parabola through the peak's bins.
                    const auto k = static_cast<std::size_t>(std::lround(bestF / binHz));
                    if (k > 0 && k + 1 < spec.size()) {
                        const double a = std::log(spec[k - 1] + 1e-30), b = std::log(spec[k] + 1e-30), c = std::log(spec[k + 1] + 1e-30);
                        const double den = a - 2 * b + c;
                        const double off = std::abs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
                        const double fRef = (static_cast<double>(k) + off) * binHz;
                        devSum += std::abs(1200.0 * std::log2(fRef / target)) * best;
                        devWeight += best;
                    }
                }
            }
            const double noise = std::max(total - harmonic, total * 1e-6);
            d.hnrDb = toDb(harmonic / noise);
            d.inharmonicityCents = devWeight > 0.0 ? devSum / devWeight : 0.0;
        }
    }

    // Motion: centroid and level per 2048-sample frame.
    {
        const int size = 2048, hop = 1024;
        const auto window = hann(size);
        std::vector<double> frame, cents, levels;
        for (std::size_t start = 0; start + static_cast<std::size_t>(size) <= mono.size(); start += static_cast<std::size_t>(hop)) {
            double sum = 0.0;
            for (int i = 0; i < size; ++i)
                sum += mono[start + static_cast<std::size_t>(i)] * mono[start + static_cast<std::size_t>(i)];
            const double lvl = toDb(sum / size);
            if (lvl < ampDb(envPeak) - 30.0 || (noteOff >= 0.0 && static_cast<double>(start + size) / fs > noteOff))
                continue;
            powerSpectrum(mono, start, size, window, frame);
            const double c = centroidOf(frame, fs, size);
            if (c > 0.0) {
                cents.push_back(std::log2(c));
                levels.push_back(lvl);
            }
        }
        if (cents.size() >= 3) {
            const double mean = std::accumulate(cents.begin(), cents.end(), 0.0) / static_cast<double>(cents.size());
            double var = 0.0;
            for (const double c : cents)
                var += (c - mean) * (c - mean);
            d.centroidVariationOct = std::sqrt(var / static_cast<double>(cents.size()));
        }
        double levelHz = 0, levelAmp = 0, colourHz = 0, colourAmp = 0;
        dominantRate(levels, fs / hop, levelHz, levelAmp);
        dominantRate(cents, fs / hop, colourHz, colourAmp);
        // Report movement only when it is clear: 1 dB of level or 0.05 octave of colour.
        if (levelAmp >= 1.0 && (levelAmp / 1.0 >= colourAmp / 0.05 || colourAmp < 0.05)) {
            d.modulationRateHz = levelHz;
            d.modulationDepthDb = levelAmp;
        } else if (colourAmp >= 0.05) {
            d.modulationRateHz = colourHz;
            d.modulationDepthDb = levelAmp;
        }
    }

    // Tags
    auto& t = d.tags;
    if (d.centroidHz >= 2500.0)
        t.push_back("bright");
    else if (d.centroidHz > 0.0 && d.centroidHz <= 700.0)
        t.push_back("dark");
    if (d.attackMs >= 0.0 && d.attackMs <= 15.0 && (d.decayShape == "percussive" || (d.decayMs >= 0.0 && d.decayMs <= 400.0)))
        t.push_back("plucky");
    if (d.decayShape == "sustained" || d.decayShape == "swelling")
        t.push_back("sustained");
    if (d.width >= 0.25 || d.correlation < 0.6)
        t.push_back("wide");
    else if (d.width < 0.02)
        t.push_back("mono");
    if (d.flatness >= 0.3 || (d.pitched && !d.pitchVaries && d.hnrDb < 6.0))
        t.push_back("noisy");
    if (!d.pitched)
        t.push_back("unpitched");
    if (d.centroidVariationOct >= 0.25 || d.modulationRateHz > 0.0)
        t.push_back("evolving");
    else
        t.push_back("static");
    if (d.clippedSamples > 0)
        t.push_back("clipping");
    return d;
}

nlohmann::json toJson(const Descriptors& d) {
    using J = nlohmann::json;
    const auto r = [](double v, double step) { return std::isfinite(v) ? round2(v, step) : 0.0; };
    J bands = J::object();
    for (std::size_t b = 0; b < 6; ++b)
        bands[kBandNames[b]] = r(d.bandShare[b] * 100.0, 0.1);
    J pitch = J::object();
    pitch["pitched"] = d.pitched;
    if (d.pitched) {
        pitch["fundamental_hz"] = r(d.fundamentalHz, 0.01);
        pitch["note"] = d.note;
        pitch["cents_off"] = r(d.centsOff, 0.1);
        pitch["stability_cents"] = r(d.stabilityCents, 0.1);
        pitch["inharmonicity_cents"] = r(d.inharmonicityCents, 0.1);
        pitch["confidence"] = r(d.pitchConfidence, 0.01);
        pitch["varies"] = d.pitchVaries;
    }
    J out = {
        {"level",
         {{"peak_dbfs", r(d.peakDb, 0.01)},
          {"rms_dbfs", r(d.rmsDb, 0.01)},
          {"loudness_lufs", r(d.loudnessLufs, 0.1)},
          {"crest_db", r(d.crestDb, 0.1)},
          {"dc_offset", r(d.dcOffset, 1e-5)},
          {"clipped_samples", d.clippedSamples}}},
        {"time",
         {{"duration_s", r(d.durationSeconds, 0.001)},
          {"attack_ms", r(d.attackMs, 0.1)},
          {"decay_shape", d.decayShape},
          {"decay_ms", r(d.decayMs, 1.0)},
          {"tail_ms", r(d.tailMs, 1.0)},
          {"tail_complete", d.tailComplete},
          {"transients", d.transients}}},
        {"pitch", pitch},
        {"spectrum",
         {{"centroid_hz", r(d.centroidHz, 1.0)},
          {"rolloff_hz", r(d.rolloffHz, 1.0)},
          {"flatness", r(d.flatness, 0.001)},
          {"hnr_db", r(d.hnrDb, 0.1)},
          {"bands_percent", bands}}},
        {"motion",
         {{"centroid_variation_oct", r(d.centroidVariationOct, 0.001)},
          {"modulation_rate_hz", r(d.modulationRateHz, 0.01)},
          {"modulation_depth_db", r(d.modulationDepthDb, 0.1)}}},
        {"stereo", {{"width", r(d.width, 0.001)}, {"correlation", r(d.correlation, 0.001)}}},
        {"tags", d.tags},
    };
    return out;
}

namespace {

std::vector<double> melProfile(const Audio& a, int bands) {
    const auto mono = monoOf(a);
    int used = 0;
    const auto spec = averageSpectrum(mono, 40.0, used);
    auto mel = melBands(spec, kFft, a.sampleRate, bands, 30.0, 16000.0);
    for (double& v : mel)
        v = std::max(toDb(v), -140.0);
    return mel;
}

std::vector<double> envelopeFromOnset(const Audio& a) {
    const int hop = std::max(1, static_cast<int>(std::round(a.sampleRate * 0.01)));
    auto env = envelope(a, hop);
    double top = 0.0;
    for (const double v : env)
        top = std::max(top, v);
    std::vector<double> out;
    if (top <= 0.0)
        return out;
    std::size_t start = 0;
    while (start < env.size() && ampDb(env[start]) < ampDb(top) - 30.0)
        ++start;
    for (std::size_t i = start; i < env.size(); ++i)
        out.push_back(std::max(ampDb(env[i]) - ampDb(top), -80.0));
    return out;
}

} // namespace

Comparison compare(const Audio& a, const Audio& b) {
    Comparison c;
    const auto da = analyze(a), db = analyze(b);
    const auto ma = melProfile(a, 40), mb = melProfile(b, 40);
    double meanA = 0.0, meanB = 0.0;
    for (std::size_t i = 0; i < ma.size(); ++i) {
        meanA += ma[i];
        meanB += mb[i];
    }
    meanA /= static_cast<double>(ma.size());
    meanB /= static_cast<double>(mb.size());
    double diff = 0.0;
    for (std::size_t i = 0; i < ma.size(); ++i)
        diff += std::abs((ma[i] - meanA) - (mb[i] - meanB));
    c.spectralDb = diff / static_cast<double>(ma.size());
    const auto ea = envelopeFromOnset(a), eb = envelopeFromOnset(b);
    const std::size_t len = std::max(ea.size(), eb.size());
    double sq = 0.0;
    for (std::size_t i = 0; i < len; ++i) {
        const double va = i < ea.size() ? ea[i] : -80.0, vb = i < eb.size() ? eb[i] : -80.0;
        sq += (va - vb) * (va - vb);
    }
    c.envelopeDb = len > 0 ? std::sqrt(sq / static_cast<double>(len)) : 0.0;
    c.loudnessDiffLu = da.loudnessLufs - db.loudnessLufs;
    c.peakDiffDb = da.peakDb - db.peakDb;
    c.centroidDiffOct = da.centroidHz > 0.0 && db.centroidHz > 0.0 ? std::log2(da.centroidHz / db.centroidHz) : 0.0;
    c.attackDiffMs = da.attackMs - db.attackMs;
    c.widthDiff = da.width - db.width;
    c.pitchDiffCents = da.pitched && db.pitched ? 1200.0 * std::log2(da.fundamentalHz / db.fundamentalHz) : 0.0;
    c.distance = c.spectralDb + 0.5 * c.envelopeDb + 0.5 * std::min(std::abs(c.loudnessDiffLu), 40.0);
    c.similarity = 1.0 / (1.0 + c.distance / 6.0);
    return c;
}

nlohmann::json toJson(const Comparison& c) {
    const auto r = [](double v, double step) { return std::isfinite(v) ? round2(v, step) : 0.0; };
    return {{"spectral_db", r(c.spectralDb, 0.01)},     {"envelope_db", r(c.envelopeDb, 0.01)},
            {"loudness_diff_lu", r(c.loudnessDiffLu, 0.01)}, {"peak_diff_db", r(c.peakDiffDb, 0.01)},
            {"centroid_diff_oct", r(c.centroidDiffOct, 0.001)}, {"attack_diff_ms", r(c.attackDiffMs, 0.1)},
            {"width_diff", r(c.widthDiff, 0.001)},       {"pitch_diff_cents", r(c.pitchDiffCents, 0.1)},
            {"distance", r(c.distance, 0.001)},          {"similarity", r(c.similarity, 0.0001)}};
}

std::vector<std::uint8_t> melSpectrogramPng(const Audio& audio, int width, int height) {
    width = std::clamp(width, 64, 2048);
    height = std::clamp(height, 64, 1024);
    const auto mono = monoOf(audio);
    const int size = 2048;
    const auto window = hann(size);
    const int bands = 128;
    std::vector<std::vector<double>> cols(static_cast<std::size_t>(width));
    std::vector<double> frame;
    double top = -200.0;
    const double span = static_cast<double>(std::max<std::size_t>(mono.size(), 1));
    for (int x = 0; x < width; ++x) {
        const auto centre = static_cast<std::size_t>(span * (x + 0.5) / width);
        const std::size_t start = centre > static_cast<std::size_t>(size / 2) ? centre - static_cast<std::size_t>(size / 2) : 0;
        powerSpectrum(mono, start, size, window, frame);
        auto mel = melBands(frame, size, audio.sampleRate, bands, 30.0, 16000.0);
        for (double& v : mel) {
            v = toDb(v);
            top = std::max(top, v);
        }
        cols[static_cast<std::size_t>(x)] = std::move(mel);
    }
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
    // Colour ramp: night blue, violet, ember, amber, pale gold.
    static constexpr double stops[5][3] = {{8, 10, 24}, {58, 28, 110}, {178, 52, 82}, {236, 140, 40}, {252, 236, 180}};
    for (int y = 0; y < height; ++y) {
        const double bandPos = (1.0 - (y + 0.5) / height) * (bands - 1);
        const int b0 = static_cast<int>(bandPos);
        const int b1 = std::min(bands - 1, b0 + 1);
        const double fb = bandPos - b0;
        for (int x = 0; x < width; ++x) {
            const auto& col = cols[static_cast<std::size_t>(x)];
            const double db = col[static_cast<std::size_t>(b0)] * (1.0 - fb) + col[static_cast<std::size_t>(b1)] * fb;
            const double v = std::clamp((db - (top - 90.0)) / 90.0, 0.0, 1.0) * 4.0;
            const int s = std::min(3, static_cast<int>(v));
            const double f = v - s;
            auto* px = rgb.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3u;
            for (int ch = 0; ch < 3; ++ch)
                px[ch] = static_cast<std::uint8_t>(std::lround(stops[s][ch] * (1.0 - f) + stops[s + 1][ch] * f));
        }
    }
    return encodePngRgb(width, height, rgb);
}

} // namespace zed::analysis
