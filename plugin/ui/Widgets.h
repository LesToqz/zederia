// The component kit (plan 3.13, 8 "component kit first"): every page is built from these parts.
// All drawing is vector; sizes are in base pixels (100 % of 1320 x 820) and the editor scales the
// whole tree. Each control has a visible label, a help text for the help strip, an accessible name
// and value, and keyboard access.
#pragma once

#include "Context.h"

#include <functional>
#include <optional>

namespace zui {

// --- Base -------------------------------------------------------------------------------------------

class Widget : public juce::Component, public juce::SettableTooltipClient {
public:
    Widget();
    void paint(juce::Graphics& g) final; // records the painting component for the snapshot checks
    virtual void draw(juce::Graphics& g) = 0;
    // Called by the frame clock while the widget is showing; returns true to repaint.
    virtual bool tick() { return false; }
    // After tick() returned true: the part of the widget that changed (all of it unless the widget
    // knows better, as a display whose marker moved does).
    virtual juce::RectangleList<int> dirtyArea() { return getLocalBounds(); }
    // For the snapshot checks: false for parts that rely on a tooltip and an accessible name.
    virtual bool hasVisibleLabel() const { return true; }

    void setHelp(const juce::String& title, const juce::String& text);
    juce::String helpTitle, helpText;
    bool advanced = false; // hidden in Simple view
    // The visible text that names this control when it draws no label of its own (a card title).
    juce::String labelledBy;
    Family family = Family::Neutral;

protected:
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }
    void drawFocus(juce::Graphics& g, juce::Rectangle<float> area, float radius = 4.0f) const;
};

// A text line that is not a control.
class Caption final : public Widget {
public:
    explicit Caption(juce::String text = {}, float size = kTextSmall, bool dim = true);
    void setText(const juce::String& text);
    const juce::String& text() const { return text_; }
    juce::Justification justification = juce::Justification::centredLeft;
    bool bold = false;
    bool upper = false;
    std::optional<juce::Colour> colour;
    void draw(juce::Graphics& g) override;

private:
    juce::String text_;
    float size_;
    bool dim_;
};

// --- Parameter controls -----------------------------------------------------------------------------

// A control bound to one registry parameter: value, gestures and undo through the session,
// modulation arcs and live value, drop target for source chips, alt-drag depth, the right-click
// menu, typed values with units, keyboard access and accessibility.
class ParamWidget : public Widget, public juce::DragAndDropTarget {
public:
    ParamWidget(Context& ctx, int param, juce::String label);
    ParamWidget(Context& ctx, const char* id, juce::String label);
    ~ParamWidget() override;

    int param() const { return param_; }
    void setParam(int param); // rebinding (effect slots)
    const zed::ParamDef& def() const;
    double value() const;    // plain
    double norm() const;     // 0..1
    juce::String valueText() const;
    // Shown instead of the formatted value when it returns text (a tempo-linked time shows "1/8").
    std::function<juce::String()> valueOverride;
    juce::String label() const { return label_; }
    void setLabel(const juce::String& text) { label_ = text; repaint(); }
    Context& context() { return ctx_; }

    // Changes from the person: a gesture (drag) or one step (click, typed value).
    void beginEdit();
    void editNorm(double n);
    void endEdit();
    void setOneStep(double plain);
    bool typeValue(const juce::String& text, juce::String* error = nullptr);
    void showTypeIn();
    void resetToDefault();
    void stepBy(int steps, bool fine); // keys and wheel

    // Modulation as drawn: one arc per route.
    struct Arc {
        int route = 0;
        juce::Colour colour;
        double low = 0, high = 0; // normalized offsets from the value
        bool muted = false;
        bool selected = false;
    };
    const std::vector<Arc>& arcs() const { return arcs_; }
    double liveNorm() const { return live_; }
    bool modulated() const { return !arcs_.empty(); }

    bool tick() override;
    // When only the live marker moved, only the part that draws it repaints.
    juce::RectangleList<int> dirtyArea() override { return liveOnly_ ? liveArea() : getLocalBounds(); }
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails& details) override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

