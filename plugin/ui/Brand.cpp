#include "Brand.h"

namespace zui::brand {

juce::Colour markColour() { return juce::Colour(0xffefb96bu); }

juce::Rectangle<float> markViewBox() { return {9.8f, 10.3f, 110.4f, 110.4f}; }

juce::Path mark() {
    // The same points as plugin/brand/zederia-mark.svg (checked by a test).
    juce::Path p;
    p.startNewSubPath(17, 26); p.lineTo(110, 26); p.lineTo(110, 43); p.lineTo(49, 86);
    p.lineTo(24, 86); p.lineTo(24, 80); p.lineTo(82, 39); p.lineTo(17, 39); p.closeSubPath();
    p.startNewSubPath(18, 94); p.lineTo(81, 94); p.lineTo(96, 82); p.lineTo(113, 82);
    p.lineTo(113, 91); p.lineTo(96, 105); p.lineTo(18, 105); p.closeSubPath();
    p.setUsingNonZeroWinding(false);
    return p;
}

juce::Path Wordmark::letters() const {
    juce::Path all(firstE);
    all.addPath(d);
    all.addPath(rest);
    return all;
}

Wordmark wordmark() {
    Wordmark w;
    const auto e = [](juce::Path& p, float x) {
        p.startNewSubPath(x + 11, 0); p.lineTo(x, 0); p.lineTo(x, 16); p.lineTo(x + 11, 16);
        p.startNewSubPath(x, 8); p.lineTo(x + 8, 8);
    };
    e(w.firstE, 28);
    w.d.startNewSubPath(45, 0); w.d.lineTo(53, 0); w.d.lineTo(58, 4); w.d.lineTo(58, 12); w.d.lineTo(53, 16); w.d.lineTo(45, 16);
    w.d.closeSubPath();
    e(w.rest, 64);
    w.rest.startNewSubPath(81, 16); w.rest.lineTo(81, 0); w.rest.lineTo(91, 0); w.rest.lineTo(94, 3);
    w.rest.lineTo(94, 6); w.rest.lineTo(91, 9); w.rest.lineTo(82, 9);
    w.rest.startNewSubPath(88, 9); w.rest.lineTo(95, 16);
    w.rest.startNewSubPath(101, 0); w.rest.lineTo(101, 16);
    w.rest.startNewSubPath(107, 16); w.rest.lineTo(113, 0); w.rest.lineTo(116, 0); w.rest.lineTo(122, 16);
    w.rest.startNewSubPath(110, 10); w.rest.lineTo(119, 10);
    return w;
}

juce::PathStrokeType wordmarkStroke() { return {2.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::square}; }

juce::AffineTransform markBesideLetters(const Wordmark& word) {
    const auto stroke = wordmarkStroke();
    const auto inked = [&](const juce::Path& p) {
        juce::Path outline;
        stroke.createStrokedPath(outline, p);
        return outline.getBounds();
    };
    const auto capital = inked(word.firstE), shape = mark().getBounds();
    const float scale = capital.getHeight() / shape.getHeight();
    const float markRight = capital.getX() - (inked(word.d).getX() - capital.getRight());
    return juce::AffineTransform::translation(-shape.getRight(), -shape.getY()).scaled(scale).translated(markRight, capital.getY());
}

} // namespace zui::brand
