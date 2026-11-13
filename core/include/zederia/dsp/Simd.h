// A thin wrapper over SIMD intrinsics (plan 5.1): one lane type per instruction set, all with the
// same operations and the same IEEE results, so a kernel written once against this interface
// gives bit-identical output whether it runs one, four or eight unison lanes at a time.
//
// Rules that keep the paths identical: only add, subtract, multiply, divide, compare, select and
// truncation are offered; there is no fused multiply-add and no fast-math; min and max follow the
// SSE definition (a < b ? a : b, a > b ? a : b).
//
//   SimdScalar  one lane, plain floats: the reference path
//   SimdSse2    four lanes (x86 baseline). On other architectures it is a portable four-lane
//               fallback written with plain arrays; NEON intrinsics join when an ARM target does.
//   SimdAvx2    eight lanes; only available in translation units compiled for AVX2
#pragma once

#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define ZED_SIMD_X86 1
#include <emmintrin.h>
#else
#define ZED_SIMD_X86 0
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace zed::dsp {

struct SimdScalar {
    static constexpr int width = 1;
    using F = float;
    using I = std::int32_t;
    using M = bool;

    static F load(const float* p) { return *p; }
    static void store(float* p, F v) { *p = v; }
    static I loadI(const std::uint32_t* p) { return static_cast<I>(*p); }
    static void storeI(std::uint32_t* p, I v) { *p = static_cast<std::uint32_t>(v); }
    static F set1(float v) { return v; }
    static F add(F a, F b) { return a + b; }
    static F sub(F a, F b) { return a - b; }
    static F mul(F a, F b) { return a * b; }
    static F div(F a, F b) { return a / b; }
    static F min(F a, F b) { return a < b ? a : b; }
    static F max(F a, F b) { return a > b ? a : b; }
    static F abs(F a) { return a < 0.0f ? -a : (a == 0.0f ? 0.0f : a); }
    static M lt(F a, F b) { return a < b; }
    static F select(M m, F a, F b) { return m ? a : b; }
    static I toInt(F a) { return static_cast<I>(a); } // truncates toward zero
    static F toFloat(I a) { return static_cast<F>(a); }
    static I addI(I a, I b) { return static_cast<I>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)); }
    static I shiftRight(I a, int bits) { return static_cast<I>(static_cast<std::uint32_t>(a) >> bits); }
    static F gather(const float* base, I index) { return base[index]; }
    static void toArray(F v, float* out) { *out = v; }
};

#if ZED_SIMD_X86

struct SimdSse2 {
    static constexpr int width = 4;
    using F = __m128;
    using I = __m128i;
    using M = __m128;

    static F load(const float* p) { return _mm_loadu_ps(p); }
    static void store(float* p, F v) { _mm_storeu_ps(p, v); }
    static I loadI(const std::uint32_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
    static void storeI(std::uint32_t* p, I v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }
    static F set1(float v) { return _mm_set1_ps(v); }
    static F add(F a, F b) { return _mm_add_ps(a, b); }
    static F sub(F a, F b) { return _mm_sub_ps(a, b); }
    static F mul(F a, F b) { return _mm_mul_ps(a, b); }
    static F div(F a, F b) { return _mm_div_ps(a, b); }
    static F min(F a, F b) { return _mm_min_ps(a, b); }
    static F max(F a, F b) { return _mm_max_ps(a, b); }
    static F abs(F a) { return _mm_andnot_ps(_mm_set1_ps(-0.0f), a); }
    static M lt(F a, F b) { return _mm_cmplt_ps(a, b); }
    static F select(M m, F a, F b) { return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b)); }
    static I toInt(F a) { return _mm_cvttps_epi32(a); }
    static F toFloat(I a) { return _mm_cvtepi32_ps(a); }
    static I addI(I a, I b) { return _mm_add_epi32(a, b); }
    static I shiftRight(I a, int bits) { return _mm_srli_epi32(a, bits); }
    static F gather(const float* base, I index) {
        alignas(16) std::int32_t i[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(i), index);
        return _mm_set_ps(base[i[3]], base[i[2]], base[i[1]], base[i[0]]);
    }
    static void toArray(F v, float* out) { _mm_storeu_ps(out, v); }
};

#else

