// The curve editor shared by LFO shapes, multi-segment envelopes, warp curves, Drive's transfer
// curves and drawn route curves (plan 3.7, 3.10, 3.8). Points with curve handles; a grid with
// separate X and Y divisions and snapping; step drawing; multi-select with proportional moves;
// a small shape library; copy and paste. Edits are live while the pointer moves (onChange) and
// become one undo step when it is released (onCommit).
#pragma once

#include "Widgets.h"

namespace zui {

class CurveEditor final : public Widget {
public:
    struct Config {
        bool bipolar = true;       // y from -1 to 1 (LFO), otherwise 0 to 1
        bool fixedEnds = false;    // the first point stays at x 0 and the last at x 1 (warp and route curves)
        bool anchoredStart = false; // the first point is fixed at (0, 0) (envelopes start from silence)
        bool loopPoint = false;    // an LFO's loop start marker
        bool envelopeMarkers = false; // sustain point and loop region (multi-segment envelopes)
        int maxPoints = zed::kMaxShapePoints;
        juce::String xUnit;        // shown under the grid ("ms" for envelopes)
        double xScale = 1.0;       // x 0..1 shown as 0..xScale in xUnit
    };

    explicit CurveEditor(Context& ctx, juce::String name);
    void setConfig(const Config& c) { config_ = c; repaint(); }
    const Config& config() const { return config_; }
    void setShape(const zed::Shape& shape);
    const zed::Shape& shape() const { return shape_; }

    // Envelope markers, as point indices of the shape (-1: none).
    int sustainPoint = -1, loopStart = -1, loopEnd = -1;
    std::function<void(int sustain, int loopStart, int loopEnd)> onMarkers;

    std::function<void(const zed::Shape&)> onChange;                        // while dragging
    std::function<void(const zed::Shape&, const juce::String& label)> onCommit; // one undo step
    std::function<double()> playhead;    // live x position 0..1, or < 0
    std::function<void()> onCopy, onPaste;

    int gridX = 8, gridY = 4;
    bool snap = true;
    bool stepMode = false;
    // The curve's ink. A modulator's shape is drawn in its source's colour (its chip and its arcs);
    // curves that shape a signal (warps, drive, route curves) in the display's bright line.
    std::optional<juce::Colour> ink;
    std::vector<int> selection() const { return selected_; }
    void selectAll();
    void deleteSelected();
    void applyPreset(const juce::String& name); // "Sine", "Triangle", "Saw up", "Saw down", "Square", "Random steps", "Line", "S-curve"
    static juce::StringArray presetNames();

    void draw(juce::Graphics& g) override;
    bool tick() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    // Geometry, public for tests.
    juce::Rectangle<float> plotArea() const;
    juce::Point<float> toScreen(double x, double y) const;
    juce::Point<double> fromScreen(juce::Point<float> p) const;

private:
    enum class Hit { None, Point, Handle, Segment };
    Hit hitTest(juce::Point<float> p, int& index) const;
    double snapX(double x) const;
    double snapY(double y) const;
    double yLow() const { return config_.bipolar ? -1.0 : 0.0; }
    void changed();
    void commit(const juce::String& label);
    void showMenu(juce::Point<float> at, int point);
    void sortAndClamp();

    Context& ctx_;
    juce::String name_;
    Config config_;
    zed::Shape shape_;
    std::vector<int> selected_;
    int dragPoint_ = -1, dragHandle_ = -1, hoverPoint_ = -1, hoverHandle_ = -1;
    bool rubber_ = false, drawing_ = false, moved_ = false;
    juce::Point<float> rubberStart_, rubberEnd_;
    std::vector<zed::ShapePoint> dragOrigin_;
    juce::Point<double> dragAnchor_;
    double handleStartCurve_ = 0.0;
    float playhead_ = -1.0f;
};

} // namespace zui
