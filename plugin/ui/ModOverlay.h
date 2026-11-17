// Seeing and editing modulation where it lives (plan 3.7): the hover list that appears beside a
// modulated control (its sources with editable amounts, polarity, mute, solo and remove), and the
// mod map that draws every active connection at once.
#pragma once

#include "Widgets.h"

namespace zui {

class ModHoverList final : public juce::Component {
public:
    explicit ModHoverList(Context& ctx);
    // Shows the routes of a control; false when it has none.
    bool showFor(ParamWidget& target);
    ParamWidget* target() const { return target_.getComponent(); }
    void rebuild();
    void paint(juce::Graphics& g) override;
    void resized() override;
    int preferredHeight() const;

private:
    struct Row {
        int route = -1;
        std::unique_ptr<Button> source, polarity, mute, solo, remove;
        std::unique_ptr<Fader> amount;
    };
    Context& ctx_;
    juce::Component::SafePointer<ParamWidget> target_;
    std::vector<std::unique_ptr<Row>> rows_;
    int generation_ = -1;
};

class ModMapOverlay final : public juce::Component {
public:
    explicit ModMapOverlay(Context& ctx);
    void paint(juce::Graphics& g) override;
    // The tree whose controls and chips are connected (the page plus the macro strip and dock).
    juce::Component* scope = nullptr;
    int connectionsDrawn() const { return drawn_; }
    juce::StringArray offPage() const { return offPage_; }

private:
    Context& ctx_;
    int drawn_ = 0;
    juce::StringArray offPage_;
};

} // namespace zui
