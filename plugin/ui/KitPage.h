// The component gallery: every part of the kit on one page, bound to real parameters. Not one of
// the seven pages; the snapshot tool renders it (`zederia-cli ui-snapshot --page kit`) so the kit
// can be reviewed and checked on its own.
#pragma once

#include "CurveEditor.h"
#include "Shell.h"

namespace zui {

class KitPage final : public Page {
public:
    explicit KitPage(Context& ctx);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    std::vector<std::unique_ptr<juce::Component>> parts_;
    Card knobs{"Knobs and faders", Family::Sources}, choices{"Switches and choices", Family::Filters}, buttons{"Buttons, tabs, chips", Family::Modulators};
    Card curves{"Curve editor", Family::Modulators}, displays{"Displays", Family::Effects};
    template <class T>
    T& add(juce::Component& parent, std::unique_ptr<T> part) {
        T& ref = *part;
        parent.addAndMakeVisible(ref);
        parts_.push_back(std::move(part));
        return ref;
    }
    std::vector<juce::Component*> knobRow_, choiceRow_, buttonRow_, displayRow_;
    CurveEditor* curve_ = nullptr;
};

} // namespace zui
