// The display components (plan 3.13): small and large wavetable views (3D and 2D), the filter
// response, the envelope, the output spectrum and the signal map. Displays read what the engine
// reports (live values, filter responses, the output) and redraw only when it changed; static
// parts are cached as images and rebuilt when their inputs or the size change.
#pragma once

#include "Widgets.h"

#include "zederia/engine/Filter.h"

namespace zui {

// One cycle of a source as it sounds now: an oscillator's table at its live position through its
// warps, Sub's shape, or the Texture sample's outline. Small, for source cards.
class WaveMini final : public Widget {
public:
    // source: 0..2 oscillators, 3 Sub, 4 Texture
    WaveMini(Context& ctx, int source);
    void draw(juce::Graphics& g) override;
    bool tick() override;
    bool hasVisibleLabel() const override { return false; }

private:
    bool rebuild();
    Context& ctx_;
    int source_;
    std::vector<float> cycle_;
    std::uint64_t key_ = 0;
};

// The Stage view of an oscillator: its table in 3D (frames in depth, the live position lit) or its
// warped cycle in 2D (what the oscillator plays right now).
class WavetableView final : public Widget {
public:
    WavetableView(Context& ctx, int osc);
    void setOsc(int osc);
    int osc() const { return osc_; }
    bool show3d = true;
    void draw(juce::Graphics& g) override;
    bool tick() override;
    bool hasVisibleLabel() const override { return false; }
    void mouseDown(const juce::MouseEvent& e) override;
    void resized() override { cache_ = {}; }

private:
    void drawCycle(juce::Graphics& g, juce::Rectangle<float> area);
    void build3d(juce::Rectangle<int> size);
    Context& ctx_;
    int osc_;
    double position_ = -1.0;
    std::uint64_t warpKey_ = 0;
    const zed::TableData* cachedTable_ = nullptr;
    juce::String cachedTheme_;
    juce::Image cache_;
    std::vector<float> cycle_;
    juce::Rectangle<float> plot_;
};

// The response of a filter slot: magnitude (or phase) at the patch's settings, with the live,
// modulated response drawn over it; the other slot faintly behind.
class FilterResponseView final : public Widget {
public:
    FilterResponseView(Context& ctx, int slot);
    void setSlot(int slot) { slot_ = slot; key_ = 0; repaint(); }
    bool showPhase = false;
    void draw(juce::Graphics& g) override;
    bool tick() override;
    bool hasVisibleLabel() const override { return false; }
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void compute();
    Context& ctx_;
    int slot_;
    std::uint64_t key_ = 0;
    std::vector<double> hz_;
    std::vector<float> live_[2], phase_[2], still_;
    bool on_[2] = {false, false};
};

// An envelope's shape from its times, levels and curves, with handles to drag (attack, hold,
// decay and sustain, release) and the live level.
class EnvelopeView final : public Widget {
public:
    EnvelopeView(Context& ctx, int env);
    void setEnv(int env) { env_ = env; repaint(); }
    void draw(juce::Graphics& g) override;
    bool tick() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    bool hasVisibleLabel() const override { return false; }

private:
    struct Geometry {
        float x[6]; // delay end, attack end, hold end, decay end, sustain end (release start), release end
        float totalMs = 1.0f;
    };
    Geometry geometry() const;
    int param(const char* suffix) const;
    Context& ctx_;
    int env_;
    int dragHandle_ = -1, hoverHandle_ = -1;
    double values_[8] = {};
    double dragStart_ = 0.0;
    float level_ = 0.0f;
};

// The output spectrum (and, when given, a response curve over it: the EQ).
class SpectrumView final : public Widget {
public:
    explicit SpectrumView(Context& ctx);
    void draw(juce::Graphics& g) override;
    bool tick() override;
    bool hasVisibleLabel() const override { return false; }
    // Fills dB values at the given frequencies; returns false when there is no curve to draw.
    std::function<bool(const std::vector<double>& hz, std::vector<float>& db)> curve;

private:
    Context& ctx_;
    std::vector<float> smoothed_;
    std::vector<float> scratch_;
    int quietFrames_ = 0;
};

// The signal map (plan 3.13): sources, filters, the direct bus, the lanes and the output, with
// their levels; click a node to jump to its module.
class SignalMap final : public Widget {
public:
    explicit SignalMap(Context& ctx);
    void draw(juce::Graphics& g) override;
    bool tick() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    bool hasVisibleLabel() const override { return false; }
    struct Node {
        juce::String module, name;
        juce::Rectangle<float> box;
        bool on = true;
        juce::String level;
        Family family = Family::Neutral;
    };
    std::vector<Node> nodes() const;

private:
    Context& ctx_;
    std::uint64_t key_ = 0;
    int hover_ = -1;
};

// Short text helpers shared by displays.
juce::String formatHz(double hz);

} // namespace zui
