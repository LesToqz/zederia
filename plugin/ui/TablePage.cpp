#include "TablePage.h"

#include "Audit.h"
#include "Displays.h"

#include "zederia/audio/WavFile.h"
#include "zederia/table/Formula.h"
#include "zederia/table/Import.h"
#include "zederia/table/Resynth.h"
#include "zederia/table/TableData.h"
#include "zederia/util/Math.h"

#include <nlohmann/json.hpp>

namespace zui {

namespace {

const zed::Registry& reg() { return zed::Registry::get(); }

juce::Path framePath(const zed::Frame& f, juce::Rectangle<float> area, int points) {
    juce::Path p;
    float peak = 0.0f;
    for (const float s : f)
        peak = std::max(peak, std::abs(s));
    const float scale = peak > 1.0f ? 1.0f / peak : 1.0f;
    for (int i = 0; i <= points; ++i) {
        const int at = std::min(zed::kFrameSize - 1, i * zed::kFrameSize / points);
        const float x = area.getX() + area.getWidth() * static_cast<float>(i) / static_cast<float>(points);
        const float y = area.getCentreY() - f[static_cast<std::size_t>(at)] * scale * area.getHeight() * 0.46f;
        if (i == 0)
            p.startNewSubPath(x, y);
        else
            p.lineTo(x, y);
    }
    return p;
}

struct FactoryFormula {
    const char* name;
    int domain; // 0 wave, 1 harmonics
    const char* expression;
};
const FactoryFormula kFormulas[] = {
    {"Sine to saw across the table", 0, "sin(tau*t) * (1 - y) + saw(t) * y"},
    {"Pulse width sweep", 0, "t < 0.5 - 0.45 * y ? 1 : -1"},
    {"Folded sine", 0, "sin(tau*t*(1 + 4*y))"},
    {"Soft square", 0, "tanh(sin(tau*t) * (1 + 12*y))"},
    {"Formant sweep", 0, "sin(tau*t) * exp(-3*t) * sin(tau*t*(2 + 10*y))"},
    {"Odd harmonics", 1, "1/h * (h % 2)"},
    {"Saw series", 1, "1/h"},
    {"Bright to dark", 1, "1/h * exp(-h * y / 8)"},
    {"Comb of partials", 1, "(h % 4 == 1) / sqrt(h)"},
    {"Random partials", 1, "abs(rand) / h"},
};

} // namespace

// --- Frames strip ------------------------------------------------------------------------------------

class FrameStrip final : public Widget {
public:
    static constexpr int kCell = 58;
    explicit FrameStrip(TablePage& page) : page_(page) {
        setWantsKeyboardFocus(true);
        keepsClickFocus = true; // arrow keys move, Delete removes
        setTitle("Frames");
        setHelp("Frames", "Key frames of the table. Click to select; Shift for a range, Ctrl for several. Drag to reorder. Arrow keys move, Delete removes.");
        family = Family::Sources;
    }
    void refreshSize() { setSize(juce::jmax(kCell, page_.table().size() * kCell + 4), getParentHeight() > 0 ? getParentHeight() : 70); }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        const auto& table = page_.table();
        const auto clip = g.getClipBounds();
        for (int f = 0; f < table.size(); ++f) {
            const auto cell = juce::Rectangle<int>(2 + f * kCell, 2, kCell - 4, getHeight() - 4);
            if (!cell.intersects(clip))
                continue;
            const bool selected = page_.selection().count(f) > 0;
            const bool current = f == page_.frame();
            // The frame being drawn on: a light edge, its cycle in the bright line and its number in
            // bold; the others amber on black (or on the selection's blue when selected).
            g.setColour(selected ? t.selection : t.display);
            g.fillRoundedRectangle(cell.toFloat(), 3.0f);
            g.setColour(current ? t.control : t.line);
            g.drawRoundedRectangle(cell.toFloat().reduced(current ? 1.0f : 0.5f), 3.0f, current ? 2.0f : 1.0f);
            g.setColour(current ? t.displayLine : t.sources.withAlpha(0.75f));
            g.strokePath(framePath(table.frames[static_cast<std::size_t>(f)], cell.toFloat().reduced(4.0f, 12.0f).withTrimmedBottom(0.0f), 48),
                         juce::PathStrokeType(current ? 1.4f : 1.0f));
            drawText(g, juce::String(f + 1), cell.toFloat().removeFromBottom(13.0f), kTextSmall, current ? t.text : t.textDim, juce::Justification::centred, current);
        }
        if (dropAt_ >= 0) {
            g.setColour(t.focus);
            g.fillRect(dropAt_ * kCell, 2, 2, getHeight() - 4);
        }
        drawFocus(g, getLocalBounds().toFloat());
    }
    int frameAt(int x) const { return juce::jlimit(0, page_.table().size() - 1, x / kCell); }
    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        from_ = frameAt(e.x);
        page_.setFrame(from_, e.mods.isShiftDown(), e.mods.isCommandDown());
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (std::abs(e.getDistanceFromDragStartX()) < 8)
            return;
        dropAt_ = juce::jlimit(0, page_.table().size(), (e.x + kCell / 2) / kCell);
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (dropAt_ >= 0 && from_ >= 0) {
            int to = dropAt_ > from_ ? dropAt_ - 1 : dropAt_;
            if (to != from_) {
                const int from = from_;
                page_.apply("Move frame " + juce::String(from + 1) + " to " + juce::String(to + 1), [from, to](zed::Wavetable& t) { return zed::moveFrame(t, from, to); });
                page_.setFrame(to);
            }
        }
        dropAt_ = -1;
        repaint();
    }
    bool keyPressed(const juce::KeyPress& key) override {
        const bool shift = key.getModifiers().isShiftDown();
        if (key == juce::KeyPress::rightKey) { page_.setFrame(juce::jmin(page_.table().size() - 1, page_.frame() + 1), shift); return true; }
        if (key == juce::KeyPress::leftKey) { page_.setFrame(juce::jmax(0, page_.frame() - 1), shift); return true; }
        if (key == juce::KeyPress::deleteKey) {
            const auto r = page_.range();
            page_.apply("Delete frames", [r](zed::Wavetable& t) { return zed::deleteFrames(t, r); });
            return true;
        }
        return false;
    }

private:
    TablePage& page_;
    int from_ = -1, dropAt_ = -1;
};

// --- Wave view -----------------------------------------------------------------------------------------

class WaveEditor final : public Widget {
public:
    enum class Tool { Free, Flat, RampUp, RampDown, Line, Sine, HalfUp, HalfDown, QuarterIn, QuarterOut, Curve, Nudge, Noise };
    explicit WaveEditor(TablePage& page) : page_(page) {
        setWantsKeyboardFocus(true);
        keepsClickFocus = true;
        setTitle("Wave");
        setHelp("Wave", "Draw on the frame. Pick a tool; with Snap the strokes follow the grid. Faint lines are the neighbouring frames.");
        family = Family::Sources;
    }
    Tool tool = Tool::Free;
    int gridX = 16, gridY = 8;
    bool snap = true;
    double paintMs = 0.0; // the last paint's cost (the workshop's feedback, plan 5.8)

    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced(6.0f); }
    juce::Point<double> toWave(juce::Point<float> p) const {
        const auto a = plot();
        return {juce::jlimit(0.0, 1.0, static_cast<double>((p.x - a.getX()) / a.getWidth())),
                juce::jlimit(-1.0, 1.0, static_cast<double>((a.getCentreY() - p.y) / (a.getHeight() * 0.46f)))};
    }
    double snapX(double x) const { return snap ? std::round(x * gridX) / gridX : x; }
    double snapY(double y) const { return snap ? std::round(y * gridY / 2.0) / (gridY / 2.0) : y; }

    void draw(juce::Graphics& g) override {
        const auto started = juce::Time::getHighResolutionTicks();
        const Theme& t = theme();
        const auto area = plot();
        drawWell(g, getLocalBounds().toFloat());
        g.setColour(t.displayGrid);
        for (int i = 1; i < gridX; ++i)
            g.fillRect(area.getX() + area.getWidth() * static_cast<float>(i) / static_cast<float>(gridX), area.getY(), 1.0f, area.getHeight());
        for (int i = 1; i < gridY; ++i)
            g.fillRect(area.getX(), area.getCentreY() + (static_cast<float>(i) - static_cast<float>(gridY) / 2.0f) * area.getHeight() * 0.92f / static_cast<float>(gridY),
                       area.getWidth(), 1.0f);
        g.setColour(t.lineStrong);
        g.fillRect(area.getX(), area.getCentreY(), area.getWidth(), 1.0f);
        const auto& table = page_.table();
        const int f = page_.frame();
        const int points = juce::jmin(1024, juce::jmax(64, static_cast<int>(area.getWidth())));
        for (const int n : {f - 1, f + 1}) // onion skin
            if (n >= 0 && n < table.size()) {
                g.setColour(t.sources.withAlpha(0.35f));
                g.strokePath(framePath(table.frames[static_cast<std::size_t>(n)], area, points), juce::PathStrokeType(1.0f));
            }
        if (f >= 0 && f < table.size()) // the frame being drawn on, over its amber neighbours
            strokeGlow(g, framePath(table.frames[static_cast<std::size_t>(f)], area, points), t.displayLine, 1.8f);
        if (dragging_ && tool != Tool::Free && tool != Tool::Nudge && tool != Tool::Noise) {
            g.setColour(t.focus.withAlpha(0.5f));
            const auto a = toScreen(start_), b = toScreen(last_);
            g.drawLine(a.x, a.y, b.x, b.y, 1.0f);
        }
        // What a stroke will do and where: the frame, and the shape that draws.
        drawText(g, "Frame " + juce::String(f + 1) + " of " + juce::String(table.size()) + "   " + page_.toolName() + (snap ? ", snap" : ""),
                 getLocalBounds().toFloat().reduced(10.0f, 6.0f).removeFromTop(14.0f), kTextSmall, t.textDim, juce::Justification::centredRight);
        drawFocus(g, getLocalBounds().toFloat());
        paintMs = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - started) * 1000.0;
    }
    juce::Point<float> toScreen(juce::Point<double> w) const {
        const auto a = plot();
        return {a.getX() + a.getWidth() * static_cast<float>(w.x), a.getCentreY() - static_cast<float>(w.y) * a.getHeight() * 0.46f};
    }
    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        dragging_ = true;
        start_ = last_ = toWave(e.position);
        if (tool == Tool::Free)
            stroke(start_, start_);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        const auto now = toWave(e.position);
        if (tool == Tool::Free) {
            stroke(last_, now);
        } else if (tool == Tool::Nudge || tool == Tool::Noise) {
            const double from = std::floor(start_.x * gridX) / gridX, to = from + 1.0 / gridX;
            const double amount = (now.y - last_.y);
            const int frame = page_.frame();
            const bool noise = tool == Tool::Noise;
            page_.apply("", [=](zed::Wavetable& t) {
                if (noise)
                    zed::addNoise(t.frames[static_cast<std::size_t>(frame)], from, to, static_cast<float>(std::abs(amount)), static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks()));
                else
                    zed::nudgeSegment(t.frames[static_cast<std::size_t>(frame)], from, to, static_cast<float>(amount));
                return true;
            }, true);
        }
        last_ = now;
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (!dragging_)
            return;
        dragging_ = false;
        if (tool != Tool::Free && tool != Tool::Nudge && tool != Tool::Noise) {
            static const zed::DrawShape shapes[] = {zed::DrawShape::Flat, zed::DrawShape::Flat, zed::DrawShape::RampUp, zed::DrawShape::RampDown, zed::DrawShape::Line,
                                                    zed::DrawShape::Sine, zed::DrawShape::HalfSineUp, zed::DrawShape::HalfSineDown, zed::DrawShape::QuarterIn,
                                                    zed::DrawShape::QuarterOut, zed::DrawShape::Curve};
            const auto shape = shapes[static_cast<int>(tool)];
            double from = snapX(std::min(start_.x, last_.x)), to = snapX(std::max(start_.x, last_.x));
            if (to - from < 1e-6)
                to = std::min(1.0, from + 1.0 / gridX);
            const bool reversed = last_.x < start_.x;
            const float a = static_cast<float>(snapY(reversed ? last_.y : start_.y)), b = static_cast<float>(snapY(reversed ? start_.y : last_.y));
            const int frame = page_.frame();
            const float level = static_cast<float>(snapY(start_.y)), height = static_cast<float>(snapY(last_.y));
            page_.apply("Draw on frame " + juce::String(frame + 1), [=](zed::Wavetable& t) {
                auto& f = t.frames[static_cast<std::size_t>(frame)];
                if (shape == zed::DrawShape::Flat)
                    zed::drawSegment(f, from, to, shape, height, height);
                else if (shape == zed::DrawShape::Sine || shape == zed::DrawShape::HalfSineUp || shape == zed::DrawShape::HalfSineDown)
                    zed::drawSegment(f, from, to, shape, level, std::abs(height - level));
                else
                    zed::drawSegment(f, from, to, shape, a, b);
                return true;
            });
            return;
        }
        page_.finish(tool == Tool::Free ? "Draw on frame " + juce::String(page_.frame() + 1) : tool == Tool::Nudge ? juce::String("Nudge") : juce::String("Add noise"));
    }
    // One segment of a freehand stroke: a straight line between two pointer positions.
    void stroke(juce::Point<double> a, juce::Point<double> b) {
        const int frame = page_.frame();
        if (a.x > b.x)
            std::swap(a, b);
        const double from = a.x, to = std::max(b.x, a.x + 1.0 / zed::kFrameSize);
        page_.apply("", [=](zed::Wavetable& t) {
            zed::drawSegment(t.frames[static_cast<std::size_t>(frame)], from, to, zed::DrawShape::Line, static_cast<float>(a.y), static_cast<float>(b.y));
            return true;
        }, true);
    }

