#include "zederia/dsp/HalfBand.h"

#include "zederia/util/Math.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace zed::dsp {
namespace {

// Theta-function series of the elliptic design; they converge in a few terms.
double seriesNumerator(double q, int order, int c) {
    double acc = 0.0, sign = 1.0;
    for (int i = 0; i < 64; ++i) {
        const double term = std::pow(q, static_cast<double>(i) * (i + 1)) * std::sin((2 * i + 1) * c * kPi / order) * sign;
        acc += term;
        sign = -sign;
        if (std::abs(term) < 1e-100)
            break;
    }
    return acc;
}

double seriesDenominator(double q, int order, int c) {
    double acc = 0.0, sign = -1.0;
    for (int i = 1; i < 64; ++i) {
        const double term = std::pow(q, static_cast<double>(i) * i) * std::cos(2 * i * c * kPi / order) * sign;
        acc += term;
        sign = -sign;
        if (std::abs(term) < 1e-100)
            break;
    }
    return acc;
}

} // namespace

HalfBandCoefs designHalfBand(double attenuationDb, double transition) {
    transition = std::clamp(transition, 1e-4, 0.499);
    double k = std::tan((1.0 - transition * 2.0) * kPi / 4.0);
    k *= k;
    const double kk = std::sqrt(std::sqrt(1.0 - k * k));
    const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
    const double e4 = e * e * e * e;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));

    const double attenuation = std::pow(10.0, -attenuationDb / 10.0);
    const double a = attenuation / (1.0 - attenuation);
    int order = static_cast<int>(std::ceil(std::log(a * a / 16.0) / std::log(q)));
    if (order % 2 == 0)
        ++order;
    order = std::clamp(order, 3, 2 * HalfBandCoefs::kMax + 1);

    HalfBandCoefs out;
    out.count = (order - 1) / 2;
    for (int i = 0; i < out.count; ++i) {
        const int c = i + 1;
        const double num = seriesNumerator(q, order, c) * std::pow(q, 0.25);
        const double den = seriesDenominator(q, order, c) + 0.5;
        double ww = num / den;
        ww *= ww;
        const double x = std::sqrt((1.0 - ww * k) * (1.0 - ww / k)) / (1.0 + ww);
        out.c[static_cast<std::size_t>(i)] = static_cast<float>((1.0 - x) / (1.0 + x));
    }
    return out;
}

double halfBandMagnitude(const HalfBandCoefs& coefs, double frequency) {
    // Each chain is a product of all-pass sections in z^2; the second chain is delayed by one sample.
    using C = std::complex<double>;
    const C z1 = std::polar(1.0, -2.0 * kPi * frequency); // z^-1
    const C z2 = z1 * z1;
    C chain[2] = {C(1.0), C(1.0)};
    for (int i = 0; i < coefs.count; ++i) {
        const double c = coefs.c[static_cast<std::size_t>(i)];
        chain[i % 2] *= (c + z2) / (1.0 + c * z2);
    }
    return std::abs(0.5 * (chain[0] + z1 * chain[1]));
}

const DecimatorDesign& decimatorDesign(bool steep) {
    // The last stage passes up to about 0.453 of the base rate (20 kHz at 44.1 kHz). An earlier
    // stage only has to remove what would fold into that pass band, so its transition band may
    // span from 0.5 to 1.5 (4x) or 0.5 to 3.5 (8x) times the base rate.
    static const DecimatorDesign normal = {{designHalfBand(96.0, 0.0465), designHalfBand(96.0, 0.25),
                                            designHalfBand(96.0, 0.375)}};
    static const DecimatorDesign strict = {{designHalfBand(116.0, 0.0465), designHalfBand(116.0, 0.25),
                                            designHalfBand(116.0, 0.375)}};
    return steep ? strict : normal;
}

} // namespace zed::dsp
