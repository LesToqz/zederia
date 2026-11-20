#include "CurveEditor.h"

#include "zederia/util/Math.h"

#include <cmath>

namespace zui {

namespace {

class ShapeDescription final : public juce::AccessibilityTextValueInterface {
public:
    explicit ShapeDescription(const CurveEditor& e) : e_(e) {}
    bool isReadOnly() const override { return true; }
    juce::String getCurrentValueAsString() const override {
        const auto& s = e_.shape();
        if (s.points.empty())
            return "default shape";
        juce::String text = juce::String(static_cast<int>(s.points.size())) + " points";
        const auto sel = e_.selection();
        if (sel.size() == 1) {
            const auto& p = s.points[static_cast<std::size_t>(sel.front())];
            text << ", point " << (sel.front() + 1) << " at x " << juce::String(p.x, 2) << ", y " << juce::String(p.y, 2);
        }
        return text;
    }
    void setValueAsString(const juce::String&) override {}

private:
    const CurveEditor& e_;
};

} // namespace

CurveEditor::CurveEditor(Context& ctx, juce::String name) : ctx_(ctx), name_(std::move(name)) {
    setWantsKeyboardFocus(true);
    keepsClickFocus = true; // Delete removes the selected points
    setTitle(name_);
    family = Family::Modulators;
    setHelp(name_, "Double-click to add or remove a point. Drag points; drag the small diamonds to bend a segment. "
                   "Shift-click or drag a frame to select several; Ctrl-drag scales them. Right-click for grid, snapping, "
                   "step drawing, shapes, copy and paste.");
}

void CurveEditor::setShape(const zed::Shape& shape) {
    if (dragPoint_ >= 0 || dragHandle_ >= 0 || drawing_)
        return; // the person is editing it
    if (shape == shape_ && !shape_.points.empty())
        return;
    shape_ = shape;
    if (shape_.points.empty()) { // the built-in shape: shown as what it is, so it can be edited
        if (config_.bipolar)
            applyPreset("Sine");
        else
            applyPreset(config_.anchoredStart ? "Line" : "S-curve");
        shape_.loopStart = shape.loopStart;
    }
    selected_.erase(std::remove_if(selected_.begin(), selected_.end(), [&](int i) { return i >= static_cast<int>(shape_.points.size()); }),
                    selected_.end());
    repaint();
}

juce::StringArray CurveEditor::presetNames() {
    return {"Sine", "Triangle", "Saw up", "Saw down", "Square", "Random steps", "Line", "S-curve"};
}

void CurveEditor::applyPreset(const juce::String& name) {
    std::vector<zed::ShapePoint> p;
    if (name == "Sine")
        p = {{0.0, 0.0, 0.55}, {0.25, 1.0, -0.55}, {0.5, 0.0, 0.55}, {0.75, -1.0, -0.55}, {1.0, 0.0, 0.0}};
    else if (name == "Triangle")
        p = {{0.0, 0.0, 0.0}, {0.25, 1.0, 0.0}, {0.75, -1.0, 0.0}, {1.0, 0.0, 0.0}};
    else if (name == "Saw up")
        p = {{0.0, -1.0, 0.0}, {1.0, 1.0, 0.0}};
    else if (name == "Saw down")
        p = {{0.0, 1.0, 0.0}, {1.0, -1.0, 0.0}};
    else if (name == "Square")
        p = {{0.0, 1.0, 0.0}, {0.5, 1.0, 0.0}, {0.5, -1.0, 0.0}, {1.0, -1.0, 0.0}};
    else if (name == "Random steps") {
        zed::Rng rng(static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks()));
        for (int i = 0; i < 8; ++i) {
            const double v = rng.uniform() * 2.0 - 1.0;
            p.push_back({i / 8.0, v, 0.0});
            p.push_back({(i + 1) / 8.0, v, 0.0});
        }
    } else if (name == "Line")
        p = {{0.0, -1.0, 0.0}, {1.0, 1.0, 0.0}};
    else // S-curve
        p = {{0.0, -1.0, -0.6}, {0.5, 0.0, 0.6}, {1.0, 1.0, 0.0}};
    if (!config_.bipolar)
        for (auto& q : p)
            q.y = (q.y + 1.0) * 0.5;
    if (config_.anchoredStart && !p.empty()) {
        if (p.front().x > 0.0 || p.front().y != 0.0)
            p.insert(p.begin(), {0.0, 0.0, 0.0});
        p.front() = {0.0, 0.0, p.front().curve};
    }
    shape_.points = std::move(p);
    selected_.clear();
}

