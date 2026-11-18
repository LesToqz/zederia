// The SYNTH page (plan 3.13): the three oscillators side by side, each with its own display;
// Sub, Texture and the Stage (filter response, warp curves, the signal map) below them; the
// filters, routing, voice, glide, character, tuning and output on the right. The modulators and
// macros are in the dock under every page.
#pragma once

#include "CurveEditor.h"
#include "Shell.h"

namespace zui {

class SourceCard;
class FilterCard;
class VoiceCard;
class Stage;
class MapCard;

class SynthPage final : public Page {
public:
    explicit SynthPage(Context& ctx);
    ~SynthPage() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool reveal(const juce::String& module) override;
    void applySimpleView() override;
    void saveState(nlohmann::json& state) const override;
    void loadState(const nlohmann::json& state) override;
    void structureChanged() override;

    // Focus: what is highlighted and what the Stage shows.
    void focusSource(int source); // 0..2 oscillators, 3 Sub, 4 Texture
    void focusFilter(int slot);
    void focusWarpCurve(int osc, int slot);
    void showSignalMap();
    void expandSource(int source); // the same as focusSource (the cards no longer collapse)
    int expandedSource() const { return expanded_; }
    Stage& stage() { return *stage_; }

private:
    std::vector<std::unique_ptr<SourceCard>> sources_;
    std::vector<std::unique_ptr<FilterCard>> filters_;
    std::unique_ptr<VoiceCard> voice_;
    std::unique_ptr<Stage> stage_;
    std::unique_ptr<MapCard> map_; // the signal map, over the oscillators when shown
    std::unique_ptr<juce::Component> ticker_;
    int expanded_ = 0;
};

} // namespace zui