private:
    TablePage& page_;
    bool dragging_ = false;
    juce::Point<double> start_, last_;
};

// --- Harmonic view -----------------------------------------------------------------------------------

class HarmonicEditor final : public Widget {
public:
    explicit HarmonicEditor(TablePage& page) : page_(page) {
        setWantsKeyboardFocus(true);
        keepsClickFocus = true;
        setTitle("Harmonics");
        setHelp("Harmonics", "Level (top) and phase (bottom) of each harmonic of the frame. Drag across the bars to draw them. Right-click for operations.");
        family = Family::Sources;
    }
    int count = 64;
    int parity = 0; // 0 all, 1 odd only, 2 even only
    void refresh() {
        const auto& table = page_.table();
        if (page_.frame() < table.size())
            h_ = zed::analyzeFrame(table.frames[static_cast<std::size_t>(page_.frame())]);
        repaint();
    }
    // Each half has a strip for its label above its bars, so no bar covers a label.
    struct Layout {
        juce::Rectangle<float> levelLabel, level, phaseLabel, phase;
    };
    Layout layout() const {
        auto inner = getLocalBounds().toFloat().reduced(8.0f);
        Layout l;
        l.level = inner.removeFromTop(inner.getHeight() * 0.62f);
        l.levelLabel = l.level.removeFromTop(16.0f);
        inner.removeFromTop(6.0f);
        l.phaseLabel = inner.removeFromTop(16.0f);
        l.phase = inner;
        return l;
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        const auto area = getLocalBounds().toFloat();
        drawWell(g, area);
        const Layout l = layout();
        const auto top = l.level, inner = l.phase;
        const float w = top.getWidth() / static_cast<float>(count);
        double peak = 1e-9;
        for (int k = 1; k <= count; ++k)
            peak = std::max(peak, h_.magnitude[static_cast<std::size_t>(k)]);
        for (int k = 1; k <= count; ++k) {
            const float m = static_cast<float>(h_.magnitude[static_cast<std::size_t>(k)] / peak);
            const auto bar = juce::Rectangle<float>(top.getX() + w * static_cast<float>(k - 1) + 1.0f, top.getBottom() - top.getHeight() * m, w - 2.0f,
                                                    top.getHeight() * m);
            g.setColour(t.sources.withAlpha(k % 2 == 1 ? 0.95f : 0.7f));
            g.fillRect(bar);
            const float ph = static_cast<float>(h_.phase[static_cast<std::size_t>(k)] / zed::kPi);
            const float y = inner.getCentreY() - ph * inner.getHeight() * 0.5f;
            g.setColour(t.control.withAlpha(m > 1e-4f ? 0.7f : 0.2f)); // phase: steel under the amber levels
            g.fillRect(juce::Rectangle<float>(inner.getX() + w * static_cast<float>(k - 1) + 1.0f, std::min(y, inner.getCentreY()), w - 2.0f, std::abs(y - inner.getCentreY()) + 1.0f));
        }
        g.setColour(t.lineStrong);
        g.fillRect(inner.getX(), inner.getCentreY(), inner.getWidth(), 1.0f);
        drawText(g, "LEVEL", l.levelLabel, kTextSmall, t.textDim, juce::Justification::centredRight, true);
        drawText(g, "PHASE", l.phaseLabel, kTextSmall, t.textDim, juce::Justification::centredRight, true);
        drawText(g, juce::String(count) + " harmonics" + (parity == 1 ? ", odd only" : parity == 2 ? ", even only" : ""),
                 l.levelLabel.withWidth(220.0f), kTextSmall, t.textDim);
        drawFocus(g, area);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        if (e.mods.isPopupMenu()) {
            showMenu();
            return;
        }
        mouseDrag(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        const auto area = getLocalBounds().toFloat().reduced(8.0f);
        const Layout l = layout();
        const auto top = l.level, bottom = l.phase;
        const int k = juce::jlimit(1, count, static_cast<int>((e.position.x - area.getX()) / (area.getWidth() / static_cast<float>(count))) + 1);
        if ((parity == 1 && k % 2 == 0) || (parity == 2 && k % 2 == 1))
            return;
        double peak = 1e-9;
        for (int i = 1; i <= count; ++i)
            peak = std::max(peak, h_.magnitude[static_cast<std::size_t>(i)]);
        if (peak < 1e-6)
            peak = 0.5;
        if (e.position.y < l.phaseLabel.getY() || (!dragPhase_ && e.getMouseDownY() < static_cast<int>(l.phaseLabel.getY()))) {
            dragPhase_ = false;
            const double level = juce::jlimit(0.0, 1.0, static_cast<double>((top.getBottom() - e.position.y) / top.getHeight()));
            h_.magnitude[static_cast<std::size_t>(k)] = level * peak;
        } else {
            dragPhase_ = true;
            h_.phase[static_cast<std::size_t>(k)] = juce::jlimit(-zed::kPi, zed::kPi, static_cast<double>((bottom.getCentreY() - e.position.y) / (bottom.getHeight() * 0.5f)) * zed::kPi);
        }
        const auto harmonics = h_;
        const int frame = page_.frame();
        page_.apply("", [harmonics, frame](zed::Wavetable& t) {
            t.frames[static_cast<std::size_t>(frame)] = zed::synthesizeFrame(harmonics);
            return true;
        }, true);
        repaint();
    }
    void mouseUp(const juce::MouseEvent& e) override {
        if (!e.mods.isPopupMenu())
            page_.finish(dragPhase_ ? "Draw phases" : "Draw harmonics");
        dragPhase_ = false;
    }
    void showMenu() {
        juce::PopupMenu menu;
        const auto op = [this](const juce::String& label, zed::HarmonicOp o, int n, double amount) {
            const auto r = page_.range();
            page_.apply(label, [=](zed::Wavetable& t) {
                const int last = r.last < 0 ? t.size() - 1 : r.last;
                for (int f = r.first; f <= last; ++f)
                    zed::applyHarmonicOp(t.frames[static_cast<std::size_t>(f)], o, n, amount, static_cast<std::uint64_t>(f + 1) * 7919u);
                return true;
            });
        };
        menu.addItem("Clear all", [op] { op("Clear harmonics", zed::HarmonicOp::ClearAll, 0, 1.0); });
        juce::PopupMenu above, below;
        for (const int n : {1, 2, 4, 8, 16, 32, 64, 128}) {
            above.addItem("Keep up to " + juce::String(n), [op, n] { op("Clear above " + juce::String(n), zed::HarmonicOp::ClearAbove, n, 1.0); });
            below.addItem("Keep from " + juce::String(n), [op, n] { op("Clear below " + juce::String(n), zed::HarmonicOp::ClearBelow, n, 1.0); });
        }
        menu.addSubMenu("Clear above", above);
        menu.addSubMenu("Clear below", below);
        menu.addItem("Fill as saw series", [op] { op("Fill saw series", zed::HarmonicOp::FillSaw, 0, 1.0); });
        menu.addItem("Randomize lowest 16, full", [op] { op("Randomize harmonics", zed::HarmonicOp::RandomizeLowest, 16, 1.0); });
        menu.addItem("Randomize lowest 16, half", [op] { op("Randomize harmonics", zed::HarmonicOp::RandomizeLowest, 16, 0.5); });
        menu.addItem("Randomize all", [op] { op("Randomize all harmonics", zed::HarmonicOp::RandomizeAll, 0, 1.0); });
        menu.addItem("Random gaps", [op] { op("Random gaps", zed::HarmonicOp::RandomGaps, 0, 0.3); });
        menu.addItem("Fade the highs", [op] { op("Fade highs", zed::HarmonicOp::FadeHighs, 0, 1.0); });
        menu.addItem("Octave up", [op] { op("Octave up", zed::HarmonicOp::OctaveUp, 0, 1.0); });
        menu.addItem("Octave down", [op] { op("Octave down", zed::HarmonicOp::OctaveDown, 0, 1.0); });
        menu.addItem("Odd harmonics only", [op] { op("Odd only", zed::HarmonicOp::OddOnly, 0, 1.0); });
        menu.addItem("Even harmonics only", [op] { op("Even only", zed::HarmonicOp::EvenOnly, 0, 1.0); });
        menu.addItem("Snap levels to quarters", [op] { op("Snap to quarters", zed::HarmonicOp::SnapQuarters, 0, 1.0); });
        menu.addItem("Scale by harmonic number", [op] { op("Scale by harmonic", zed::HarmonicOp::ScaleByHarmonic, 0, -0.5); });
        menu.addSeparator();
        juce::PopupMenu show;
        for (const int n : {16, 32, 64, 128, 256})
            show.addItem(juce::String(n) + " harmonics", true, count == n, [this, n] { count = n; repaint(); });
        menu.addSubMenu("Show", show);
        menu.addItem("Draw odd harmonics only", true, parity == 1, [this] { parity = parity == 1 ? 0 : 1; repaint(); });
        menu.addItem("Draw even harmonics only", true, parity == 2, [this] { parity = parity == 2 ? 0 : 2; repaint(); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
    }

private:
    TablePage& page_;
    zed::Harmonics h_;
    bool dragPhase_ = false;
};

// --- Spectral view -------------------------------------------------------------------------------------

class SpectralEditor final : public Widget {
public:
    explicit SpectralEditor(TablePage& page) : page_(page) {
        setWantsKeyboardFocus(true);
        keepsClickFocus = true;
        setTitle("Spectrum of the table");
        setHelp("Spectral view", "Frames (across) by harmonics (up), brightness is level. Paint, erase or smooth with the brush; Lock keeps the brush on one harmonic.");
        family = Family::Sources;
    }
    enum class Brush { Paint, Erase, Smooth };
    Brush brush = Brush::Paint;
    bool lockHarmonic = false;
    int harmonics = 64;
    void refresh() {
        const auto& table = page_.table();
        spectra_.resize(static_cast<std::size_t>(table.size()));
        for (int f = 0; f < table.size(); ++f)
            spectra_[static_cast<std::size_t>(f)] = zed::analyzeFrame(table.frames[static_cast<std::size_t>(f)]);
        peak_ = 1e-9;
        for (const auto& s : spectra_)
            for (int k = 1; k <= harmonics; ++k)
                peak_ = std::max(peak_, s.magnitude[static_cast<std::size_t>(k)]);
        image_ = {};
        repaint();
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        const auto area = getLocalBounds().toFloat();
        drawWell(g, area);
        const auto plot = area.reduced(8.0f);
        const int frames = static_cast<int>(spectra_.size());
        if (frames == 0)
            return;
        if (!image_.isValid()) {
            image_ = juce::Image(juce::Image::ARGB, frames, harmonics, true, juce::SoftwareImageType());
            for (int f = 0; f < frames; ++f)
                for (int k = 1; k <= harmonics; ++k) {
                    const double db = 20.0 * std::log10(spectra_[static_cast<std::size_t>(f)].magnitude[static_cast<std::size_t>(k)] / peak_ + 1e-9);
                    const float level = juce::jlimit(0.0f, 1.0f, static_cast<float>((db + 60.0) / 60.0));
                    image_.setPixelAt(f, harmonics - k, t.display.interpolatedWith(t.sources, level).interpolatedWith(t.displayLine, level * level * 0.6f));
                }
        }
        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        g.drawImage(image_, plot);
        const float x = plot.getX() + plot.getWidth() * (static_cast<float>(page_.frame()) + 0.5f) / static_cast<float>(frames);
        g.setColour(t.focus.withAlpha(0.7f));
        g.fillRect(x, plot.getY(), 1.0f, plot.getHeight());
        drawText(g, juce::String(frames) + " frames x " + juce::String(harmonics) + " harmonics", area.reduced(10.0f, 6.0f).removeFromTop(14.0f), kTextSmall, t.textDim,
                 juce::Justification::centredRight);
        drawFocus(g, area);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        grabKeyboardFocus();
        lockedRow_ = cellAt(e.position).second;
        mouseDrag(e);
    }
    std::pair<int, int> cellAt(juce::Point<float> p) const {
        const auto plot = getLocalBounds().toFloat().reduced(8.0f);
        const int frames = juce::jmax(1, static_cast<int>(spectra_.size()));
        const int f = juce::jlimit(0, frames - 1, static_cast<int>((p.x - plot.getX()) / plot.getWidth() * static_cast<float>(frames)));
        const int k = juce::jlimit(1, harmonics, harmonics - static_cast<int>((p.y - plot.getY()) / plot.getHeight() * static_cast<float>(harmonics)));
        return {f, k};
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        auto [f, k] = cellAt(e.position);
        if (lockHarmonic)
            k = lockedRow_;
        auto& s = spectra_[static_cast<std::size_t>(f)];
        for (int dk = -1; dk <= 1; ++dk) { // a brush three harmonics tall (one when locked)
            const int h = k + dk;
            if (h < 1 || h > harmonics || (lockHarmonic && dk != 0))
                continue;
            auto& m = s.magnitude[static_cast<std::size_t>(h)];
            if (brush == Brush::Paint)
                m = std::max(m, peak_ * (dk == 0 ? 0.6 : 0.3));
            else if (brush == Brush::Erase)
                m = 0.0;
            else {
                const double left = f > 0 ? spectra_[static_cast<std::size_t>(f - 1)].magnitude[static_cast<std::size_t>(h)] : m;
                const double right = f + 1 < static_cast<int>(spectra_.size()) ? spectra_[static_cast<std::size_t>(f + 1)].magnitude[static_cast<std::size_t>(h)] : m;
                m = (left + 2.0 * m + right) / 4.0;
            }
        }
        const auto harmonicsOfFrame = s;
        page_.apply("", [f, harmonicsOfFrame](zed::Wavetable& t) {
            t.frames[static_cast<std::size_t>(f)] = zed::synthesizeFrame(harmonicsOfFrame);
            return true;
        }, true);
        image_ = {};
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override {
        page_.finish(brush == Brush::Paint ? "Paint spectrum" : brush == Brush::Erase ? "Erase spectrum" : "Smooth spectrum");
    }

private:
    TablePage& page_;
    std::vector<zed::Harmonics> spectra_;
    double peak_ = 1.0;
    juce::Image image_;
    int lockedRow_ = 1;
};

// --- Import wizard ---------------------------------------------------------------------------------------

class ImportWizard final : public juce::Component {
public:
    explicit ImportWizard(Context& ctx, TablePage& page) : ctx_(ctx), page_(page) {
        setTitle("Import wizard");
        markOverlay(*this);
        for (Button* b : {&choose_, &methodTable_, &methodCycles_, &methodPitch_, &methodFixed_, &methodSpectral_, &methodImage_, &left_, &right_, &mid_,
                          &split_, &snap_, &fade_, &normalize_, &trim_, &audition_, &commit_, &cancel_}) {
            b->textSize = kTextSmall + 0.5f;
            addAndMakeVisible(*b);
        }
        for (Button* b : {&methodTable_, &methodCycles_, &methodPitch_, &methodFixed_, &methodSpectral_, &methodImage_, &left_, &right_, &mid_, &snap_,
                          &fade_, &normalize_, &trim_})
            b->toggleRole = true;
        commit_.accentStyle = true;
        commit_.family = Family::Sources;
        addAndMakeVisible(status_);
        addAndMakeVisible(suggestion_);
        addAndMakeVisible(preview_);
        cycleField_.setText("", false);
        addAndMakeVisible(cycleField_);
        choose_.onClick = [this] { chooseFile(); };
        const auto method = [this](Button& b, const juce::String& m) { b.onClick = [this, m] { method_ = m; rebuild(); }; };
        method(methodTable_, "table");
        method(methodCycles_, "single_cycle");
        method(methodPitch_, "pitch_tracked");
        method(methodFixed_, "fixed_cycles");
        method(methodSpectral_, "spectral");
        method(methodImage_, "image");
        left_.onClick = [this] { stereo_ = zed::StereoChoice::Left; rebuild(); };
        right_.onClick = [this] { stereo_ = zed::StereoChoice::Right; rebuild(); };
        mid_.onClick = [this] { stereo_ = zed::StereoChoice::Mid; rebuild(); };
        split_.setHelp("Left to Osc 1, right to Osc 2", "One gesture for stereo files: the left channel becomes Osc 1's table and the right Osc 2's.");
        split_.onClick = [this] { commitSplit(); };
        for (Button* b : {&snap_, &fade_, &normalize_, &trim_})
            b->onClick = [this, b] { b->setToggled(!b->toggled()); rebuild(); };
        cycleField_.onReturnKey = [this] { rebuild(); };
        audition_.setHelp("Audition", "Plays a note through the oscillator with the result; Commit keeps it, Cancel goes back.");
        audition_.onClick = [this] { auditionResult(); };
        commit_.onClick = [this] { commit(); };
        cancel_.onClick = [this] { cancel(); };
        normalize_.setToggled(true);
    }
    // Opens a file directly (tests and drag and drop); returns false when it cannot be read.
    bool open(const juce::File& file) {
        file_ = file;
        bytes_.clear();
        juce::MemoryBlock data;
        if (!file.loadFileAsData(data)) {
            status_.setText("Could not read " + file.getFileName());
            return false;
        }
        bytes_.assign(static_cast<const std::uint8_t*>(data.getData()), static_cast<const std::uint8_t*>(data.getData()) + data.getSize());
        image_ = !file.hasFileExtension("wav");
        std::string error;
        wav_ = {};
        if (!image_ && !zed::decodeWav(bytes_, wav_, error)) {
            status_.setText(str(error));
            return false;
        }
        if (image_) {
            method_ = "image";
            suggestion_.setText("An image: each row becomes a frame.");
        } else {
            const auto s = zed::suggestImport(wav_);
            method_ = str(s.method);
            suggestion_.setText("Suggested: " + methodName(method_) + ". " + str(s.reason));
        }
        rebuild();
        return result_ != nullptr;
    }
    std::shared_ptr<zed::Wavetable> result() const { return result_; }
    const std::vector<double>& markers() const { return markers_; }
    juce::String method() const { return method_; }
    void begin() {
        before_ = std::make_shared<zed::Wavetable>(page_.table());
        result_.reset();
        markers_.clear();
        status_.setText("Choose a WAV file (single cycles, a table, or a pitched recording) or an image.");
        suggestion_.setText({});
        repaint();
    }
    void commit() {
        if (result_ == nullptr)
            return;
        page_.replaceTable(std::make_shared<zed::Wavetable>(*result_), "Import " + file_.getFileName());
        setVisible(false);
    }
    void cancel() {
        if (auditioned_ && before_ != nullptr)
            ctx_.processor.setTableLive(page_.osc(), before_);
        auditioned_ = false;
        setVisible(false);
    }
    void resized() override {
        auto area = getLocalBounds().reduced(16);
        area.removeFromTop(28);
        auto row = area.removeFromTop(28);
        choose_.setBounds(row.removeFromLeft(130));
        row.removeFromLeft(10);
        status_.setBounds(row);
        area.removeFromTop(4);
        suggestion_.setBounds(area.removeFromTop(18));
        area.removeFromTop(8);
        row = area.removeFromTop(26);
        placeRow(row, {{&methodTable_, 70}, {&methodCycles_, 104}, {&methodPitch_, 110}, {&methodFixed_, 110}, {&methodSpectral_, 82}, {&methodImage_, 70}}, 4);
        area.removeFromTop(6);
        row = area.removeFromTop(26);
        placeRow(row, {{&left_, 56}, {&right_, 60}, {&mid_, 50}, {&split_, 200}, {&snap_, 150}}, 4);
        row.removeFromLeft(470);
        cycleField_.setBounds(row.removeFromLeft(150));
        area.removeFromTop(8);
        auto bottom = area.removeFromBottom(30);
        commit_.setBounds(bottom.removeFromRight(100));
        bottom.removeFromRight(6);
        cancel_.setBounds(bottom.removeFromRight(90));
        bottom.removeFromRight(6);
        audition_.setBounds(bottom.removeFromRight(100));
        placeRow(bottom, {{&fade_, 100}, {&normalize_, 100}, {&trim_, 140}}, 4);
        area.removeFromBottom(8);
        preview_.setBounds(area);
    }
    void paint(juce::Graphics& g) override {
        const Theme& t = theme();
        g.setColour(t.raised);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), t.radius);
        g.setColour(t.lineStrong);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), t.radius, 1.0f);
        drawText(g, "IMPORT INTO OSC " + juce::String(page_.osc() + 1), getLocalBounds().reduced(16, 12).removeFromTop(22).toFloat(), kTextTitle, t.text,
                 juce::Justification::centredLeft, true);
    }

private:
    // The source with its cycle markers (left) and the resulting table (right).
    class Preview final : public Widget {
    public:
        explicit Preview(ImportWizard& w) : w_(w) {
            setTitle("Import preview");
            setInterceptsMouseClicks(false, false);
        }
        bool hasVisibleLabel() const override { return false; }
        void draw(juce::Graphics& g) override {
            const Theme& t = theme();
            auto area = getLocalBounds().toFloat();
            auto source = area.removeFromLeft(area.getWidth() * 0.5f - 4.0f);
            area.removeFromLeft(8.0f);
            drawWell(g, source);
            drawWell(g, area);
            const auto& wav = w_.wav_;
            if (!wav.channels.empty() && !w_.image_) {
                const auto& ch = wav.channels.front();
                const auto plot = source.reduced(6.0f, 16.0f);
                juce::Path p;
                const int n = static_cast<int>(ch.size());
                const int points = juce::jmax(2, static_cast<int>(plot.getWidth()));
                for (int i = 0; i < points; ++i) {
                    const int a = static_cast<int>(static_cast<std::int64_t>(i) * n / points), b = static_cast<int>(static_cast<std::int64_t>(i + 1) * n / points);
                    float lo = 0.0f, hi = 0.0f;
                    for (int k = a; k < b && k < n; ++k) {
                        lo = std::min(lo, ch[static_cast<std::size_t>(k)]);
                        hi = std::max(hi, ch[static_cast<std::size_t>(k)]);
                    }
                    const float x = plot.getX() + static_cast<float>(i);
                    p.addLineSegment(juce::Line<float>(x, plot.getCentreY() - hi * plot.getHeight() * 0.5f, x, plot.getCentreY() - lo * plot.getHeight() * 0.5f), 1.0f);
                }
                g.setColour(t.sources.withAlpha(0.8f));
                g.fillPath(p);
                g.setColour(t.focus.withAlpha(0.7f));
                for (const double m : w_.markers_) // the cycles found
                    g.fillRect(plot.getX() + plot.getWidth() * static_cast<float>(m / std::max(1, n)), plot.getY(), 1.0f, plot.getHeight());
                drawText(g, "Source, with " + juce::String(static_cast<int>(w_.markers_.size())) + " cycle markers", source.reduced(8.0f, 2.0f).removeFromTop(14.0f),
                         kTextSmall, t.textDim);
            } else {
                drawText(g, w_.image_ ? "Image" : "No file yet", source, kTextBody, t.textDim, juce::Justification::centred);
            }
            if (w_.result_ != nullptr && !w_.result_->frames.empty()) {
                const auto& table = *w_.result_;
                const auto plot = area.reduced(10.0f, 18.0f);
                const int shown = std::min(table.size(), 24);
                for (int l = shown - 1; l >= 0; --l) {
                    const int f = shown > 1 ? l * (table.size() - 1) / (shown - 1) : 0;
                    const float depth = shown > 1 ? static_cast<float>(l) / static_cast<float>(shown - 1) : 0.0f;
                    const auto box = plot.withWidth(plot.getWidth() * 0.75f).withHeight(plot.getHeight() * 0.5f)
                                         .translated(depth * plot.getWidth() * 0.25f, plot.getHeight() * 0.5f * (1.0f - depth));
                    g.setColour(t.sources.withAlpha(0.15f + 0.6f * (1.0f - depth)));
                    g.strokePath(framePath(table.frames[static_cast<std::size_t>(f)], box, 96), juce::PathStrokeType(1.0f));
                }
                drawText(g, "Result: " + juce::String(table.size()) + " frames", area.reduced(8.0f, 2.0f).removeFromTop(14.0f), kTextSmall, t.textDim);
            }
        }