void CurveEditor::selectAll() {
    selected_.clear();
    for (int i = 0; i < static_cast<int>(shape_.points.size()); ++i)
        selected_.push_back(i);
    repaint();
}

void CurveEditor::deleteSelected() {
    const int n = static_cast<int>(shape_.points.size());
    std::vector<zed::ShapePoint> kept;
    for (int i = 0; i < n; ++i) {
        const bool end = (i == 0 && (config_.fixedEnds || config_.anchoredStart)) || (i == n - 1 && config_.fixedEnds);
        if (end || std::find(selected_.begin(), selected_.end(), i) == selected_.end())
            kept.push_back(shape_.points[static_cast<std::size_t>(i)]);
    }
    if (kept.size() < 2 || kept.size() == shape_.points.size())
        return;
    shape_.points = std::move(kept);
    selected_.clear();
    changed();
    commit("Delete points");
}

juce::Rectangle<float> CurveEditor::plotArea() const {
    auto area = getLocalBounds().toFloat().reduced(4.0f);
    if (config_.xUnit.isNotEmpty())
        area.removeFromBottom(13.0f);
    return area;
}

juce::Point<float> CurveEditor::toScreen(double x, double y) const {
    const auto a = plotArea().reduced(4.0f);
    const double t = (y - yLow()) / (1.0 - yLow());
    return {a.getX() + a.getWidth() * static_cast<float>(x), a.getBottom() - a.getHeight() * static_cast<float>(t)};
}

juce::Point<double> CurveEditor::fromScreen(juce::Point<float> p) const {
    const auto a = plotArea().reduced(4.0f);
    const double x = juce::jlimit(0.0, 1.0, static_cast<double>((p.x - a.getX()) / a.getWidth()));
    const double t = juce::jlimit(0.0, 1.0, static_cast<double>((a.getBottom() - p.y) / a.getHeight()));
    return {x, yLow() + t * (1.0 - yLow())};
}

double CurveEditor::snapX(double x) const { return snap ? std::round(x * gridX) / gridX : x; }
double CurveEditor::snapY(double y) const {
    if (!snap)
        return y;
    const double range = 1.0 - yLow();
    const int divisions = config_.bipolar ? gridY * 2 : gridY;
    return yLow() + std::round((y - yLow()) / range * divisions) / divisions * range;
}

CurveEditor::Hit CurveEditor::hitTest(juce::Point<float> p, int& index) const {
    const int n = static_cast<int>(shape_.points.size());
    for (int i = n - 1; i >= 0; --i) {
        const auto& q = shape_.points[static_cast<std::size_t>(i)];
        if (toScreen(q.x, q.y).getDistanceFrom(p) <= 6.0f) {
            index = i;
            return Hit::Point;
        }
    }
    for (int i = 0; i + 1 < n; ++i) {
        const auto& a = shape_.points[static_cast<std::size_t>(i)];
        const auto& b = shape_.points[static_cast<std::size_t>(i + 1)];
        if (b.x - a.x < 1e-6)
            continue;
        const double mx = (a.x + b.x) * 0.5;
        if (toScreen(mx, zed::shapeValue(shape_, mx)).getDistanceFrom(p) <= 6.0f) {
            index = i;
            return Hit::Handle;
        }
    }
    index = -1;
    return Hit::None;
}

void CurveEditor::changed() {
    if (onChange)
        onChange(shape_);
    repaint();
}

void CurveEditor::commit(const juce::String& label) {
    if (onCommit)
        onCommit(shape_, name_ + ": " + label);
}

