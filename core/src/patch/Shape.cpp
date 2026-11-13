#include "zederia/patch/Shape.h"

#include <algorithm>
#include <cmath>

namespace zed {

double curveValue(double x, double curve) noexcept {
    x = std::clamp(x, 0.0, 1.0);
    const double a = 6.0 * std::clamp(curve, -1.0, 1.0);
    if (std::abs(a) < 1.0e-6)
        return x;
    return (1.0 - std::exp(-a * x)) / (1.0 - std::exp(-a));
}

double shapeValue(const Shape& shape, double x) noexcept {
    const auto& p = shape.points;
    if (p.empty())
        return 0.0;
    if (x <= p.front().x)
        return p.front().y;
    if (x >= p.back().x)
        return p.back().y;
    // The last point at or left of x starts the segment x is in (so a step takes its upper value).
    std::size_t i = 0;
    while (i + 1 < p.size() && p[i + 1].x <= x)
        ++i;
    const ShapePoint& a = p[i];
    const ShapePoint& b = p[std::min(i + 1, p.size() - 1)];
    const double width = b.x - a.x;
    if (width <= 0.0)
        return b.y;
    return a.y + (b.y - a.y) * curveValue((x - a.x) / width, a.curve);
}

void compileShape(const Shape& shape, float* table, int size) noexcept {
    for (int i = 0; i <= size; ++i)
        table[i] = static_cast<float>(shapeValue(shape, static_cast<double>(i) / size));
}

} // namespace zed
