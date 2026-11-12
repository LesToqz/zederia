#include "zederia/dsp/Fft.h"

#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace zed::dsp {

bool isPowerOfTwo(int n) { return n > 0 && (n & (n - 1)) == 0; }

int nextPowerOfTwo(int n) {
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

Fft::Fft(int size) : size_(size) {
    assert(isPowerOfTwo(size) && size >= 2);
    reversed_.resize(static_cast<std::size_t>(size));
    int bits = 0;
    while ((1 << bits) < size)
        ++bits;
    for (int i = 0; i < size; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        reversed_[static_cast<std::size_t>(i)] = r;
    }
    twiddles_.resize(static_cast<std::size_t>(size / 2));
    for (int k = 0; k < size / 2; ++k) {
        const double angle = -2.0 * kPi * k / size;
        twiddles_[static_cast<std::size_t>(k)] = {std::cos(angle), std::sin(angle)};
    }
}

void Fft::transform(Complex* data, bool inverse) const {
    const int n = size_;
    for (int i = 0; i < n; ++i) {
        const int j = reversed_[static_cast<std::size_t>(i)];
        if (j > i)
            std::swap(data[i], data[j]);
    }
    // Work on the raw doubles: std::complex multiplication carries checks that are not needed here.
    auto* d = reinterpret_cast<double*>(data);
    const auto* tw = reinterpret_cast<const double*>(twiddles_.data());
    const double sign = inverse ? -1.0 : 1.0;
    for (int half = 1; half < n; half <<= 1) {
        const int step = n / (half * 2);
        for (int start = 0; start < n; start += half * 2) {
            for (int k = 0; k < half; ++k) {
                const double wr = tw[2 * k * step];
                const double wi = sign * tw[2 * k * step + 1];
                double* a = d + 2 * (start + k);
                double* b = d + 2 * (start + k + half);
                const double tr = b[0] * wr - b[1] * wi;
                const double ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr;
                b[1] = a[1] - ti;
                a[0] += tr;
                a[1] += ti;
            }
        }
    }
    if (inverse) {
        const double scale = 1.0 / n;
        for (int i = 0; i < 2 * n; ++i)
            d[i] *= scale;
    }
}

void Fft::forward(Complex* data) const { transform(data, false); }
void Fft::inverse(Complex* data) const { transform(data, true); }

std::vector<Fft::Complex> dftAnyLength(const std::vector<Fft::Complex>& in) {
    using Complex = Fft::Complex;
    const int n = static_cast<int>(in.size());
    if (n <= 1)
        return in;
    if (isPowerOfTwo(n)) {
        std::vector<Complex> out = in;
        Fft::forSize(n).forward(out.data());
        return out;
    }
    // k n = (k^2 + n^2 - (k - n)^2) / 2 turns the transform into a convolution with a chirp.
    const int m = nextPowerOfTwo(2 * n - 1);
    std::vector<Complex> chirp(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto square = static_cast<std::int64_t>(i) * i % (2 * static_cast<std::int64_t>(n)); // keeps the angle small
        const double angle = kPi * static_cast<double>(square) / n;
        chirp[static_cast<std::size_t>(i)] = {std::cos(angle), std::sin(angle)};
    }
    std::vector<Complex> a(static_cast<std::size_t>(m)), b(static_cast<std::size_t>(m));
    for (int i = 0; i < n; ++i) {
        a[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(i)] * std::conj(chirp[static_cast<std::size_t>(i)]);
        b[static_cast<std::size_t>(i)] = chirp[static_cast<std::size_t>(i)];
        if (i > 0)
            b[static_cast<std::size_t>(m - i)] = chirp[static_cast<std::size_t>(i)];
    }
    const Fft& fft = Fft::forSize(m);
    fft.forward(a.data());
    fft.forward(b.data());
    for (int i = 0; i < m; ++i)
        a[static_cast<std::size_t>(i)] *= b[static_cast<std::size_t>(i)];
    fft.inverse(a.data());
    std::vector<Complex> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i)] = a[static_cast<std::size_t>(i)] * std::conj(chirp[static_cast<std::size_t>(i)]);
    return out;
}

const Fft& Fft::forSize(int size) {
    static rt::Mutex mutex;
    static std::map<int, std::unique_ptr<Fft>> plans;
    const std::lock_guard<rt::Mutex> lock(mutex);
    auto& plan = plans[size];
    if (!plan)
        plan = std::make_unique<Fft>(size);
    return *plan;
}

} // namespace zed::dsp