void CurveEditor::sortAndClamp() {
    auto& pts = shape_.points;
    for (auto& p : pts) {
        p.x = juce::jlimit(0.0, 1.0, p.x);
        p.y = juce::jlimit(yLow(), 1.0, p.y);
        p.curve = juce::jlimit(-1.0, 1.0, p.curve);
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
    if (config_.fixedEnds && pts.size() >= 2) {
        pts.front().x = 0.0;
        pts.back().x = 1.0;
    }
    if (config_.anchoredStart && !pts.empty())
        pts.front() = {0.0, 0.0, pts.front().curve};
}

bool CurveEditor::tick() {
    if (!playhead)
        return false;
    const float x = static_cast<float>(playhead());
    if (std::abs(x - playhead_) > 0.002f) {
        playhead_ = x;
        return true;
    }
    return false;
}

void CurveEditor::mouseMove(const juce::MouseEvent& e) {
    int index = -1;
    const Hit hit = hitTest(e.position, index);
    const int point = hit == Hit::Point ? index : -1, handle = hit == Hit::Handle ? index : -1;
    if (point != hoverPoint_ || handle != hoverHandle_) {
        hoverPoint_ = point;
        hoverHandle_ = handle;
        repaint();
    }
}

void CurveEditor::mouseExit(const juce::MouseEvent&) {
    hoverPoint_ = hoverHandle_ = -1;
    repaint();
}

void CurveEditor::mouseDown(const juce::MouseEvent& e) {
    moved_ = false;
    int index = -1;
    const Hit hit = hitTest(e.position, index);
    if (e.mods.isPopupMenu()) {
        showMenu(e.position, hit == Hit::Point ? index : -1);
        return;
    }
    if (stepMode) {
        drawing_ = true;
        mouseDrag(e);
        return;
    }
    if (hit == Hit::Point) {
        const bool isSelected = std::find(selected_.begin(), selected_.end(), index) != selected_.end();
        if (e.mods.isShiftDown()) {
            if (isSelected)
                selected_.erase(std::find(selected_.begin(), selected_.end(), index));
            else
                selected_.push_back(index);
        } else if (!isSelected) {
            selected_ = {index};
        }
        dragPoint_ = index;
        dragOrigin_ = shape_.points;
        dragAnchor_ = fromScreen(e.position);
    } else if (hit == Hit::Handle) {
        dragHandle_ = index;
        handleStartCurve_ = shape_.points[static_cast<std::size_t>(index)].curve;
        dragAnchor_ = fromScreen(e.position);
    } else {
        if (!e.mods.isShiftDown())
            selected_.clear();
        rubber_ = true;
        rubberStart_ = rubberEnd_ = e.position;
    }
    repaint();
}

void CurveEditor::mouseDrag(const juce::MouseEvent& e) {
    const auto at = fromScreen(e.position);
    if (drawing_) { // step drawing: each grid column becomes a flat step at the pointer's height
        const int columns = juce::jlimit(1, 32, gridX);
        std::vector<double> steps(static_cast<std::size_t>(columns));
        for (int c = 0; c < columns; ++c)
            steps[static_cast<std::size_t>(c)] = zed::shapeValue(shape_, (c + 0.5) / columns);
        const int column = juce::jlimit(0, columns - 1, static_cast<int>(at.x * columns));
        steps[static_cast<std::size_t>(column)] = snapY(at.y);
        shape_.points.clear();
        for (int c = 0; c < columns; ++c) {
            shape_.points.push_back({static_cast<double>(c) / columns, steps[static_cast<std::size_t>(c)], 0.0});
            shape_.points.push_back({static_cast<double>(c + 1) / columns, steps[static_cast<std::size_t>(c)], 0.0});
        }
        sortAndClamp();
        moved_ = true;
        changed();
        return;
    }
    if (dragHandle_ >= 0) {
        auto& p = shape_.points[static_cast<std::size_t>(dragHandle_)];
        const auto& next = shape_.points[static_cast<std::size_t>(dragHandle_ + 1)];
        const double direction = next.y >= p.y ? 1.0 : -1.0;
        p.curve = juce::jlimit(-1.0, 1.0, handleStartCurve_ + (at.y - dragAnchor_.y) / (1.0 - yLow()) * 3.0 * direction);
        moved_ = true;
        changed();
        return;
    }
    if (dragPoint_ >= 0) {
        const int n = static_cast<int>(dragOrigin_.size());
        const auto isSel = [&](int i) { return std::find(selected_.begin(), selected_.end(), i) != selected_.end(); };
        if (e.mods.isCommandDown() && selected_.size() > 1) { // proportional: scale the selection around its mean
            double mean = 0.0;
            for (const int i : selected_)
                mean += dragOrigin_[static_cast<std::size_t>(i)].y;
            mean /= static_cast<double>(selected_.size());
            const double factor = juce::jlimit(-2.0, 3.0, 1.0 + (at.y - dragAnchor_.y) * 2.0);
            for (const int i : selected_)
                shape_.points[static_cast<std::size_t>(i)].y = mean + (dragOrigin_[static_cast<std::size_t>(i)].y - mean) * factor;
        } else {
            double dx = at.x - dragAnchor_.x, dy = at.y - dragAnchor_.y;
            const auto& anchor = dragOrigin_[static_cast<std::size_t>(dragPoint_)];
            if (snap && !e.mods.isAltDown()) {
                dx = snapX(anchor.x + dx) - anchor.x;
                dy = snapY(anchor.y + dy) - anchor.y;
            }
            // Keep the moved points between their unselected neighbours.
            double lo = -1.0, hi = 1.0;
            for (const int i : selected_) {
                const double x = dragOrigin_[static_cast<std::size_t>(i)].x;
                double left = 0.0, right = 1.0;
                for (int j = i - 1; j >= 0; --j)
                    if (!isSel(j)) { left = dragOrigin_[static_cast<std::size_t>(j)].x; break; }
                for (int j = i + 1; j < n; ++j)
                    if (!isSel(j)) { right = dragOrigin_[static_cast<std::size_t>(j)].x; break; }
                const bool fixedX = ((config_.fixedEnds || config_.anchoredStart) && i == 0) || (config_.fixedEnds && i == n - 1);
                if (fixedX) {
                    lo = std::max(lo, 0.0);
                    hi = std::min(hi, 0.0);
                } else {
                    lo = std::max(lo, left - x);
                    hi = std::min(hi, right - x);
                }
            }
            dx = juce::jlimit(std::min(lo, 0.0), std::max(hi, 0.0), dx);
            for (const int i : selected_) {
                auto& p = shape_.points[static_cast<std::size_t>(i)];
                const auto& o = dragOrigin_[static_cast<std::size_t>(i)];
                p.x = o.x + dx;
                p.y = juce::jlimit(yLow(), 1.0, o.y + dy);
            }
        }
        if (config_.anchoredStart && !shape_.points.empty())
            shape_.points.front() = {0.0, 0.0, shape_.points.front().curve};
        moved_ = true;
        changed();
        return;
    }
    if (rubber_) {
        rubberEnd_ = e.position;
        const auto box = juce::Rectangle<float>(rubberStart_, rubberEnd_);
        if (!e.mods.isShiftDown())
            selected_.clear();
        for (int i = 0; i < static_cast<int>(shape_.points.size()); ++i) {
            const auto& q = shape_.points[static_cast<std::size_t>(i)];
            if (box.contains(toScreen(q.x, q.y)) && std::find(selected_.begin(), selected_.end(), i) == selected_.end())
                selected_.push_back(i);
        }
        repaint();
    }
}

void CurveEditor::mouseUp(const juce::MouseEvent&) {
    const bool wasDrawing = drawing_, wasHandle = dragHandle_ >= 0, wasPoint = dragPoint_ >= 0;
    drawing_ = false;
    rubber_ = false;
    dragPoint_ = dragHandle_ = -1;
    if (moved_) {
        sortAndClamp();
        commit(wasDrawing ? "Draw steps" : wasHandle ? "Bend segment" : wasPoint ? (selected_.size() > 1 ? "Move points" : "Move point") : "Edit");
    }
    moved_ = false;
    repaint();
}

void CurveEditor::mouseDoubleClick(const juce::MouseEvent& e) {
    if (stepMode || e.mods.isPopupMenu())
        return;
    int index = -1;
    const Hit hit = hitTest(e.position, index);
    if (hit == Hit::Point) {
        selected_ = {index};
        deleteSelected();
        return;
    }
    if (static_cast<int>(shape_.points.size()) >= config_.maxPoints)
        return;
    const auto at = fromScreen(e.position);
    zed::ShapePoint p{snapX(at.x), snapY(at.y), 0.0};
    if (config_.fixedEnds && (p.x <= 0.0 || p.x >= 1.0))
        return;
    shape_.points.push_back(p);
    sortAndClamp();
    selected_.clear();
    for (int i = 0; i < static_cast<int>(shape_.points.size()); ++i)
        if (shape_.points[static_cast<std::size_t>(i)] == p)
            selected_ = {i};
    changed();
    commit("Add point");
}

bool CurveEditor::keyPressed(const juce::KeyPress& key) {
    const int code = key.getKeyCode();
    const bool cmd = key.getModifiers().isCommandDown();
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) {
        deleteSelected();
        return true;
    }
    if (cmd && (code == 'A' || code == 'a')) {
        selectAll();
        return true;
    }
    if (cmd && (code == 'C' || code == 'c') && onCopy) {
        onCopy();
        return true;
    }
    if (cmd && (code == 'V' || code == 'v') && onPaste) {
        onPaste();
        return true;
    }
    const int n = static_cast<int>(shape_.points.size());
    if (n == 0)
        return false;
    const bool shift = key.getModifiers().isShiftDown();
    if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && !shift) { // move through the points
        const int current = selected_.empty() ? -1 : selected_.front();
        const int next = juce::jlimit(0, n - 1, code == juce::KeyPress::rightKey ? current + 1 : (current < 0 ? n - 1 : current - 1));
        selected_ = {next};
        repaint();
        return true;
    }
    if (selected_.empty())
        return false;
    const double stepY = (1.0 - yLow()) / (snap ? (config_.bipolar ? gridY * 2 : gridY) : 50);
    const double stepX = snap ? 1.0 / gridX : 0.01;
    double dx = 0.0, dy = 0.0;
    if (code == juce::KeyPress::upKey) dy = stepY;
    else if (code == juce::KeyPress::downKey) dy = -stepY;
    else if (code == juce::KeyPress::leftKey) dx = -stepX;
    else if (code == juce::KeyPress::rightKey) dx = stepX;
    else return false;
    for (const int i : selected_) {
        auto& p = shape_.points[static_cast<std::size_t>(i)];
        const bool fixedX = ((config_.fixedEnds || config_.anchoredStart) && i == 0) || (config_.fixedEnds && i == n - 1);
        const double left = i > 0 ? shape_.points[static_cast<std::size_t>(i - 1)].x : 0.0;
        const double right = i + 1 < n ? shape_.points[static_cast<std::size_t>(i + 1)].x : 1.0;
        if (!fixedX)
            p.x = juce::jlimit(left, right, p.x + dx);
        if (!(config_.anchoredStart && i == 0))
            p.y = juce::jlimit(yLow(), 1.0, p.y + dy);
    }
    changed();
    commit("Nudge points");
    return true;
}

