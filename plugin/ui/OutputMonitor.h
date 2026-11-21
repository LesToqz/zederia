#pragma once

#include "Displays.h"

#include <array>

namespace zui {

// A non-consuming view of the processor's existing mono output history. The compact display
// and its expanded viewer share the same acquisition and the same temporary reference.
class OutputScope final : public Button {
public:
    explicit OutputScope(Context& ctx);
    bool tick() override;
    void draw(juce::Graphics& g) override;
    void drawTraces(juce::Graphics& g, juce::Rectangle<float> plot, float gain) const;
    void capture();
    void clearReference();
    bool hasReference() const { return referenceHeld_; }
    bool hasSignal() const { return live_.peak > 0.0f; }
    juce::String peakText(bool reference) const;
    double spanMs() const { return spanMs_; }
    unsigned revision() const { return revision_; }

private:
    struct Trace {
        static constexpr int columns = 256;
        std::array<float, columns> low{}, high{};
        float peak = 0.0f;
    };
    Context& ctx_;
    Trace live_, reference_;
    std::array<float, 8192> scratch_{};
    double spanMs_ = 12.0;
    unsigned frames_ = 0, revision_ = 0;
    bool referenceHeld_ = false;
};

class OutputMonitor final : public Widget {
public:
    OutputMonitor(Context& ctx, OutputScope& scope);
    void draw(juce::Graphics& g) override;
    bool tick() override;
    juce::RectangleList<int> dirtyArea() override { return traceArea_; }
    void resized() override;
    void visibilityChanged() override;
    Button close{"", Glyph::Close};

private:
    void refresh();
    OutputScope& scope_;
    SpectrumView spectrum_;
    Button capture_{"Capture reference"}, clear_{"Clear"}, zoom_{"4x zoom"};
    juce::Rectangle<int> traceArea_, spectrumArea_;
    unsigned seen_ = 0;
};

} // namespace zui
