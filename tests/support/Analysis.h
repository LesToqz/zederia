// Measurement helpers for the oscillator tests: a spectrum with very low leakage, the level of
// everything that is not a harmonic of a known fundamental, and the click measure.
#pragma once

#include "zederia/dsp/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace zedtest {

// Modified Bessel function of the first kind, order zero.
inline double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 200; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-18)
            break;
    }
    return sum;
}

struct Spectrum {
    std::vector<double> magnitude; // amplitude of a sine at each bin (a full-scale sine reads 1)
    double binHz = 0.0;
    int lobeBins = 0; // half-width of a spectral line, in bins
};

// Kaiser-windowed spectrum of `size` samples starting at `from`. With beta = 30 the leakage of a
// line is below -200 dB outside +/- 11 bins, so nearby weak components stay visible.
inline Spectrum spectrumOf(const std::vector<float>& x, std::size_t from, int size, double sampleRate) {
    const double beta = 30.0;
    std::vector<std::complex<double>> data(static_cast<std::size_t>(size));
    double windowSum = 0.0;
    const double denominator = besselI0(beta);
    for (int i = 0; i < size; ++i) {
        const double r = 2.0 * i / (size - 1) - 1.0;
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / denominator;
        windowSum += w;
        const std::size_t at = from + static_cast<std::size_t>(i);
        data[static_cast<std::size_t>(i)] = at < x.size() ? x[at] * w : 0.0;
    }
    zed::dsp::Fft::forSize(size).forward(data.data());
    Spectrum s;
    s.binHz = sampleRate / size;
    s.lobeBins = 12;
    s.magnitude.resize(static_cast<std::size_t>(size / 2 + 1));
    for (std::size_t k = 0; k < s.magnitude.size(); ++k)
        s.magnitude[k] = 2.0 * std::abs(data[k]) / windowSum;
    return s;
}

struct HarmonicReport {
    double carrierDb = -300.0;   // level of the fundamental
    double worstDbc = -300.0;    // strongest component that is not a harmonic, relative to the fundamental
    double worstHz = 0.0;
};

// Everything within a line's width of a multiple of `f0` (and of zero) counts as harmonic.
// `maxHz` limits the search (0 = up to half the sample rate).
inline HarmonicReport nonHarmonicLevel(const Spectrum& s, double f0, double maxHz = 0.0) {
    HarmonicReport r;
    const auto bins = static_cast<int>(s.magnitude.size());
    const int lastBin = maxHz > 0.0 ? std::min(bins - 1, static_cast<int>(maxHz / s.binHz)) : bins - 1;
    const double f0Bins = f0 / s.binHz;
    double carrier = 0.0;
    for (int k = static_cast<int>(f0Bins) - 2; k <= static_cast<int>(f0Bins) + 3; ++k)
        if (k >= 0 && k < bins)
            carrier = std::max(carrier, s.magnitude[static_cast<std::size_t>(k)]);
    double worst = 0.0;
    int worstBin = 0;
    for (int k = 0; k <= lastBin; ++k) {
        const double harmonic = std::round(k / f0Bins);
        // The allowance grows slowly with the harmonic number: a tuning error of a thousandth of
        // a cent moves the 1000th harmonic by a small fraction of a bin.
        const double tolerance = s.lobeBins + 1.0 + harmonic * 2.0e-4 * f0Bins;
        if (std::abs(k - harmonic * f0Bins) <= tolerance)
            continue;
        if (s.magnitude[static_cast<std::size_t>(k)] > worst) {
            worst = s.magnitude[static_cast<std::size_t>(k)];
            worstBin = k;
        }
    }
    r.carrierDb = 20.0 * std::log10(std::max(carrier, 1e-300));
    r.worstDbc = 20.0 * std::log10(std::max(worst, 1e-300) / std::max(carrier, 1e-300));
    r.worstHz = worstBin * s.binHz;
    return r;
}

// Level of the spectral line nearest `hz` (searched within three bins). The window's main lobe
// is close to a Gaussian, so a parabola through the logarithms of the three highest bins gives
// the true peak even when the line falls between two bins.
inline double lineLevel(const Spectrum& s, double hz) {
    const auto bins = static_cast<int>(s.magnitude.size());
    const int centre = static_cast<int>(std::lround(hz / s.binHz));
    int best = std::clamp(centre, 1, bins - 2);
    for (int k = centre - 3; k <= centre + 3; ++k)
        if (k >= 1 && k < bins - 1 && s.magnitude[static_cast<std::size_t>(k)] > s.magnitude[static_cast<std::size_t>(best)])
            best = k;
    const double m0 = s.magnitude[static_cast<std::size_t>(best - 1)], m1 = s.magnitude[static_cast<std::size_t>(best)],
                 m2 = s.magnitude[static_cast<std::size_t>(best + 1)];
    if (m0 <= 0.0 || m1 <= 0.0 || m2 <= 0.0)
        return m1;
    const double a = std::log(m0), b = std::log(m1), c = std::log(m2);
    const double curvature = a - 2.0 * b + c;
    if (curvature >= 0.0)
        return m1;
    const double offset = 0.5 * (a - c) / curvature;
    return std::exp(b - 0.25 * (a - c) * offset);
}

// The click measure (plan 6.1): the largest sample-to-sample jump in [from, to).
inline float largestJump(const std::vector<float>& x, std::size_t from, std::size_t to) {
    float jump = 0.0f;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < to && i < x.size(); ++i)
        jump = std::max(jump, std::abs(x[i] - x[i - 1]));
    return jump;
}

inline double rmsOf(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < to && i < x.size(); ++i, ++n)
        sum += static_cast<double>(x[i]) * x[i];
    return n > 0 ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

} // namespace zedtest
