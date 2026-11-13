// The four-lane copy of the oscillator kernel.
#include "zederia/dsp/Simd.h"
#include "zederia/engine/OscKernel.h"

#define ZED_KERNEL_SIMD zed::dsp::SimdSse2
#include "OscKernel.inl"
#undef ZED_KERNEL_SIMD

namespace zed::osc {
void renderSse2(Lanes& lanes, const Block& block) { renderImpl(lanes, block); }
} // namespace zed::osc
