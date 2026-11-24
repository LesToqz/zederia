// Zederia's mark and wordmark, in one place: the header draws them, the icon tool renders them, and
// a test checks the mark against plugin/brand/zederia-mark.svg, the logo's source.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

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

// --- Icons and installer images, made from the logo's SVG file (zederia-cli brand) ----------------

// The logo as the SVG file draws it, with the square its drawing is placed in (the SVG's width and
// height, which its viewBox fills). Null with a message when the file cannot be read.
struct Logo {
    std::unique_ptr<juce::Drawable> drawable;
    float viewport = 0.0f;
};
Logo loadLogo(const juce::File& svg, juce::String& error);

// The logo filling a transparent square of `size` pixels, drawn 8 times larger and averaged down,
// so every pixel holds the share of it the shape covers.
juce::Image renderIcon(const Logo& logo, int size);

// How the gap between the two slabs comes out in an icon: the most coverage found in the pixel
// rows that lie wholly in the gap (0..1), and the least of the strongest coverage of the slab
// above and of the slab below, in the same columns. Visible: the gap stays mostly clear and both
// slabs mostly solid.
struct GapReading {
    float gap = 1.0f, slabs = 0.0f;
    int gapRows = 0;
    bool visible() const { return gapRows > 0 && gap <= 0.35f && slabs >= 0.75f; }
};
GapReading measureGap(const juce::Image& icon);

// A Windows icon file holding the square images (32-bit with alpha; 256 px stored as PNG).
juce::MemoryBlock iconFile(const std::vector<juce::Image>& images);

// The installer's images: the side image of its welcome and finish pages (the mark over the
// wordmark on the Graphite background), and the small image of its other pages (the mark alone,
// on transparent).
juce::Image installerSideImage(const Logo& logo, int width, int height);
juce::Image installerSmallImage(const Logo& logo, int size);

} // namespace zui::brand
