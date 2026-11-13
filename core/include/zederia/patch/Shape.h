// The shape model (plan 3.7): a list of points joined by curved segments. LFO shapes, drawn
// modulation curves, warp curves and multi-segment envelopes all use it, and they all bend their
// segments with the same curve function.
#pragma once

#include <string>
#include <vector>

namespace zed {

inline constexpr int kMaxShapePoints = 64;
inline constexpr int kMaxEnvPoints = 32;

// Bends the straight line from (0,0) to (1,1). 0 keeps it straight; positive values move fast at
// first and then ease, negative values the reverse; +/-1 is the strongest bend. Mirrored curves
// are exact mirror images: curveValue(x, -c) == 1 - curveValue(1 - x, c).
double curveValue(double x, double curve) noexcept;

struct ShapePoint {
    double x = 0.0;     // 0..1, never decreasing along the list
    double y = 0.0;     // -1..1 for LFO shapes; 0..1 for curves
    double curve = 0.0; // -1..1: bends the segment that starts at this point
    bool operator==(const ShapePoint&) const = default;
};

struct Shape {
    std::vector<ShapePoint> points; // empty means "the built-in shape" of whatever uses it
    double loopStart = 0.0;         // 0..1: where an LFO in loop-point mode starts over
    bool empty() const { return points.empty(); }
    bool operator==(const Shape&) const = default;
};

// The shape's value at x. Left of the first point and right of the last their values are held;
// two points at the same x make a step.
double shapeValue(const Shape& shape, double x) noexcept;

// Fills table[0 .. size] (size + 1 entries) with the shape sampled evenly from x = 0 to x = 1.
void compileShape(const Shape& shape, float* table, int size) noexcept;

// One point of a multi-segment envelope (plan 3.7): the envelope travels from the previous
// point's level to this one's in `timeMs`, or in a note length when `sync` is set ("1/8", "3/16").
struct EnvPoint {
    double timeMs = 0.0;
    std::string sync;
    double level = 0.0; // 0..1
    double curve = 0.0; // -1..1: bends the segment that ends at this point
    bool operator==(const EnvPoint&) const = default;
};

struct EnvShape {
    std::vector<EnvPoint> points; // at most kMaxEnvPoints
    int sustainPoint = -1;        // the envelope waits here while the key is held; -1 = no sustain
    int loopStart = -1, loopEnd = -1; // while the key is held, reaching loopEnd jumps back to loopStart
    bool operator==(const EnvShape&) const = default;
};

} // namespace zed
