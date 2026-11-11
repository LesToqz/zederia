#pragma once

#include "zederia/params/ParamDef.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace zed {

inline constexpr double kPi = 3.14159265358979323846;

inline double dbToGain(double db) { return db <= kSilenceDb ? 0.0 : std::pow(10.0, db / 20.0); }
inline double gainToDb(double gain) { return gain <= 0.0 ? -200.0 : 20.0 * std::log10(gain); }

// Frequency of a (fractional) MIDI note for a given reference A.
inline double noteToHz(double note, double referenceHz = 440.0) {
    return referenceHz * std::exp2((note - 69.0) / 12.0);
}

// Small, fast, seedable generator. The same seed gives the same sequence on every platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed = 1) : state_(seed) {}
    void seed(std::uint64_t seed) { state_ = seed; }
    std::uint64_t next() { // splitmix64
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); } // [0, 1)

private:
    std::uint64_t state_;
};

// FNV-1a, 64 bit.
inline std::uint64_t fnv1a(const void* data, std::size_t size, std::uint64_t hash = 0xcbf29ce484222325ull) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

} // namespace zed
