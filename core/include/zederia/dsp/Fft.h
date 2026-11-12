// A small complex FFT for power-of-two sizes, in double precision. It serves the wavetable
// toolkit and the table builder, which run off the audio thread; nothing here is used inside the
// audio callback.
#pragma once

#include <complex>
#include <vector>

namespace zed::dsp {

class Fft {
public:
    using Complex = std::complex<double>;

    explicit Fft(int size); // size must be a power of two, 2 or more
    int size() const { return size_; }

    void forward(Complex* data) const; // X[k] = sum x[n] e^(-2 pi i k n / N)
    void inverse(Complex* data) const; // x[n] = (1/N) sum X[k] e^(+2 pi i k n / N)

    // A plan for `size`, created on first use and kept for the life of the program.
    // Safe from any non-audio thread.
    static const Fft& forSize(int size);

private:
    void transform(Complex* data, bool inverse) const;

    int size_ = 0;
    std::vector<int> reversed_;
    std::vector<Complex> twiddles_; // e^(-2 pi i k / N), k < N / 2
};

// Discrete Fourier transform of any length (Bluestein's method on top of the power-of-two FFT):
// out[k] = sum in[n] e^(-2 pi i k n / N).
std::vector<Fft::Complex> dftAnyLength(const std::vector<Fft::Complex>& in);

bool isPowerOfTwo(int n);
int nextPowerOfTwo(int n); // smallest power of two that is >= n (and >= 1)

} // namespace zed::dsp
