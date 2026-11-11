// Flush-to-zero and denormals-are-zero for the duration of the audio callback (plan 5.3).
// This is one of the few places with platform-specific code.
#pragma once

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#define ZED_HAS_MXCSR 1
#else
#define ZED_HAS_MXCSR 0
#endif

namespace zed::rt {

class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept {
#if ZED_HAS_MXCSR
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u); // FTZ | DAZ
#elif defined(__aarch64__)
        __asm__ volatile("mrs %0, fpcr" : "=r"(saved_));
        const unsigned long long flushed = saved_ | (1ull << 24);
        __asm__ volatile("msr fpcr, %0" : : "r"(flushed));
#endif
    }
    ~ScopedNoDenormals() {
#if ZED_HAS_MXCSR
        _mm_setcsr(saved_);
#elif defined(__aarch64__)
        __asm__ volatile("msr fpcr, %0" : : "r"(saved_));
#endif
    }
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
#if ZED_HAS_MXCSR
    unsigned int saved_ = 0;
#else
    unsigned long long saved_ = 0;
#endif
};

} // namespace zed::rt