    private:
        ImportWizard& w_;
    };

    static juce::String methodName(const juce::String& m) {
        if (m == "table") return "a table file";
        if (m == "single_cycle") return "single cycle";
        if (m == "pitch_tracked") return "pitch-tracked cycles";
        if (m == "fixed_cycles") return "fixed-length cycles";
        if (m == "spectral") return "spectral snapshots";
        return "image";
    }

    void chooseFile() {
        auto chooser = std::make_shared<juce::FileChooser>("Import into a wavetable", ctx_.prefs.userContentFolder(), "*.wav;*.png;*.bmp;*.pgm;*.ppm");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, chooser](const juce::FileChooser& fc) {
            if (fc.getResult().existsAsFile())
                open(fc.getResult());
        });
    }

    void rebuild() {
        for (Button* b : {&methodTable_, &methodCycles_, &methodPitch_, &methodFixed_, &methodSpectral_, &methodImage_})
            b->setToggled(false);
        Button* chosen = method_ == "table" ? &methodTable_ : method_ == "single_cycle" ? &methodCycles_ : method_ == "pitch_tracked" ? &methodPitch_
                         : method_ == "fixed_cycles" ? &methodFixed_ : method_ == "spectral" ? &methodSpectral_ : &methodImage_;
        chosen->setToggled(true);
        left_.setToggled(stereo_ == zed::StereoChoice::Left);
        right_.setToggled(stereo_ == zed::StereoChoice::Right);
        mid_.setToggled(stereo_ == zed::StereoChoice::Mid);
        const bool stereo = wav_.channelCount() > 1;
        for (Button* b : {&left_, &right_, &mid_, &split_})
            b->setEnabled(stereo);
        result_ = build(stereo_);
        repaint();
        preview_.repaint();
    }

    std::shared_ptr<zed::Wavetable> build(zed::StereoChoice channel) {
        markers_.clear();
        if (bytes_.empty())
            return nullptr;
        auto table = std::make_shared<zed::Wavetable>();
        std::string error;
        bool ok = false;
        if (method_ == "image") {
            zed::ImageData image;
            ok = zed::decodeImage(bytes_, image, error) && zed::importImage(image, zed::ImageMode::Harmonics, zed::kMaxFrames, *table, error);
        } else if (method_ == "table") {
            zed::TableReadOptions options;
            options.channel = channel == zed::StereoChoice::Right ? 1 : channel == zed::StereoChoice::Left ? 0 : -1;
            ok = zed::decodeWavetable(bytes_, *table, error, options);
        } else {
            const zed::AudioClip clip = zed::clipFromWav(wav_, channel);
            zed::SliceOptions slice;
            slice.snapToZeroCrossings = snap_.toggled();
            const juce::String typed = cycleField_.getText().trim();
            double note = 0.0;
            if (typed.isNotEmpty()) {
                if (zed::parseNoteName(typed.toStdString(), note))
                    slice.cycleSamples = zed::cycleSamplesForNote(note, clip.sampleRate);
                else
                    slice.cycleSamples = typed.getDoubleValue();
            }
            zed::ImportReport report;
            if (method_ == "single_cycle") {
                const zed::AudioClip one[] = {clip};
                ok = zed::importSingleCycles(one, *table, error);
            } else if (method_ == "pitch_tracked") {
                ok = zed::importPitchTracked(clip, slice, *table, error, &report);
            } else if (method_ == "fixed_cycles") {
                ok = zed::importFixedCycles(clip, slice, *table, error, &report);
            } else {
                zed::SnapshotOptions snap;
                ok = zed::importSpectralSnapshots(clip, snap, *table, error);
            }
            markers_ = report.markers;
            if (report.detectedHz > 0.0)
                status_.setText(file_.getFileName() + ": " + juce::String(report.detectedHz, 1) + " Hz, " + juce::String(report.cyclesFound) + " cycles");
        }
        if (!ok) {
            status_.setText(str(error));
            return nullptr;
        }
        if (fade_.toggled())
            zed::processFrames(*table, zed::allFrames(), zed::ProcessOp::CrossfadeEdges, 32);
        if (trim_.toggled())
            zed::trimSilentFrames(*table, -60.0);
        if (normalize_.toggled())
            zed::processFrames(*table, zed::allFrames(), zed::ProcessOp::NormalizeShared);
        table->name = file_.getFileNameWithoutExtension().toStdString();
        if (method_ == "pitch_tracked" || method_ == "fixed_cycles" || method_ == "spectral")
            table->morph = zed::MorphMode::Crossfade;
        if (markers_.empty() && method_ != "pitch_tracked" && method_ != "fixed_cycles")
            status_.setText(file_.getFileName() + ": " + juce::String(table->size()) + " frames");
        return table;
    }

    void auditionResult() {
        if (result_ == nullptr)
            return;
        ctx_.processor.setTableLive(page_.osc(), result_);
        auditioned_ = true;
        ctx_.processor.audition({{48, 0.0, 0.9, 0.8}, {55, 0.0, 0.9, 0.7}, {60, 1.0, 0.9, 0.8}});
    }

    void commitSplit() {
        const auto left = build(zed::StereoChoice::Left);
        const auto right = build(zed::StereoChoice::Right);
        if (left == nullptr || right == nullptr)
            return;
        ctx_.session.edit("Import " + file_.getFileName() + " left to Osc 1, right to Osc 2", [&](zed::Patch& p) {
            p.tableData[0].table = left;
            p.tableData[1].table = right;
            p.tables[0] = p.tables[1] = file_.getFullPathName().toStdString();
            return true;
        });
        setVisible(false);
    }

    Context& ctx_;
    TablePage& page_;
    juce::File file_;
    std::vector<std::uint8_t> bytes_;
    zed::WavData wav_;
    bool image_ = false;
    juce::String method_ = "table";
    zed::StereoChoice stereo_ = zed::StereoChoice::Mid;
    std::shared_ptr<zed::Wavetable> result_, before_;
    std::vector<double> markers_;
    bool auditioned_ = false;
    Button choose_{"Choose a file", Glyph::Folder}, methodTable_{"Table"}, methodCycles_{"Single cycle"}, methodPitch_{"Pitch-tracked"},
        methodFixed_{"Fixed length"}, methodSpectral_{"Spectral"}, methodImage_{"Image"}, left_{"Left"}, right_{"Right"}, mid_{"Mid"},
        split_{"Left to Osc 1, right to Osc 2"}, snap_{"Snap to zero crossings"}, fade_{"Fade edges"}, normalize_{"Normalize"}, trim_{"Trim silent frames"},
        audition_{"Audition", Glyph::Speaker}, commit_{"Commit"}, cancel_{"Cancel"};
    Caption status_{"", kTextSmall + 0.5f, false}, suggestion_{"", kTextSmall, true};
    TextField cycleField_{"Cycle: samples or a note (A2)", "Cycle length"};
    Preview preview_{*this};
};

