// The eight-lane copy of the oscillator kernel. This file alone is compiled for AVX2 (see
// core/CMakeLists.txt); it includes nothing that could put AVX2 code into a function shared with
// other files. It is only called after cpuHasAvx2() said yes.
#include "zederia/dsp/Simd.h"
#include "zederia/engine/OscKernel.h"

#if defined(__AVX2__)

#define ZED_KERNEL_SIMD zed::dsp::SimdAvx2
#include "OscKernel.inl"
#undef ZED_KERNEL_SIMD

namespace zed::osc {
void renderAvx2(Lanes& lanes, const Block& block) { renderImpl(lanes, block); }
} // namespace zed::osc

#else

namespace zed::osc {
void renderAvx2(Lanes& lanes, const Block& block) { renderSse2(lanes, block); }
} // namespace zed::osc

#endif
