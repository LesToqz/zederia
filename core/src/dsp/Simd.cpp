#include "zederia/dsp/Simd.h"

#if ZED_SIMD_X86 && defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#endif

namespace zed::dsp {
namespace {

bool detectAvx2() noexcept {
#if ZED_SIMD_X86 && defined(_MSC_VER)
    int info[4] = {0, 0, 0, 0};
    __cpuid(info, 0);
    if (info[0] < 7)
        return false;
    __cpuid(info, 1);
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    const bool avx = (info[2] & (1 << 28)) != 0;
    if (!osxsave || !avx)
        return false;
    if ((_xgetbv(0) & 0x6) != 0x6) // the operating system saves the XMM and YMM registers
        return false;
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
#elif ZED_SIMD_X86 && (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("avx2") != 0;
#else
    return false;
#endif
}

} // namespace

bool cpuHasAvx2() noexcept {
    static const bool has = detectAvx2();
    return has;
}

} // namespace zed::dsp