// --- The page ----------------------------------------------------------------------------------------

TablePage::TablePage(Context& ctx) : Page(ctx) {
    setTitle("TABLE");
    table_ = std::make_shared<zed::Wavetable>(zed::TableData::builtinWavetable());
    const auto button = [&](std::vector<juce::Component*>& group, const juce::String& text, Glyph glyph, const juce::String& help, std::function<void()> click) -> Button& {
        auto& b = parts_.add<Button>(*this, text, glyph);
        b.textSize = kTextSmall + 0.5f;
        b.setHelp(text, help);
        b.onClick = std::move(click);
        group.push_back(&b);
        return b;
    };
    for (int o = 0; o < 3; ++o) {
        oscButtons_[o] = &button(toolbar_, "Osc " + juce::String(o + 1), Glyph::None, "Edit this oscillator's table.", [this, o] { setOsc(o); });
        oscButtons_[o]->toggleRole = true;
    }
    name_ = &parts_.add<Caption>(*this, "", kTextBody, false);
    toolbar_.push_back(name_);
    button(toolbar_, "Import", Glyph::Folder, "Make a table from audio or an image, with a preview before it is kept.", [this] { showImport(); });
    button(toolbar_, "Export", Glyph::Save, "Write the table, the selection, or each frame as a single-cycle file.", [this] { showExportMenu(); });
    button(toolbar_, "Save table", Glyph::Save, "Save the table in your Wavetables folder; the oscillator then plays it by name.", [this] { saveTableAs(); });
    button(toolbar_, "Morph", Glyph::None, "How the positions between key frames are made, and baking them into frames.", [this] { showMorphMenu(); });
    button(toolbar_, "Process", Glyph::None, "Normalize, fades, filters, splits and more, on the selected frames or all.", [this] { showProcessMenu(); });
    button(toolbar_, "Sort", Glyph::None, "Reorder the frames by brightness, partials or fundamental.", [this] { showSortMenu(); });
    button(toolbar_, "Resynth", Glyph::Wand, "Bake a warp or the whole sound into a table; turn an LFO shape into a frame.", [this] { showResynthMenu(); });

    button(frameButtons_, "Insert", Glyph::Plus, "Insert a copy of the current frame after it.", [this] {
        const int at = frame_ + 1;
        const zed::Frame copy = table_->frames[static_cast<std::size_t>(frame_)];
        apply("Insert frame", [at, copy](zed::Wavetable& t) { return zed::insertFrame(t, at, copy); });
        setFrame(at);
    });
    button(frameButtons_, "Duplicate", Glyph::Copy, "Duplicate the selected frames.", [this] { const auto r = range(); apply("Duplicate frames", [r](zed::Wavetable& t) { return zed::duplicateFrames(t, r); }); });
    button(frameButtons_, "Delete", Glyph::Trash, "Delete the selected frames.", [this] {
        const auto r = range();
        apply("Delete frames", [r](zed::Wavetable& t) { return zed::deleteFrames(t, r); });
        setFrame(juce::jmin(frame_, table_->size() - 1));
    });
    button(frameButtons_, "Thin", Glyph::None, "Keep every second (or Nth) frame.", [this] {
        juce::PopupMenu menu;
        for (const int n : {2, 3, 4, 8})
            menu.addItem("Keep every " + juce::String(n), [this, n] { apply("Thin frames by " + juce::String(n), [n](zed::Wavetable& t) { return zed::thinFrames(t, n); }); setFrame(0); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(frameButtons_.back()));
    });
    button(frameButtons_, "Crop", Glyph::None, "Keep only the selected frames.", [this] { const auto r = range(); apply("Crop to selection", [r](zed::Wavetable& t) { return zed::cropFrames(t, r); }); setFrame(0); });
    button(frameButtons_, "Reverse", Glyph::None, "Reverse the order of the frames.", [this] { apply("Reverse frames", [](zed::Wavetable& t) { zed::reverseFrames(t); return true; }); });

    strip_ = std::make_unique<FrameStrip>(*this);
    stripView_.setViewedComponent(strip_.get(), false);
    stripView_.setScrollBarsShown(false, true);
    stripView_.setTitle("Frames strip");
    addAndMakeVisible(stripView_);
    wave_ = std::make_unique<WaveEditor>(*this);
    addAndMakeVisible(*wave_);
    harmonics_ = std::make_unique<HarmonicEditor>(*this);
    spectral_ = std::make_unique<SpectralEditor>(*this);
    addChildComponent(*harmonics_);
    addChildComponent(*spectral_);

    // The drawing shapes beside the wave they draw on: the common ones as buttons that show the
    // silhouette each draws (named for screen readers and the help strip), the rest in one
    // labelled selector; then Snap, a latch, and the grid, which says its size.
    frameCaption_ = &parts_.add<Caption>(*this, "FRAMES", kTextSmall, true);
    frameCaption_->bold = true;
    shapeCaption_ = &parts_.add<Caption>(*this, "DRAW", kTextSmall, true);
    shapeCaption_->bold = true;
    static const std::tuple<const char*, Glyph, WaveEditor::Tool> common[] = {
        {"Free", Glyph::Pencil, WaveEditor::Tool::Free},         {"Line", Glyph::ShapeLine, WaveEditor::Tool::Line},
        {"Flat", Glyph::ShapeFlat, WaveEditor::Tool::Flat},      {"Ramp up", Glyph::ShapeRampUp, WaveEditor::Tool::RampUp},
        {"Ramp down", Glyph::ShapeRampDown, WaveEditor::Tool::RampDown}, {"Sine", Glyph::ShapeSine, WaveEditor::Tool::Sine},
        {"Curve", Glyph::ShapeCurve, WaveEditor::Tool::Curve}};
    for (const auto& [name, glyph, tool] : common) {
        auto& b = button(waveTools_, "", glyph, "Drawing shape: " + juce::String(name) + ". Drag across the wave.", {});
        b.setTitle(juce::String("Draw: ") + name);
        b.setTooltip(name);
        b.toggleRole = true;
        const auto t = tool;
        b.onClick = [this, t] {
            wave_->tool = t;
            updateTools();
        };
        shapeButtons_.push_back({&b, static_cast<int>(tool)});
    }
    auto& more = button(waveTools_, "More shapes", Glyph::ChevronDown, "Arches, quarter curves, Nudge and Noise.", [this] { showMoreShapes(); });
    more.toggleRole = true;
    more.textSize = kTextSmall;
    moreShapes_ = &more;
    auto& snap = button(waveTools_, "Snap", Glyph::None, "Strokes follow the grid. The lamp is lit while it is latched.", {});
    snap.toggleRole = true;
    snap.latch = true;
    snap.textSize = kTextSmall;
    snap.setToggled(true);
    snap.onClick = [this] { wave_->snap = !wave_->snap; updateTools(); };
    snap_ = &snap;
    auto& grid = button(waveTools_, "Grid 16", Glyph::ChevronDown, "Grid lines across the cycle: choose how many.", {});
    grid.textSize = kTextSmall;
    grid.onClick = [this] {
        juce::PopupMenu menu;
        for (const int n : {4, 8, 12, 16, 24, 32, 64})
            menu.addItem(juce::String(n) + " across", true, wave_->gridX == n, [this, n] { wave_->gridX = n; updateTools(); wave_->repaint(); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(grid_));
    };
    grid_ = &grid;

    auto& tabs = parts_.add<TabStrip>(*this, juce::StringArray{"HARMONICS", "SPECTRAL", "FORMULA"});
    tabs.family = Family::Sources;
    tabs.setTitle("Workshop views");
    tabs.onChange = [this](int) { layoutRightTab(); };
    rightTabs_ = &tabs;
    for (const auto& [name, brush] : {std::pair{"Paint", SpectralEditor::Brush::Paint}, std::pair{"Erase", SpectralEditor::Brush::Erase}, std::pair{"Smooth", SpectralEditor::Brush::Smooth}}) {
        auto& b = button(formulaParts_, name, juce::String(name) == "Erase" ? Glyph::Erase : Glyph::Pencil, "Spectral brush: " + juce::String(name), {});
        b.toggleRole = true;
        const auto br = brush;
        b.onClick = [this, br] { spectral_->brush = br; layoutRightTab(); };
        b.getProperties().set("spectral", true);
    }
    auto& lock = button(formulaParts_, "Lock harmonic", Glyph::Lock, "The brush stays on the harmonic where the stroke began.", {});
    lock.toggleRole = true;
    lock.getProperties().set("spectral", true);
    lock.onClick = [this, &lock] { spectral_->lockHarmonic = !spectral_->lockHarmonic; lock.setToggled(spectral_->lockHarmonic); };
    addAndMakeVisible(formula_);
    addAndMakeVisible(phaseFormula_);
    addAndMakeVisible(formulaError_);
    formula_.onReturnKey = [this] { applyFormula(); };
    auto& domain = button(formulaParts_, "Wave", Glyph::None, "Write samples (t, x) or harmonic levels (h).", {});
    domain.onClick = [this, &domain] { formulaDomain_ = 1 - formulaDomain_; domain.setText(formulaDomain_ == 0 ? "Wave" : "Harmonics"); };
    auto& scope = button(formulaParts_, "All frames", Glyph::None, "Which frames the formula writes.", {});
    scope.onClick = [this, &scope] {
        formulaScope_ = (formulaScope_ + 1) % 3;
        scope.setText(formulaScope_ == 0 ? "This frame" : formulaScope_ == 1 ? "Selection" : "All frames");
    };
    auto& applyButton = button(formulaParts_, "Apply", Glyph::None, "Write the frames with the formula (Enter).", [this] { applyFormula(); });
    applyButton.accentStyle = true;
    applyButton.family = Family::Sources;
    button(formulaParts_, "Formulas", Glyph::None, "Factory formulas and yours.", [this] {
        juce::PopupMenu menu;
        for (const auto& f : kFormulas)
            menu.addItem(f.name, [this, &f] {
                formula_.setText(f.expression, false);
                formulaDomain_ = f.domain;
                for (auto* c : formulaParts_)
                    if (auto* b = dynamic_cast<Button*>(c); b != nullptr && (b->text() == "Wave" || b->text() == "Harmonics"))
                        b->setText(f.domain == 0 ? "Wave" : "Harmonics");
            });
        const juce::File userList = ctx_.prefs.userContentFolder().getChildFile("Formulas.txt");
        juce::StringArray lines;
        userList.readLines(lines);
        if (lines.size() > 0) {
            menu.addSeparator();
            for (const auto& line : lines)
                if (line.trim().isNotEmpty())
                    menu.addItem(line.trim(), [this, line] { formula_.setText(line.trim(), false); });
        }
        menu.addSeparator();
        menu.addItem("Keep this formula in my list", formula_.getText().trim().isNotEmpty(), false, [this, userList] {
            userList.getParentDirectory().createDirectory();
            userList.appendText(formula_.getText().trim() + "\n");
        });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
    });
    wizard_ = std::make_unique<ImportWizard>(ctx, *this);
    addChildComponent(*wizard_);
    setOsc(0);
    layoutRightTab();
}

TablePage::~TablePage() = default;

void TablePage::setOsc(int osc) {
    osc_ = juce::jlimit(0, 2, osc);
    for (int o = 0; o < 3; ++o)
        oscButtons_[o]->setToggled(o == osc_);
    reload();
}

void TablePage::reload() {
    loaded_ = ctx_.processor.editableTable(osc_);
    table_ = std::make_shared<zed::Wavetable>(loaded_ != nullptr ? *loaded_ : zed::TableData::builtinWavetable());
    frame_ = juce::jlimit(0, table_->size() - 1, frame_);
    std::erase_if(selection_, [&](int f) { return f >= table_->size(); });
    if (selection_.empty())
        selection_.insert(frame_);
    juce::String ref;
    bool edited = false;
    ctx_.processor.readDocument([&](const zed::Patch& p) {
        ref = str(p.tables[static_cast<std::size_t>(osc_)]);
        edited = static_cast<bool>(p.tableData[static_cast<std::size_t>(osc_)]);
    });
    const juce::String shown = table_->name.empty() ? (ref.isEmpty() ? juce::String("Basic Shapes") : ref.fromLastOccurrenceOf("/", false, false)) : str(table_->name);
    name_->setText(shown + "  " + juce::String(table_->size()) + (table_->size() == 1 ? " frame" : " frames") + (edited ? "  (edited, kept in the patch)" : ""));
    if (strip_ != nullptr) {
        strip_->refreshSize();
        strip_->repaint();
    }
    if (harmonics_ != nullptr)
        harmonics_->refresh();
    if (spectral_ != nullptr && spectral_->isVisible())
        spectral_->refresh();
    if (wave_ != nullptr)
        wave_->repaint();
}

void TablePage::structureChanged() {
    // Undo, redo, presets and the workshop's own edits all change the document; reload only when
    // the processor's table is no longer the one shown.
    if (ctx_.processor.editableTable(osc_) != loaded_)
        reload();
}

zed::FrameSelection TablePage::range() const {
    if (selection_.empty())
        return zed::oneFrame(frame_);
    return {*selection_.begin(), *selection_.rbegin()};
}

void TablePage::setFrame(int frame, bool extend, bool toggle) {
    frame = juce::jlimit(0, table_->size() - 1, frame);
    if (extend) {
        const int a = std::min(frame, frame_), b = std::max(frame, frame_);
        for (int f = a; f <= b; ++f)
            selection_.insert(f);
    } else if (toggle) {
        if (selection_.count(frame) && selection_.size() > 1)
            selection_.erase(frame);
        else
            selection_.insert(frame);
    } else {
        selection_ = {frame};
    }
    frame_ = frame;
    strip_->repaint();
    wave_->repaint();
    harmonics_->refresh();
    spectral_->repaint();
    const int x = frame * FrameStrip::kCell;
    if (x < stripView_.getViewPositionX() || x + FrameStrip::kCell > stripView_.getViewPositionX() + stripView_.getViewWidth())
        stripView_.setViewPosition(juce::jmax(0, x - stripView_.getViewWidth() / 2), 0);
}

void TablePage::apply(const juce::String& label, const std::function<bool(zed::Wavetable&)>& change, bool live) {
    auto next = std::make_shared<zed::Wavetable>(*table_);
    if (!change(*next))
        return;
    if (next->frames.empty())
        return;
    table_ = next;
    loaded_ = table_;
    frame_ = juce::jlimit(0, table_->size() - 1, frame_);
    ctx_.processor.setTableLive(osc_, table_);
    if (!live)
        ctx_.session.commit(label);
    strip_->refreshSize();
    strip_->repaint();
    wave_->repaint();
    if (!live)
        harmonics_->refresh();
    if (spectral_->isVisible() && !live)
        spectral_->refresh();
    if (!live)
        reload();
}

void TablePage::finish(const juce::String& label) {
    if (label.isNotEmpty())
        ctx_.session.commit(label);
    harmonics_->refresh();
    if (spectral_->isVisible())
        spectral_->refresh();
    reload();
}

void TablePage::replaceTable(std::shared_ptr<zed::Wavetable> table, const juce::String& label) {
    if (table == nullptr || table->frames.empty())
        return;
    table_ = std::move(table);
    loaded_ = table_;
    ctx_.processor.setTableLive(osc_, table_);
    ctx_.session.commit(label);
    frame_ = 0;
    selection_ = {0};
    reload();
}

void TablePage::drawStroke(double x0, double y0, double x1, double y1, int steps) {
    const int frame = frame_;
    double lastX = x0, lastY = y0;
    for (int i = 1; i <= steps; ++i) {
        const double x = x0 + (x1 - x0) * i / steps, y = y0 + (y1 - y0) * i / steps;
        const double from = std::min(lastX, x), to = std::max(std::max(lastX, x), std::min(lastX, x) + 1.0 / zed::kFrameSize);
        const float a = static_cast<float>(lastX <= x ? lastY : y), b = static_cast<float>(lastX <= x ? y : lastY);
        apply("", [=](zed::Wavetable& t) {
            zed::drawSegment(t.frames[static_cast<std::size_t>(frame)], from, to, zed::DrawShape::Line, a, b);
            return true;
        }, true);
        lastX = x;
        lastY = y;
    }
    finish("Draw on frame " + juce::String(frame + 1));
}

void TablePage::showImport() {
    wizard_->begin();
    wizard_->setVisible(true);
    wizard_->toFront(true);
}

void TablePage::applyFormula() {
    zed::FormulaRequest request;
    request.domain = formulaDomain_ == 0 ? zed::FormulaDomain::Wave : zed::FormulaDomain::Harmonics;
    request.expression = formula_.getText().toStdString();
    request.phaseExpression = phaseFormula_.getText().toStdString();
    request.scope = formulaScope_ == 0 ? zed::oneFrame(frame_) : formulaScope_ == 1 ? range() : zed::allFrames();
    request.selectedFrame = frame_;
    request.normalize = true;
    std::string error;
    bool ok = true;
    apply("Formula " + formula_.getText().substring(0, 40), [&](zed::Wavetable& t) {
        ok = zed::applyFormula(t, request, error);
        return ok;
    });
    formulaError_.setText(ok ? juce::String("Applied.") : str(error));
}

void TablePage::showProcessMenu() {
    juce::PopupMenu menu;
    const auto op = [this](const juce::String& label, zed::ProcessOp o, int n) {
        const auto r = range();
        apply(label, [=](zed::Wavetable& t) { zed::processFrames(t, r, o, n); return true; });
    };
    menu.addSectionHeader("On the selected frames (" + juce::String(static_cast<int>(selection_.size())) + ")");
    menu.addItem("Normalize each", [op] { op("Normalize each", zed::ProcessOp::NormalizeEach, 0); });
    menu.addItem("Normalize to a shared peak", [op] { op("Normalize shared", zed::ProcessOp::NormalizeShared, 0); });
    menu.addItem("Remove DC", [op] { op("Remove DC", zed::ProcessOp::RemoveDc, 0); });
    menu.addItem("Flip polarity", [op] { op("Flip polarity", zed::ProcessOp::FlipPolarity, 0); });
    menu.addItem("Mirror in time", [op] { op("Mirror in time", zed::ProcessOp::MirrorTime, 0); });
    menu.addItem("Shift to zero crossing", [op] { op("Shift to zero crossing", zed::ProcessOp::ShiftToZeroCrossing, 0); });
    menu.addItem("Align fundamental phase", [op] { op("Align fundamental", zed::ProcessOp::AlignFundamental, 0); });
    menu.addItem("Remove fundamental", [op] { op("Remove fundamental", zed::ProcessOp::RemoveFundamental, 0); });
    menu.addItem("Clear", [op] { op("Clear frames", zed::ProcessOp::Clear, 0); });
    juce::PopupMenu fades, lowpass, reduce;
    for (const int n : {16, 64, 256}) {
        fades.addItem("Fade edges, " + juce::String(n) + " samples", [op, n] { op("Fade edges", zed::ProcessOp::FadeEdges, n); });
        fades.addItem("Crossfade edges, " + juce::String(n) + " samples", [op, n] { op("Crossfade edges", zed::ProcessOp::CrossfadeEdges, n); });
    }
    for (const int n : {4, 8, 16, 32, 64, 128})
        lowpass.addItem("Keep " + juce::String(n) + " harmonics", [op, n] { op("Low-pass " + juce::String(n), zed::ProcessOp::LowPass, n); });
    for (const int n : {4, 8, 16, 32, 64})
        reduce.addItem(juce::String(n) + " steps per cycle", [op, n] { op("Sample-rate reduce " + juce::String(n), zed::ProcessOp::SampleRateReduce, n); });
    menu.addSubMenu("Fades", fades);
    menu.addSubMenu("Low-pass", lowpass);
    menu.addSubMenu("Sample-rate reduce", reduce);
    menu.addSectionHeader("On the whole table");
    menu.addItem("Split each frame in two", [this] { apply("Split frames", [](zed::Wavetable& t) { return zed::splitFrames(t); }); });
    menu.addItem("Join frame pairs", [this] { apply("Join frame pairs", [](zed::Wavetable& t) { zed::joinFramePairs(t); return true; }); });
    menu.addItem("Pulse-width sweep from this frame", [this] {
        const zed::Frame f = table_->frames[static_cast<std::size_t>(frame_)];
        apply("Pulse-width sweep", [f](zed::Wavetable& t) { t = zed::pulseWidthSweep(f, 64); return true; });
    });
    menu.addItem("Copy this frame's spectrum to all", [this] { const int f = frame_; apply("Copy spectrum to all", [f](zed::Wavetable& t) { zed::copySpectrumToAll(t, f); return true; }); });
    menu.addItem("Copy this frame's phases to all", [this] { const int f = frame_; apply("Copy phases to all", [f](zed::Wavetable& t) { zed::copyPhasesToAll(t, f); return true; }); });
    juce::PopupMenu subtract;
    for (int o = 0; o < 3; ++o)
        if (o != osc_)
            subtract.addItem("Osc " + juce::String(o + 1), [this, o] {
                const auto other = ctx_.processor.editableTable(o);
                if (other != nullptr)
                    apply("Subtract Osc " + juce::String(o + 1) + " spectrum", [other](zed::Wavetable& t) { zed::subtractSpectrum(t, *other); return true; });
            });
    menu.addSubMenu("Subtract another oscillator's spectrum", subtract);
    menu.addItem("Trim silent frames", [this] { apply("Trim silent frames", [](zed::Wavetable& t) { return zed::trimSilentFrames(t) > 0 && !t.frames.empty(); }); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

void TablePage::showMorphMenu() {
    juce::PopupMenu menu;
    static const std::pair<const char*, zed::MorphMode> modes[] = {{"None (key frames only)", zed::MorphMode::None}, {"Crossfade", zed::MorphMode::Crossfade},
                                                                  {"Spectral", zed::MorphMode::Spectral}, {"Spectral, fundamental aligned", zed::MorphMode::SpectralAligned},
                                                                  {"Spectral, phases zeroed", zed::MorphMode::SpectralZero}};
    for (const auto& [name, mode] : modes) {
        const auto m = mode;
        menu.addItem(name, true, table_->morph == mode, [this, m, name] { apply(juce::String("Morph: ") + name, [m](zed::Wavetable& t) { t.morph = m; return true; }); });
    }
    menu.addItem("Stepped (no gliding between frames)", true, table_->stepped, [this] {
        apply(table_->stepped ? "Smooth positions" : "Stepped positions", [](zed::Wavetable& t) { t.stepped = !t.stepped; return true; });
    });
    menu.addSeparator();
    for (const int n : {32, 64, 128, 256})
        menu.addItem("Bake the morph into " + juce::String(n) + " frames", [this, n] { apply("Bake morph into " + juce::String(n), [n](zed::Wavetable& t) { zed::bakeMorph(t, n); return true; }); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

void TablePage::showSortMenu() {
    juce::PopupMenu menu;
    static const std::pair<const char*, zed::SortKey> keys[] = {{"Brightest peak", zed::SortKey::BrightestPeak}, {"Average brightness", zed::SortKey::Brightness},
                                                               {"Strongest partial", zed::SortKey::StrongestPartial}, {"Number of partials", zed::SortKey::ActivePartials},
                                                               {"Highest partial", zed::SortKey::HighestPartial}, {"Fundamental strength", zed::SortKey::Fundamental}};
    for (const auto& [name, key] : keys) {
        const auto k = key;
        menu.addItem(juce::String("By ") + name, [this, k, name] { apply(juce::String("Sort by ") + name, [k](zed::Wavetable& t) { zed::sortFrames(t, k); return true; }); });
        menu.addItem(juce::String("By ") + name + ", descending", [this, k, name] { apply(juce::String("Sort by ") + name, [k](zed::Wavetable& t) { zed::sortFrames(t, k, true); return true; }); });
    }
    menu.addItem("Invert the order", [this] { apply("Invert order", [](zed::Wavetable& t) { zed::reverseFrames(t); return true; }); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

void TablePage::showResynthMenu() {
    juce::PopupMenu menu;
    const juce::String m = "osc" + juce::String(osc_ + 1);
    const int mode = static_cast<int>(ctx_.processor.plainValue(reg().indexOf((m + ".warp_a.mode").toStdString())));
    const double position = ctx_.processor.plainValue(reg().indexOf((m + ".position").toStdString()));
    menu.addItem("Bake warp A across 64 frames", mode > 0 && !zed::osc::warpUsesModulator(static_cast<zed::osc::WarpMode>(mode)), false, [this, mode, position] {
        apply("Bake warp", [mode, position](zed::Wavetable& t) {
            t = zed::bakeWarp(t, position, static_cast<zed::osc::WarpMode>(mode), 64);
            return true;
        });
    });
    menu.addItem("Bake the whole sound (one bar at C3)", [this] {
        zed::BakePatchOptions options;
        options.frames = 64;
        for (int o = 0; o < 3; ++o)
            options.tables[static_cast<std::size_t>(o)] = ctx_.processor.engine().table(o);
        options.texture = ctx_.processor.engine().texture();
        const zed::Patch patch = ctx_.processor.snapshot();
        std::string error;
        zed::Wavetable baked;
        if (zed::bakePatch(patch, options, baked, error))
            replaceTable(std::make_shared<zed::Wavetable>(std::move(baked)), "Bake the sound");
        else if (ctx_.notify)
            ctx_.notify(str(error));
    });
    juce::PopupMenu lfos;
    for (int l = 0; l < zed::kNumLfos; ++l)
        lfos.addItem("LFO " + juce::String(l + 1), [this, l] {
            zed::Shape shape;
            ctx_.processor.readDocument([&](const zed::Patch& p) { shape = p.lfoShapes[static_cast<std::size_t>(l)]; });
            std::vector<float> values(1025);
            if (shape.empty())
                for (int i = 0; i <= 1024; ++i)
                    values[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(2.0 * zed::kPi * i / 1024.0));
            else
                zed::compileShape(shape, values.data(), 1024);
            const zed::Frame f = zed::shapeToFrame(values, false);
            const int at = frame_;
            apply("LFO " + juce::String(l + 1) + " shape to frame " + juce::String(at + 1), [f, at](zed::Wavetable& t) { t.frames[static_cast<std::size_t>(at)] = f; return true; });
        });
    menu.addSubMenu("Shape to wave (into this frame)", lfos);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

void TablePage::showExportMenu() {
    juce::PopupMenu menu;
    const auto save = [this](zed::Wavetable table, const juce::String& title) {
        auto chooser = std::make_shared<juce::FileChooser>(title, ctx_.prefs.userContentFolder().getChildFile("Wavetables").getChildFile(str(table.name.empty() ? "Table" : table.name) + ".wav"), "*.wav");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [chooser, table](const juce::FileChooser& fc) {
            if (fc.getResult() == juce::File())
                return;
            const auto bytes = zed::encodeWavetable(table);
            fc.getResult().replaceWithData(bytes.data(), bytes.size());
        });
    };
    menu.addItem("Whole table...", [this, save] { save(*table_, "Export the table"); });
    menu.addItem("Selected frames...", [this, save] {
        zed::Wavetable part = *table_;
        zed::cropFrames(part, range());
        save(part, "Export the selection");
    });
    menu.addItem("Each frame as a single-cycle file...", [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Folder for single cycles", ctx_.prefs.userContentFolder());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, chooser](const juce::FileChooser& fc) {
            const juce::File folder = fc.getResult();
            if (!folder.isDirectory())
                return;
            const auto files = zed::exportSingleCycles(*table_, range());
            for (std::size_t i = 0; i < files.size(); ++i)
                folder.getChildFile(str(table_->name.empty() ? "Cycle" : table_->name) + " " + juce::String(static_cast<int>(i) + 1).paddedLeft('0', 3) + ".wav")
                    .replaceWithData(files[i].data(), files[i].size());
            if (ctx_.notify)
                ctx_.notify(juce::String(static_cast<int>(files.size())) + " files written to " + folder.getFullPathName());
        });
    });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

void TablePage::saveTableAs() {
    auto dialog = std::make_shared<juce::AlertWindow>("Save table", "Saved in your Wavetables folder.", juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("name", str(table_->name.empty() ? "My Table" : table_->name), "Name");
    dialog->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, dialog](int result) {
        if (result != 1)
            return;
        const juce::String name = juce::File::createLegalFileName(dialog->getTextEditorContents("name").trim());
        if (name.isEmpty())
            return;
        zed::Wavetable table = *table_;
        table.name = name.toStdString();
        const auto bytes = zed::encodeWavetable(table);
        const juce::File file = juce::File(ctx_.processor.library().userDir().empty() ? ctx_.prefs.userContentFolder().getFullPathName()
                                                                                         : str(ctx_.processor.library().userDir()))
                                    .getChildFile("Wavetables").getChildFile(name + ".wav");
        file.getParentDirectory().createDirectory();
        if (!file.replaceWithData(bytes.data(), bytes.size())) {
            if (ctx_.notify)
                ctx_.notify("Could not write " + file.getFullPathName());
            return;
        }
        const std::string ref = "user:" + name.toStdString();
        ctx_.session.edit("Save table " + name, [&](zed::Patch& p) {
            p.tables[static_cast<std::size_t>(osc_)] = ref;
            p.tableData[static_cast<std::size_t>(osc_)] = {};
            return true;
        });
        reload();
    }), false);
}

namespace {
// The drawing shapes in the selector, by name.
const std::pair<const char*, WaveEditor::Tool> kMoreShapes[] = {{"Arch up", WaveEditor::Tool::HalfUp},      {"Arch down", WaveEditor::Tool::HalfDown},
                                                                {"Quarter in", WaveEditor::Tool::QuarterIn}, {"Quarter out", WaveEditor::Tool::QuarterOut},
                                                                {"Nudge", WaveEditor::Tool::Nudge},          {"Noise", WaveEditor::Tool::Noise}};
} // namespace

void TablePage::updateTools() {
    // The chosen shape is seated; a shape from the selector seats the selector and names it there.
    juce::String moreName;
    for (const auto& [name, tool] : kMoreShapes)
        if (wave_->tool == tool)
            moreName = name;
    for (auto& [b, tool] : shapeButtons_)
        b->setToggled(static_cast<int>(wave_->tool) == tool);
    moreShapes_->setToggled(moreName.isNotEmpty());
    moreShapes_->setText(moreName.isNotEmpty() ? moreName : juce::String("More shapes"));
    moreShapes_->setTitle(moreName.isNotEmpty() ? "More shapes: " + moreName : juce::String("More shapes"));
    snap_->setToggled(wave_->snap);
    grid_->setText("Grid " + juce::String(wave_->gridX));
    grid_->setTitle("Grid: " + juce::String(wave_->gridX) + " across");
    wave_->repaint(); // its caption names the frame and the shape
}

void TablePage::showMoreShapes() {
    juce::PopupMenu menu;
    for (const auto& [name, tool] : kMoreShapes) {
        const auto t = tool;
        menu.addItem(name, true, wave_->tool == tool, [this, t] {
            wave_->tool = t;
            updateTools();
        });
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(moreShapes_));
}

juce::String TablePage::toolName() const {
    static const char* const names[] = {"Free", "Flat", "Ramp up", "Ramp down", "Line", "Sine", "Arch up", "Arch down", "Quarter in", "Quarter out", "Curve", "Nudge", "Noise"};
    return names[static_cast<int>(wave_->tool)];
}

void TablePage::setTool(const juce::String& name) {
    for (int i = 0; i < 13; ++i) {
        wave_->tool = static_cast<WaveEditor::Tool>(i);
        if (toolName() == name)
            break;
    }
    updateTools();
}

std::vector<Button*> TablePage::toolButtons() const {
    std::vector<Button*> out;
    for (const auto& [b, tool] : shapeButtons_)
        out.push_back(b);
    out.push_back(moreShapes_);
    out.push_back(snap_);
    out.push_back(grid_);
    return out;
}

void TablePage::layoutRightTab() {
    const int tab = rightTabs_->selected();
    harmonics_->setVisible(tab == 0);
    spectral_->setVisible(tab == 1);
    if (tab == 1)
        spectral_->refresh();
    for (auto* c : formulaParts_) {
        const bool spectralPart = c->getProperties().getWithDefault("spectral", false);
        c->setVisible(spectralPart ? tab == 1 : tab == 2);
        if (auto* b = dynamic_cast<Button*>(c); b != nullptr && spectralPart && b->toggleRole && b->text() != "Lock harmonic")
            b->setToggled((b->text() == "Paint" && spectral_->brush == SpectralEditor::Brush::Paint) || (b->text() == "Erase" && spectral_->brush == SpectralEditor::Brush::Erase) ||
                          (b->text() == "Smooth" && spectral_->brush == SpectralEditor::Brush::Smooth));
    }
    formula_.setVisible(tab == 2);
    phaseFormula_.setVisible(tab == 2);
    formulaError_.setVisible(tab == 2);
    resized();
}

void TablePage::resized() {
    auto area = getLocalBounds().reduced(8);
    auto toolbar = area.removeFromTop(28);
    std::vector<FlowItem> items;
    for (auto* c : toolbar_)
        items.push_back({c, c == name_ ? 330 : dynamic_cast<Button*>(c) != nullptr && dynamic_cast<Button*>(c)->text().startsWith("Osc") ? 56 : 92, 26});
    flow(toolbar, items, 26, 6);
    area.removeFromTop(8);
    // The frames and the commands that act on them, side by side: the strip is as wide as its
    // frames (it scrolls when they do not fit), the commands right after it.
    auto stripRow = area.removeFromTop(78);
    frameCaption_->setBounds(stripRow.removeFromLeft(54).removeFromTop(16));
    constexpr int kButtonsW = 3 * 90 + 2 * 4;
    const int stripW = juce::jmin(table_->size() * FrameStrip::kCell + 4, stripRow.getWidth() - kButtonsW - 8);
    stripView_.setBounds(stripRow.removeFromLeft(stripW));
    strip_->setSize(juce::jmax(stripW, table_->size() * FrameStrip::kCell + 4), stripRow.getHeight() - stripView_.getScrollBarThickness());
    stripRow.removeFromLeft(8);
    std::vector<FlowItem> frameItems;
    for (auto* c : frameButtons_)
        frameItems.push_back({c, 90, 22});
    flow(stripRow.withWidth(kButtonsW).withSizeKeepingCentre(kButtonsW, 48), frameItems, 22, 4);
    area.removeFromTop(8);
    auto left = area.removeFromLeft((area.getWidth() - 8) / 2);
    area.removeFromLeft(8);
    // The shapes over the wave they draw on, in one row.
    auto tools = left.removeFromTop(24);
    shapeCaption_->setBounds(tools.removeFromLeft(40));
    for (auto& [b, tool] : shapeButtons_) {
        b->setBounds(tools.removeFromLeft(30));
        tools.removeFromLeft(3);
    }
    tools.removeFromLeft(3);
    moreShapes_->setBounds(tools.removeFromLeft(118));
    tools.removeFromLeft(14);
    snap_->setBounds(tools.removeFromLeft(66));
    tools.removeFromLeft(4);
    grid_->setBounds(tools.removeFromLeft(82));
    updateTools();
    left.removeFromTop(6);
    wave_->setBounds(left);
    rightTabs_->setBounds(area.removeFromTop(24));
    area.removeFromTop(6);
    const int tab = rightTabs_->selected();
    if (tab == 2) {
        formula_.setBounds(area.removeFromTop(28));
        area.removeFromTop(6);
        phaseFormula_.setBounds(area.removeFromTop(28));
        area.removeFromTop(6);
        std::vector<FlowItem> f;
        for (auto* c : formulaParts_)
            if (c->isVisible())
                f.push_back({c, 96, 26});
        flow(area.removeFromTop(26), f, 26, 6);
        area.removeFromTop(6);
        formulaError_.setBounds(area.removeFromTop(18));
    } else if (tab == 1) {
        std::vector<FlowItem> f;
        for (auto* c : formulaParts_)
            if (c->isVisible())
                f.push_back({c, 110, 24});
        flow(area.removeFromTop(24), f, 24, 6);
        area.removeFromTop(6);
        spectral_->setBounds(area);
    } else {
        harmonics_->setBounds(area);
    }
    wizard_->setBounds(getLocalBounds().reduced(60, 30));
}

void TablePage::paint(juce::Graphics& g) {
    const auto area = getLocalBounds().toFloat();
    drawPanel(g, area, theme().panel);
    drawPlate(g, area.withBottom(static_cast<float>(stripView_.getY() - 2)));
    drawPlate(g, area.withTop(static_cast<float>(stripView_.getY() - 2)).withBottom(static_cast<float>(rightTabs_->getY() - 4)));
    g.setColour(theme().line);
    g.fillRect(area.withLeft(static_cast<float>(rightTabs_->getX() - 4)).withWidth(1.0f).withTop(static_cast<float>(rightTabs_->getY() - 4)));
}

bool TablePage::reveal(const juce::String& module) {
    if (module.startsWith("table"))
        setOsc(juce::jlimit(1, 3, module.getTrailingIntValue()) - 1);
    return true;
}

void TablePage::saveState(nlohmann::json& state) const {
    state["osc"] = osc_;
    state["frame"] = frame_;
    state["view"] = rightTabs_->selected();
}

void TablePage::loadState(const nlohmann::json& state) {
    setOsc(state.value("osc", 0));
    setFrame(state.value("frame", 0));
    rightTabs_->setSelected(juce::jlimit(0, 2, state.value("view", 0)));
}

} // namespace zui

namespace zui {
juce::Component& TablePage::waveEditor() { return *wave_; }
juce::Component& TablePage::importWizard() { return *wizard_; }
} // namespace zui