// Portable four-lane fallback with the same semantics.
struct SimdSse2 {
    static constexpr int width = 4;
    struct F { float v[4]; };
    struct I { std::int32_t v[4]; };
    struct M { bool v[4]; };

#define ZED_LANES(expr)                                                                                               \
    F r;                                                                                                              \
    for (int k = 0; k < 4; ++k)                                                                                       \
        r.v[k] = (expr);                                                                                              \
    return r
    static F load(const float* p) { ZED_LANES(p[k]); }
    static void store(float* p, F a) { for (int k = 0; k < 4; ++k) p[k] = a.v[k]; }
    static I loadI(const std::uint32_t* p) { I r; for (int k = 0; k < 4; ++k) r.v[k] = static_cast<std::int32_t>(p[k]); return r; }
    static void storeI(std::uint32_t* p, I a) { for (int k = 0; k < 4; ++k) p[k] = static_cast<std::uint32_t>(a.v[k]); }
    static F set1(float x) { ZED_LANES(x); }
    static F add(F a, F b) { ZED_LANES(a.v[k] + b.v[k]); }
    static F sub(F a, F b) { ZED_LANES(a.v[k] - b.v[k]); }
    static F mul(F a, F b) { ZED_LANES(a.v[k] * b.v[k]); }
    static F div(F a, F b) { ZED_LANES(a.v[k] / b.v[k]); }
    static F min(F a, F b) { ZED_LANES(a.v[k] < b.v[k] ? a.v[k] : b.v[k]); }
    static F max(F a, F b) { ZED_LANES(a.v[k] > b.v[k] ? a.v[k] : b.v[k]); }
    static F abs(F a) { ZED_LANES(SimdScalar::abs(a.v[k])); }
    static M lt(F a, F b) { M r; for (int k = 0; k < 4; ++k) r.v[k] = a.v[k] < b.v[k]; return r; }
    static F select(M m, F a, F b) { ZED_LANES(m.v[k] ? a.v[k] : b.v[k]); }
    static I toInt(F a) { I r; for (int k = 0; k < 4; ++k) r.v[k] = static_cast<std::int32_t>(a.v[k]); return r; }
    static F toFloat(I a) { ZED_LANES(static_cast<float>(a.v[k])); }
    static I addI(I a, I b) { I r; for (int k = 0; k < 4; ++k) r.v[k] = SimdScalar::addI(a.v[k], b.v[k]); return r; }
    static I shiftRight(I a, int bits) { I r; for (int k = 0; k < 4; ++k) r.v[k] = SimdScalar::shiftRight(a.v[k], bits); return r; }
    static F gather(const float* base, I index) { ZED_LANES(base[index.v[k]]); }
    static void toArray(F a, float* out) { for (int k = 0; k < 4; ++k) out[k] = a.v[k]; }
#undef ZED_LANES
};

#endif

#if defined(__AVX2__)

struct SimdAvx2 {
    static constexpr int width = 8;
    using F = __m256;
    using I = __m256i;
    using M = __m256;

    static F load(const float* p) { return _mm256_loadu_ps(p); }
    static void store(float* p, F v) { _mm256_storeu_ps(p, v); }
    static I loadI(const std::uint32_t* p) { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)); }
    static void storeI(std::uint32_t* p, I v) { _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), v); }
    static F set1(float v) { return _mm256_set1_ps(v); }
    static F add(F a, F b) { return _mm256_add_ps(a, b); }
    static F sub(F a, F b) { return _mm256_sub_ps(a, b); }
    static F mul(F a, F b) { return _mm256_mul_ps(a, b); }
    static F div(F a, F b) { return _mm256_div_ps(a, b); }
    static F min(F a, F b) { return _mm256_min_ps(a, b); }
    static F max(F a, F b) { return _mm256_max_ps(a, b); }
    static F abs(F a) { return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a); }
    static M lt(F a, F b) { return _mm256_cmp_ps(a, b, _CMP_LT_OQ); }
    static F select(M m, F a, F b) { return _mm256_blendv_ps(b, a, m); }
    static I toInt(F a) { return _mm256_cvttps_epi32(a); }
    static F toFloat(I a) { return _mm256_cvtepi32_ps(a); }
    static I addI(I a, I b) { return _mm256_add_epi32(a, b); }
    static I shiftRight(I a, int bits) { return _mm256_srli_epi32(a, bits); }
    static F gather(const float* base, I index) { return _mm256_i32gather_ps(base, index, 4); }
    static void toArray(F v, float* out) { _mm256_storeu_ps(out, v); }
};

#endif

// True when the processor and the operating system support AVX2 (checked once).
bool cpuHasAvx2() noexcept;

} // namespace zed::dsp
