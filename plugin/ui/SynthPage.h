// The SYNTH page (plan 3.13): source cards on the left, the Stage in the centre (a large display
// that follows focus), the filters, routing, voice, glide, character, tuning and output on the
// right, and the modulator dock (envelopes, LFOs, Flux, with drag chips) or the keyboard below.
#pragma once

#include "CurveEditor.h"
#include "Shell.h"

namespace zui {

class SourceCard;
class FilterCard;
class VoiceCard;
class Stage;
class ModDock;

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

    // Focus: what the Stage shows.
    void focusSource(int source); // 0..2 oscillators, 3 Sub, 4 Texture
    void focusFilter(int slot);
    void focusModulator(const juce::String& source); // "env2", "lfo3", "flux1"
    void focusWarpCurve(int osc, int slot);
    void showSignalMap();
    void expandSource(int source);
    int expandedSource() const { return expanded_; }
    Stage& stage() { return *stage_; }
    ModDock& dock() { return *dock_; }
    bool keyboardShown() const;
    void setKeyboardShown(bool shown);

private:
    std::vector<std::unique_ptr<SourceCard>> sources_;
    std::vector<std::unique_ptr<FilterCard>> filters_;
    std::unique_ptr<VoiceCard> voice_;
    std::unique_ptr<Stage> stage_;
    std::unique_ptr<ModDock> dock_;
    std::unique_ptr<juce::Component> ticker_;
    int expanded_ = 0;
};

} // namespace zui