protected:
    virtual juce::AccessibilityRole role() const { return juce::AccessibilityRole::slider; }
    // How far a vertical drag of one pixel moves the normalized value.
    virtual double dragSpeed() const { return 1.0 / 200.0; }
    virtual bool dragsValue() const { return true; }
    virtual juce::Rectangle<int> liveArea() const { return getLocalBounds(); } // where the live marker is drawn
    virtual void clicked(juce::Point<float> position, bool keyboard) { juce::ignoreUnused(position, keyboard); }
    void drawModulation(juce::Graphics& g, juce::Point<float> centre, float radius, float startAngle, float endAngle) const;
    // The state decorations every control shares: lock, MIDI learn, drop target, highlight, focus.
    void drawDecorations(juce::Graphics& g, juce::Rectangle<float> area) const;
    juce::Colour accent() const;
    bool sourceHighlight() const; // the hovered chip modulates this control

    Context& ctx_;
    int param_ = -1;
    juce::String label_;
    double shown_ = 0.0;
    double live_ = -1.0;
    std::vector<Arc> arcs_;
    int arcsGeneration_ = -1;
    bool dragging_ = false, depthDrag_ = false, dropTarget_ = false, highlighted_ = false, locked_ = false, learning_ = false;
    bool liveOnly_ = false; // the last tick changed nothing but the live marker
    int depthRoute_ = -1;
    double dragStartNorm_ = 0.0, depthStart_ = 0.0;
    juce::Point<float> dragStartPos_;
    std::unique_ptr<juce::TextEditor> typeIn_;
};

class Knob final : public ParamWidget {
public:
    using ParamWidget::ParamWidget;
    void draw(juce::Graphics& g) override;
    bool small = false;
    bool showValue = true;
    bool showLabel = true; // off where a caption beside it names it (the macro strip)
    bool hasVisibleLabel() const override { return showLabel || labelledBy.isNotEmpty(); }

protected:
    juce::Rectangle<float> knobArea() const;
    juce::Rectangle<int> liveArea() const override { return knobArea().getSmallestIntegerContainer(); }
};

// A horizontal bar: route amounts, mixes, anything that reads better as a length.
class Fader final : public ParamWidget {
public:
    using ParamWidget::ParamWidget;
    void draw(juce::Graphics& g) override;
    bool labelLeft = true; // label on the left of the bar, otherwise above it
    int labelWidth = 0;    // 0: as wide as needed

protected:
    double dragSpeed() const override { return 1.0 / juce::jmax(60.0, static_cast<double>(getWidth())); }
};

// A number that can be dragged and typed: octaves, semitones, voices, polyphony, steps.
class NumberBox final : public ParamWidget {
public:
    using ParamWidget::ParamWidget;
    void draw(juce::Graphics& g) override;
    bool labelAbove = true;
    bool costWarning = false; // colours the value as cost rises (unison voices, plan 3.4)

protected:
    double dragSpeed() const override;
};

// An on/off switch with its label.
class Toggle final : public ParamWidget {
public:
    using ParamWidget::ParamWidget;
    void draw(juce::Graphics& g) override;
    bool power = false; // drawn as a power button (module headers); the header shows the name
    bool hasVisibleLabel() const override { return !power; }

protected:
    juce::AccessibilityRole role() const override { return juce::AccessibilityRole::toggleButton; }
    bool dragsValue() const override { return false; }
    void clicked(juce::Point<float> position, bool keyboard) override;
};

// A choice from a list, opened as a menu; the wheel and arrow keys step through it.
class ChoiceBox final : public ParamWidget {
public:
    using ParamWidget::ParamWidget;
    void draw(juce::Graphics& g) override;
    bool labelAbove = true;
    bool hasVisibleLabel() const override { return showLabel || labelledBy.isNotEmpty(); }
    bool showLabel = true;
    // Optional grouping of choices into submenus (filter types, warp modes): group name per choice.
    std::function<juce::String(const juce::String& token)> groupOf;
    void openMenu();

protected:
    juce::AccessibilityRole role() const override { return juce::AccessibilityRole::comboBox; }
    bool dragsValue() const override { return false; }
    void clicked(juce::Point<float> position, bool keyboard) override;
};

