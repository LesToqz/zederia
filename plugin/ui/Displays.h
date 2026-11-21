// The display components (plan 3.13): small and large wavetable views (3D and 2D), the filter
// response, the envelope, the output spectrum and the signal map. Displays read what the engine
// reports (live values, filter responses, the output) and redraw only when it changed; static
// parts are cached as images and rebuilt when their inputs or the size change.
#pragma once

#include "Widgets.h"

#include "zederia/dsp/Fft.h"
#include "zederia/engine/Filter.h"

#include <array>

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
    bool sourceOn() const;
    Context& ctx_;
    int source_;
    std::vector<float> cycle_;
    std::uint64_t key_ = 0;
    int ticks_ = 0;
    bool active_ = true; // the power state drawn
};

// The Stage view of an oscillator: its table in 3D (frames in depth, the live position lit) or its
// warped cycle in 2D (what the oscillator plays right now). While the oscillator is on, the trace
// that sounds is crisp and bright over the table's quieter layers; while it is off, the whole
// display is a thin graphite preview. A small 3D / 2D control in its corner switches the Table
// view (Classic always shows its cycle and keeps the Table view for when Table returns); the
// position's percentage is a caption of its own.
class WavetableView final : public Widget {
public:
    WavetableView(Context& ctx, int osc);
    void setOsc(int osc);
    int osc() const { return osc_; }
    bool show3d = true;    // the Table view (kept while the engine is Classic)
    void setShow3d(bool on);
    bool showing3d() const; // what is drawn now: 3D only in Table mode
    bool drawnActive() const { return activeShown_; }
    void draw(juce::Graphics& g) override;
    bool tick() override;
    juce::RectangleList<int> dirtyArea() override { return dirty_; }
    bool hasVisibleLabel() const override { return false; }
    void resized() override;
    Button& viewButton() { return viewButton_; }

private:
    void drawCycle(juce::Graphics& g, juce::Rectangle<float> area, bool active);
    void build3d(juce::Rectangle<int> size, bool active);
    void updateViewButton();
    bool oscOn() const;
    bool classic() const;
    void bindParams();
    int onParam_ = -1, engineParam_ = -1, positionParam_ = -1;
    static constexpr int kLitPoints = 96;
    // The playing position across the surface: kLitPoints + 1 points, the cycle closed.
    std::vector<juce::Point<float>> litPoints(const zed::TableData& table, double position) const;
    juce::Rectangle<int> captionArea() const { return getLocalBounds().reduced(5, 4).removeFromTop(18).removeFromRight(43); }
    Context& ctx_;
    int osc_;
    Button viewButton_{"3D"};
    double position_ = -1.0;
    bool classicShown_ = false;
    bool activeShown_ = true;  // the power state the display shows
    bool cacheActive_ = true;  // the power state the 3D image was drawn for
    int captionPercent_ = -1; // the patch's position the caption shows
    std::vector<juce::Point<float>> litDrawn_; // where the lit line was last painted
    juce::RectangleList<int> dirty_;
    std::uint64_t warpKey_ = 0;
    const zed::TableData* cachedTable_ = nullptr;
    const zed::TableData* tickedTable_ = nullptr;
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
    void setSlot(int slot);
    void togglePhase();
    bool isShowingPhase() const { return showPhase; }
    std::function<void()> onPhaseChanged;
    void draw(juce::Graphics& g) override;
    bool tick() override;
    bool hasVisibleLabel() const override { return false; }
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void compute();
    Context& ctx_;
    int slot_;
    bool showPhase = false;
    std::uint64_t key_ = 0;
    std::vector<double> hz_;
    std::vector<float> live_[2], phase_[2], still_;
    bool on_[2] = {false, false};
    CachedDrawing staticLayer_;
    std::uint64_t patchKey_ = 0;
    unsigned frames_ = 0;
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
    void mouseExit(const juce::MouseEvent&) override;
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
    double values_[9] = {};
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
    static constexpr int kSize = 2048, kBins = 120;
    std::array<float, kSize> scratch_{};
    std::array<double, kSize> window_{};
    std::array<zed::dsp::Fft::Complex, kSize> fft_{};
    std::array<int, kBins + 1> edges_{};
    double binsRate_ = 0.0;
    std::vector<float> smoothed_;
    std::vector<double> responseHz_;
    std::vector<float> responseDb_;
    CachedDrawing axes_;
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

// A source's output over the last seconds, for sources without a shape to draw (Flux, Sub, Texture).
class SourceScope final : public Widget {
public:
    explicit SourceScope(Context& ctx);
    juce::String source;
    bool tick() override;
    void draw(juce::Graphics& g) override;
    bool hasVisibleLabel() const override { return false; }

private:
    Context& ctx_;
    std::vector<float> history_;
    int pos_ = 0;
    int moving_ = 0;
    float last_ = 0.0f;
};

// Short text helpers shared by displays.
juce::String formatHz(double hz);

} // namespace zui
