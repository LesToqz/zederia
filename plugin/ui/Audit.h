// Snapshots and the automatic interface checks (plan 6.1): a component tree painted offscreen at a
// chosen scale (software rendering, no window, no GPU), and checks for clipped, overlapping or
// unlabeled controls, truncated or too small text, and theme colours with too little contrast.
#pragma once

#include "Theme.h"

#include <vector>

namespace zui {

struct Finding {
    juce::String kind;   // "clipped", "overlap", "truncated", "small_text", "unlabeled", "zero_size", "contrast"
    juce::String where;  // component path ("Root/SynthPage/Osc 1/Position")
    juce::String detail;
};

// A readable path to a component: its titles or names from the top.
juce::String componentPath(const juce::Component& c);

// Paints `root` (already at its final size) into an image, with text recording on. Text that did
// not fit, or was smaller than 11 px at 100 %, is added to `findings` when given.
juce::Image renderSnapshot(juce::Component& root, std::vector<Finding>* findings = nullptr);
bool writePng(const juce::Image& image, const juce::File& file);

// Layout checks on the visible tree under `root`.
std::vector<Finding> checkLayout(juce::Component& root);
// Contrast of the theme's text and control colours on its surfaces (WCAG: 4.5 for text, 3 for
// graphics; the Contrast theme asks 7 for text).
std::vector<Finding> checkTheme(const Theme& theme);

// Marks a component as an overlay (popovers, the mod map): it may cover its siblings.
void markOverlay(juce::Component& c);
// Marks a component whose children may extend past it (scrolling content).
void markScrolling(juce::Component& c);

} // namespace zui
