// Zederia's mark and wordmark, in one place: the header draws them, the icon tool renders them, and
// a test checks the mark against plugin/brand/zederia-mark.svg, the logo's source.
#pragma once

#include <juce_graphics/juce_graphics.h>

namespace zui::brand {

// The mark's colour (#efb96b), the same as the theme's lamp colour.
juce::Colour markColour();
// The logo's square, in the units of its drawing: x and y from 9.8 and 10.3, 110.4 wide and high.
juce::Rectangle<float> markViewBox();
// The cut Z: two slabs, filled even-odd, in the units of markViewBox().
juce::Path mark();

// The six engraved letters of the wordmark after the mark, as strokes in 16-unit-high capitals
// starting at x = 28: the first E, the D (the mark is spaced from them), and the rest.
struct Wordmark {
    juce::Path firstE, d, rest;
    juce::Path letters() const; // all three together
};
Wordmark wordmark();
// The stroke the letters are drawn with.
juce::PathStrokeType wordmarkStroke();

// The mark fitted to the inked capitals of the wordmark, as the header shows it: as tall as the E
// from its top edge to its baseline, and as far from the E as the E is from the D. Returns the
// transform that places the mark (in its own units) beside letters drawn at the origin.
juce::AffineTransform markBesideLetters(const Wordmark& word);

} // namespace zui::brand