// A few choices side by side.
class Segmented final : public ParamWidget {
public:
    using ParamWidget::ParamWidget;
    void draw(juce::Graphics& g) override;
    bool hasVisibleLabel() const override { return showLabel || labelledBy.isNotEmpty(); }
    bool showLabel = true;
    std::vector<juce::String> names; // display names; empty: from the parameter

protected:
    juce::AccessibilityRole role() const override { return juce::AccessibilityRole::comboBox; }
    bool dragsValue() const override { return false; }
    void clicked(juce::Point<float> position, bool keyboard) override;
    juce::Rectangle<float> segmentArea() const;
};

// --- Plain controls -----------------------------------------------------------------------------------

enum class Glyph {
    None, Prev, Next, Undo, Redo, Menu, Close, Plus, Minus, Power, Lock, Search, Keyboard, Map, Speaker, Star, StarFilled,
    Folder, Save, Copy, Paste, Dice, Wand, Blend, ChevronDown, ChevronRight, ChevronUp, Grip, Pencil, Erase, Play, Stop,
    History, Gear, Link, Trash, Help
};
juce::Path glyphPath(Glyph glyph, juce::Rectangle<float> box);

class Button : public Widget {
public:
    Button(juce::String text, Glyph glyph = Glyph::None);
    void draw(juce::Graphics& g) override;
    void setText(const juce::String& text) {
        if (getTitle() == text_ || getTitle().isEmpty())
            setTitle(text); // an accessible name of its own stays
        text_ = text;
        repaint();
    }
    const juce::String& text() const { return text_; }
    void setToggled(bool on) { toggled_ = on; repaint(); }
    bool toggled() const { return toggled_; }
    void setEnabledState(bool on) { setEnabled(on); repaint(); }
    bool hasVisibleLabel() const override { return text_.isNotEmpty(); }
    std::function<void()> onClick;
    std::function<void(const juce::MouseEvent&)> onRightClick;
    bool accentStyle = false;  // filled with the family accent
    bool ghost = false;        // no frame until hovered
    bool toggleRole = false;   // announces itself as a toggle
    bool leftAlign = false;    // text from the left edge (lists)
    float textSize = kTextBody;
    Glyph glyph;

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    bool keyPressed(const juce::KeyPress& key) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    juce::String text_;
    bool toggled_ = false;
};

// Tabs: one focusable button each.
class TabStrip final : public Widget {
public:
    explicit TabStrip(juce::StringArray names);
    void draw(juce::Graphics& g) override;
    void resized() override;
    void setSelected(int index, bool notify = true);
    int selected() const { return selected_; }
    int count() const { return static_cast<int>(tabs_.size()); }
    void setTabVisible(int index, bool visible);
    void setName(int index, const juce::String& name);
    std::function<void(int)> onChange;
    bool large = false; // page tabs
    Button& tab(int index) { return *tabs_[static_cast<std::size_t>(index)]; }

private:
    std::vector<std::unique_ptr<Button>> tabs_;
    int selected_ = 0;
};

// A panel with a header: title, family accent, optional power switch and header controls, and an
// optional collapse arrow.
class Card : public Widget {
public:
    Card(juce::String title, Family family);
    void draw(juce::Graphics& g) override;
    void resized() override;
    juce::Rectangle<int> body() const;  // the area below the header
    juce::Rectangle<int> header() const;
    // Header parts, placed from the right edge leftwards by layoutHeader().
    std::vector<juce::Component*> headerRight;
    Toggle* powerSwitch = nullptr;     // drawn left of the title
    bool collapsible = false;
    bool collapsed = false;
    std::function<void(bool)> onCollapse;
    std::function<void()> onHeaderClick;
    std::function<void(juce::Graphics&, juce::Rectangle<float>)> drawHeaderExtra; // e.g. a mini wave in the header
    juce::Rectangle<float> headerExtraArea; // where drawHeaderExtra paints (set by the owner)
    void setTitleText(const juce::String& t) { title_ = t; repaint(); }
    const juce::String& titleText() const { return title_; }
    juce::String moduleId; // for copy, paste, locks and module presets
    int headerHeight = 28;
    bool selectedLook = false;
    void mouseDown(const juce::MouseEvent& e) override;
    virtual void layoutBody(juce::Rectangle<int>) {}

private:
    juce::String title_;
};

