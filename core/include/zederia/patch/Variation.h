// Variation (plan 3.11): Randomize, Mutate and Blend. All of them respect locks (a locked
// parameter keeps its value) and never touch the instrument-wide settings (quality, polyphony,
// tuning, output) or the macro positions. Deterministic for a given seed.
#pragma once

#include "zederia/patch/Patch.h"

#include <cstdint>
#include <span>
#include <vector>

namespace zed {

// What Randomize may change.
struct VariationScope {
    bool sources = true;    // oscillators, Sub, Texture
    bool filters = true;    // the two filters and their routing
    bool modulators = true; // envelopes, LFOs, Flux and the amounts of existing routes
    bool effects = true;    // the values of the loaded effects (never the rack's layout)
    bool character = false; // voice character: drift, variance, warmth, noise floor, glide
    bool play = false;      // arpeggiator and chord settings
};

// The group of a parameter for VariationScope; false when no variation may touch it.
enum class VariationGroup { None, Sources, Filters, Modulators, Effects, Character, Play };
VariationGroup variationGroup(int paramIndex);

// Moves each parameter in scope `amount` (0..1) of the way to a random value; a choice changes
// with probability `amount`. Levels stay between -24 and 0 dB and unison below 9 voices, so the
// result stays playable. `locked` is indexed by registry index (empty: nothing locked).
void randomizePatch(Patch& patch, const VariationScope& scope, double amount, std::span<const bool> locked,
                    std::uint64_t seed);

// Small changes around the current sound: continuous values move by a few percent of their range
// (scaled by `amount`, 0..1), choices rarely change. Every group except the play settings.
void mutatePatch(Patch& patch, double amount, std::span<const bool> locked, std::uint64_t seed);

// A new sound built from modules of several parents: each module (an oscillator with its table
// and warp curves, a filter, an envelope with its shape, an LFO with its shape ...) comes whole
// from one parent chosen at random; the rack comes from one parent, the routes and macros from
// one parent. Instrument settings and locked parameters keep the values of `base`.
Patch blendPatches(const Patch& base, std::span<const Patch> parents, std::span<const bool> locked, std::uint64_t seed);

} // namespace zed