void CurveEditor::showMenu(juce::Point<float>, int point) {
    juce::PopupMenu menu;
    if (point >= 0) {
        selected_ = {point};
        menu.addItem("Delete point", [this] { deleteSelected(); });
        menu.addItem("Straighten segment", [this, point] {
            shape_.points[static_cast<std::size_t>(point)].curve = 0.0;
            changed();
            commit("Straighten segment");
        });
        if (config_.envelopeMarkers) {
            menu.addItem("Sustain here", true, sustainPoint == point, [this, point] {
                sustainPoint = sustainPoint == point ? -1 : point;
                if (onMarkers) onMarkers(sustainPoint, loopStart, loopEnd);
                repaint();
            });
            menu.addItem("Loop starts here", true, loopStart == point, [this, point] {
                loopStart = loopStart == point ? -1 : point;
                if (onMarkers) onMarkers(sustainPoint, loopStart, loopEnd);
                repaint();
            });
            menu.addItem("Loop ends here", true, loopEnd == point, [this, point] {
                loopEnd = loopEnd == point ? -1 : point;
                if (onMarkers) onMarkers(sustainPoint, loopStart, loopEnd);
                repaint();
            });
        }
        menu.addSeparator();
    }
    juce::PopupMenu gx, gy, presets;
    for (const int d : {2, 3, 4, 6, 8, 12, 16, 32})
        gx.addItem(juce::String(d), true, gridX == d, [this, d] { gridX = d; repaint(); });
    for (const int d : {1, 2, 3, 4, 6, 8})
        gy.addItem(juce::String(d), true, gridY == d, [this, d] { gridY = d; repaint(); });
    for (const auto& name : presetNames())
        presets.addItem(name, [this, name] {
            applyPreset(name);
            changed();
            commit("Shape " + name);
        });
    menu.addSubMenu("Grid across", gx);
    menu.addSubMenu("Grid up and down", gy);
    menu.addItem("Snap to grid", true, snap, [this] { snap = !snap; });
    menu.addItem("Step drawing", true, stepMode, [this] { stepMode = !stepMode; repaint(); });
    if (config_.loopPoint)
        menu.addItem("Loop from here", [this, point] {
            if (point >= 0)
                shape_.loopStart = shape_.points[static_cast<std::size_t>(point)].x;
            changed();
            commit("Loop point");
        });
    menu.addSeparator();
    menu.addSubMenu("Shapes", presets);
    menu.addItem("Select all", [this] { selectAll(); });
    if (onCopy)
        menu.addItem("Copy shape", [this] { onCopy(); });
    if (onPaste)
        menu.addItem("Paste shape", [this] { onPaste(); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

void CurveEditor::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto plot = plotArea();
    drawWell(g, plot);
    const auto inner = plot.reduced(4.0f);
    // Grid.
    g.setColour(t.displayGrid);
    for (int i = 1; i < gridX; ++i) {
        const float x = inner.getX() + inner.getWidth() * static_cast<float>(i) / static_cast<float>(gridX);
        g.fillRect(x, inner.getY(), 1.0f, inner.getHeight());
    }
    const int rows = config_.bipolar ? gridY * 2 : gridY;
    for (int i = 1; i < rows; ++i) {
        const float y = inner.getY() + inner.getHeight() * static_cast<float>(i) / static_cast<float>(rows);
        g.fillRect(inner.getX(), y, inner.getWidth(), 1.0f);
    }
    if (config_.bipolar) {
        g.setColour(t.lineStrong);
        g.fillRect(inner.getX(), toScreen(0.0, 0.0).y, inner.getWidth(), 1.0f);
    }
    const juce::Colour accent = ink.value_or(t.displayLine);
    // Envelope loop region and sustain point; LFO loop start.
    const int n = static_cast<int>(shape_.points.size());
    if (config_.envelopeMarkers && loopStart >= 0 && loopEnd > loopStart && loopEnd < n) {
        const float x0 = toScreen(shape_.points[static_cast<std::size_t>(loopStart)].x, 0).x;
        const float x1 = toScreen(shape_.points[static_cast<std::size_t>(loopEnd)].x, 0).x;
        g.setColour(accent.withAlpha(0.1f));
        g.fillRect(juce::Rectangle<float>(x0, inner.getY(), x1 - x0, inner.getHeight()));
        drawText(g, "loop", juce::Rectangle<float>(x0 + 3.0f, inner.getY() + 1.0f, 40.0f, 13.0f), kTextSmall, t.textDim);
    }
    if (config_.envelopeMarkers && sustainPoint >= 0 && sustainPoint < n) {
        const float x = toScreen(shape_.points[static_cast<std::size_t>(sustainPoint)].x, 0).x;
        juce::Path line;
        line.startNewSubPath(x, inner.getY());
        line.lineTo(x, inner.getBottom());
        juce::Path dashed;
        const float dashes[] = {3.0f, 3.0f};
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, line, dashes, 2);
        g.setColour(t.textDim);
        g.fillPath(dashed);
        drawText(g, "sustain", juce::Rectangle<float>(x + 3.0f, inner.getBottom() - 14.0f, 52.0f, 13.0f), kTextSmall, t.textDim);
    }
    if (config_.loopPoint && shape_.loopStart > 0.0) {
        const float x = toScreen(shape_.loopStart, 0).x;
        g.setColour(t.textDim);
        g.fillRect(x, inner.getY(), 1.0f, inner.getHeight());
    }
    // The curve.
    if (n > 0) {
        juce::Path curve, fill;
        const int samples = juce::jmax(64, static_cast<int>(inner.getWidth()));
        const float base = toScreen(0.0, config_.bipolar ? 0.0 : 0.0).y;
        for (int i = 0; i <= samples; ++i) {
            const double x = static_cast<double>(i) / samples;
            const auto p = toScreen(x, zed::shapeValue(shape_, x));
            if (i == 0) {
                curve.startNewSubPath(p);
                fill.startNewSubPath(p.x, base);
                fill.lineTo(p);
            } else {
                curve.lineTo(p);
                fill.lineTo(p);
            }
        }
        fill.lineTo(inner.getRight(), base);
        fill.closeSubPath();
        g.setColour(accent.withAlpha(0.12f));
        g.fillPath(fill);
        strokeGlow(g, curve, accent, 1.6f);
        // Handles and points.
        for (int i = 0; i + 1 < n; ++i) {
            const auto& a = shape_.points[static_cast<std::size_t>(i)];
            const auto& b = shape_.points[static_cast<std::size_t>(i + 1)];
            if (b.x - a.x < 1e-6)
                continue;
            const double mx = (a.x + b.x) * 0.5;
            const auto c = toScreen(mx, zed::shapeValue(shape_, mx));
            juce::Path diamond;
            const float s = i == hoverHandle_ || i == dragHandle_ ? 4.5f : 3.0f;
            diamond.addQuadrilateral(c.x, c.y - s, c.x + s, c.y, c.x, c.y + s, c.x - s, c.y);
            g.setColour(i == hoverHandle_ || i == dragHandle_ ? accent : t.textFaint);
            g.fillPath(diamond);
        }
        for (int i = 0; i < n; ++i) {
            const auto& q = shape_.points[static_cast<std::size_t>(i)];
            const auto c = toScreen(q.x, q.y);
            const bool sel = std::find(selected_.begin(), selected_.end(), i) != selected_.end();
            const float r = sel || i == hoverPoint_ ? 4.5f : 3.5f;
            g.setColour(sel ? t.focus : t.display);
            g.fillEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(c));
            g.setColour(sel ? t.focus : accent);
            g.drawEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(c), 1.4f);
        }
    }
    if (playhead_ >= 0.0f && playhead_ <= 1.0f && n > 0) {
        const auto p = toScreen(playhead_, zed::shapeValue(shape_, playhead_));
        g.setColour(t.displayLine.withAlpha(0.35f));
        g.fillRect(p.x, inner.getY(), 1.0f, inner.getHeight());
        g.setColour(t.displayLine);
        g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(p));
    }
    if (rubber_) {
        const auto box = juce::Rectangle<float>(rubberStart_, rubberEnd_);
        g.setColour(t.focus.withAlpha(0.12f));
        g.fillRect(box);
        g.setColour(t.focus);
        g.drawRect(box, 1.0f);
    }
    if (stepMode)
        drawText(g, "STEP", juce::Rectangle<float>(inner.getRight() - 40.0f, inner.getY() + 2.0f, 38.0f, 13.0f), kTextSmall, t.focus,
                 juce::Justification::centredRight, true);
    if (config_.xUnit.isNotEmpty()) {
        const auto axis = getLocalBounds().toFloat().reduced(4.0f).removeFromBottom(13.0f).reduced(4.0f, 0.0f);
        const auto label = [&](double fraction) {
            const double v = fraction * config_.xScale;
            return (v >= 1000.0 ? juce::String(v / 1000.0, 2) + " s" : juce::String(juce::roundToInt(v)) + " " + config_.xUnit);
        };
        drawText(g, "0", axis.withWidth(40.0f), kTextSmall, t.textDim);
        drawText(g, label(0.5), axis.withSizeKeepingCentre(80.0f, 13.0f), kTextSmall, t.textDim, juce::Justification::centred);
        drawText(g, label(1.0), axis.withLeft(axis.getRight() - 80.0f), kTextSmall, t.textDim, juce::Justification::centredRight);
    }
    drawFocus(g, getLocalBounds().toFloat());
}

std::unique_ptr<juce::AccessibilityHandler> CurveEditor::createAccessibilityHandler() {
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::group, juce::AccessibilityActions(),
                                                        juce::AccessibilityHandler::Interfaces{std::make_unique<ShapeDescription>(*this)});
}

} // namespace zui