// A modulation source to drag onto any control (plan 3.7). Click selects it; hovering it lights up
// its destinations.
class DragChip final : public Widget {
public:
    DragChip(Context& ctx, juce::String source);
    void draw(juce::Graphics& g) override;
    bool tick() override; // the live value moved: only its bar along the bottom edge repaints
    juce::RectangleList<int> dirtyArea() override { return getLocalBounds().removeFromBottom(5); }
    const juce::String& source() const { return source_; }
    juce::String text;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    std::function<void()> onSelect;

private:
    Context& ctx_;
    juce::String source_;
    float value_ = 0.0f;
    bool dragging_ = false;
};

// Peak meter, two channels side by side, with a slow fall and a held peak.
class Meter final : public Widget {
public:
    explicit Meter(Context& ctx);
    void draw(juce::Graphics& g) override;
    bool tick() override;
    bool vertical = false;
    bool hasVisibleLabel() const override { return false; }

private:
    Context& ctx_;
    float level_[2] = {0, 0}, hold_[2] = {0, 0};
    int holdFrames_[2] = {0, 0};
};

// A one-line text field (search boxes, names, formulas).
class TextField final : public juce::TextEditor {
public:
    explicit TextField(const juce::String& placeholder, const juce::String& accessibleName);
    void paintOverChildren(juce::Graphics& g) override;
    juce::String placeholder;
};

// --- Layout helpers -----------------------------------------------------------------------------------

inline constexpr int kKnobW = 56, kKnobH = 64, kSmallKnobW = 50, kSmallKnobH = 58;
inline constexpr int kChoiceH = 38; // label above, 22 px box
inline constexpr int kRowGap = 6;

// Places components left to right with the given widths and a gap; returns the area used.
juce::Rectangle<int> placeRow(juce::Rectangle<int> area, std::initializer_list<std::pair<juce::Component*, int>> items, int gap = 4);

// Draws a panel background (surfaces share radius and border).
void drawPanel(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill, bool border = true);
// A display well: deep background, inner border, optional grid.
void drawWell(juce::Graphics& g, juce::Rectangle<float> area);
// A luminous line: a soft wide stroke under a thin bright one.
void strokeGlow(juce::Graphics& g, const juce::Path& path, juce::Colour colour, float width = 1.5f);

// Short display names for choice tokens of some parameters (filter types, warp modes ...).
juce::String choiceName(const zed::ParamDef& def, int index);

} // namespace zui

namespace zui {

// Owns the parts of a page or card and adds them to a parent.
class Parts {
public:
    template <class T, class... Args>
    T& add(juce::Component& parent, Args&&... args) {
        auto part = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *part;
        parent.addAndMakeVisible(ref);
        items_.push_back(std::move(part));
        return ref;
    }
    void clear() { items_.clear(); }

private:
    std::vector<std::unique_ptr<juce::Component>> items_;
};

// Lays out components left to right, wrapping to new rows, skipping hidden ones. Each item is a
// component and its width; rows are `rowHeight` tall. Returns the height used.
struct FlowItem {
    juce::Component* c = nullptr;
    int width = kKnobW;
    int height = 0; // 0: the row height
    bool newRow = false;
};
int flow(juce::Rectangle<int> area, const std::vector<FlowItem>& items, int rowHeight, int gap = 4);

// True when the widget should be visible in the current view (advanced parts hide in Simple view).
inline bool shownInView(const Widget& w, bool simple) { return !(simple && w.advanced); }

} // namespace zui

namespace zui {
// True when the component and every parent up to (not including) `root` are visible: "shown in
// the editor" whether or not the editor has a window (offscreen snapshots and tests have none).
bool visibleWithin(const juce::Component& c, const juce::Component* root);
} // namespace zui
