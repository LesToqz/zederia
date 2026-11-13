// Resynthesis (plan 3.10): making tables out of what the instrument itself produces.
#pragma once

#include "zederia/engine/OscKernel.h"
#include "zederia/engine/SampleData.h"
#include "zederia/patch/Patch.h"
#include "zederia/table/TableData.h"
#include "zederia/table/Wavetable.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace zed {

// One cycle exactly as the oscillator plays it at a low pitch: the table at `position` (0..1)
// through both warp slots. This is what the 2D display shows and what Bake warp captures; it
// runs the oscillator's own code, so displays and audio agree (plan 5.7). Modes that need
// another source (pm, am, rm) are left out. `curve`: evenly spaced values of the drawn phase
// curve, empty for the default.
struct WarpSetup {
    osc::WarpMode modeA = osc::WarpMode::Off, modeB = osc::WarpMode::Off;
    double amountA = 0.0, amountB = 0.0;
    std::span<const float> curveA, curveB;
};
Frame renderWarpedCycle(const TableData& table, double position, const WarpSetup& warp);

// Bake warp: the frame at `position` rendered across the warp's whole range into `frames` frames.
Wavetable bakeWarp(const Wavetable& table, double position, osc::WarpMode mode, int frames = kMaxFrames,
                   std::span<const float> curve = {});

// Bake patch: plays the sound for a number of bars and captures it as a table, one frame per
// cycle of the played note (the render runs at a sample rate where a cycle is 2048 samples).
struct BakePatchOptions {
    double bars = 1.0;
    double bpm = 120.0;
    int note = 48;
    int frames = kMaxFrames;
    std::uint64_t seed = 1;
    std::array<std::shared_ptr<const TableData>, kNumOscs> tables; // the patch's tables; null = built-in
    std::shared_ptr<const SampleData> texture;
};
bool bakePatch(const Patch& patch, const BakePatchOptions& options, Wavetable& out, std::string& error);

// Shape to wave: evenly spaced values of a shape (for instance an LFO shape) become one frame.
// A unipolar shape (0..1) is moved to -1..1.
Frame shapeToFrame(std::span<const float> values, bool unipolar);

} // namespace zed
