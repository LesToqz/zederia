#include "Widgets.h"

#include <cmath>

namespace zui {

namespace {

constexpr float kStartAngle = -0.75f * juce::MathConstants<float>::pi;
constexpr float kEndAngle = 0.75f * juce::MathConstants<float>::pi;

float angleFor(double norm) { return kStartAngle + static_cast<float>(juce::jlimit(0.0, 1.0, norm)) * (kEndAngle - kStartAngle); }

juce::Path arc(juce::Point<float> c, float radius, float from, float to) {
    juce::Path p;
    p.addCentredArc(c.x, c.y, radius, radius, 0.0f, from, to, true);
    return p;
}

void strokeArc(juce::Graphics& g, juce::Point<float> c, float radius, float from, float to, float width, juce::Colour colour) {
    if (std::abs(to - from) < 0.001f)
        return;
    g.setColour(colour);
    g.strokePath(arc(c, radius, std::min(from, to), std::max(from, to)),
                 juce::PathStrokeType(width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

bool isBipolar(const zed::ParamDef& d) { return d.min < 0.0 && d.max > 0.0 && d.type == zed::ParamType::Float; }

// Value interface for screen readers: the value as shown, with its unit; typed values with units.
class ParamValue final : public juce::AccessibilityValueInterface {
public:
    explicit ParamValue(ParamWidget& w) : w_(w) {}
    bool isReadOnly() const override { return false; }
    double getCurrentValue() const override { return w_.value(); }
    juce::String getCurrentValueAsString() const override { return w_.valueText(); }
    void setValue(double v) override { w_.setOneStep(v); }
    void setValueAsString(const juce::String& s) override { w_.typeValue(s); }
    juce::AccessibilityValueInterface::AccessibleValueRange getRange() const override {
        const auto& d = w_.def();
        if (!(d.max > d.min))
            return {};
        return juce::AccessibilityValueInterface::AccessibleValueRange({d.min, d.max}, d.stepped() ? 1.0 : (d.max - d.min) / 1000.0);
    }

private:
    ParamWidget& w_;
};

} // namespace

// --- Base --------------------------------------------------------------------------------------------

Widget::Widget() { setRepaintsOnMouseActivity(false); }

void Widget::paint(juce::Graphics& g) {
    const juce::Component* previous = AuditRecorder::painting();
    AuditRecorder::painting() = this;
    draw(g);
    AuditRecorder::painting() = previous;
}

void Widget::setHelp(const juce::String& title, const juce::String& text) {
    helpTitle = title;
    helpText = text;
    setDescription(text);
    if (Prefs::get().tooltips && text.isNotEmpty())
        setTooltip(title.isNotEmpty() ? title + ": " + text : text);
}

void Widget::drawFocus(juce::Graphics& g, juce::Rectangle<float> area, float radius) const {
    if (!showsFocus())
        return;
    g.setColour(theme().focus);
    g.drawRoundedRectangle(area.reduced(0.5f), radius, 1.5f);
}

bool Widget::handFocusToEditor() {
    auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>();
    if (editor == nullptr || !editor->getWantsKeyboardFocus() || !editor->isShowing())
        return false;
    editor->grabKeyboardFocus();
    return !hasKeyboardFocus(false);
}

void Widget::focusGained(FocusChangeType cause) {
    if (cause == focusChangedByMouseClick && !keepsClickFocus && handFocusToEditor())
        return;
    focusCue_ = cause != focusChangedByMouseClick;
    repaint();
}

void Widget::clickedWhileFocused() {
    if (!keepsClickFocus && handFocusToEditor())
        return;
    if (focusCue_) {
        focusCue_ = false;
        repaint();
    }
}

bool Widget::keyStateChanged(bool isKeyDown) {
    if (isKeyDown && !focusCue_ && hasKeyboardFocus(false)) {
        static const int editingKeys[] = {juce::KeyPress::leftKey,   juce::KeyPress::rightKey,   juce::KeyPress::upKey,     juce::KeyPress::downKey,
                                              juce::KeyPress::pageUpKey, juce::KeyPress::pageDownKey, juce::KeyPress::homeKey,   juce::KeyPress::endKey,
                                              juce::KeyPress::deleteKey, juce::KeyPress::backspaceKey, juce::KeyPress::returnKey, juce::KeyPress::spaceKey,
                                              juce::KeyPress::tabKey};
        for (const int key : editingKeys)
            if (juce::KeyPress::isKeyCurrentlyDown(key)) {
                focusCue_ = true;
                repaint();
                break;
            }
    }
    return false;
}

Caption::Caption(juce::String text, float size, bool dim) : text_(std::move(text)), size_(size), dim_(dim) {
    setInterceptsMouseClicks(false, false);
    setAccessible(false);
}

void Caption::setText(const juce::String& text) {
    if (text != text_) {
        text_ = text;
        repaint();
    }
}

void Caption::draw(juce::Graphics& g) {
    const Theme& t = theme();
    drawText(g, upper ? text_.toUpperCase() : text_, getLocalBounds().toFloat(), size_, colour.value_or(dim_ ? t.textDim : t.text),
             justification, bold);
}

// --- ParamWidget -------------------------------------------------------------------------------------

ParamWidget::ParamWidget(Context& ctx, int param, juce::String label) : ctx_(ctx), label_(std::move(label)) {
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
    setParam(param);
}

ParamWidget::ParamWidget(Context& ctx, const char* id, juce::String label)
    : ParamWidget(ctx, zed::Registry::get().indexOf(id), std::move(label)) {
    jassert(param_ >= 0); // an id that is not in the registry
}

// Nothing to tell the editor: it holds the hovered control by a safe pointer, and clearing the
// hover here would also clear it when an unrelated control goes (the hover list's own rows).
ParamWidget::~ParamWidget() = default;

void ParamWidget::setParam(int param) {
    param_ = param;
    const auto& reg = zed::Registry::get();
    const auto& own = reg.at(param_);
    const auto& d = def();
    juce::String name = str(own.name);
    if (&d != &own) // an effect slot's generic parameter takes the loaded effect's name
        for (const auto& m : reg.modules())
            if (m.id == own.module)
                name = str(m.name) + " " + str(d.name);
    setTitle(name);
    setHelp(name, str(d.description));
    shown_ = value();
    arcsGeneration_ = -1;
    // The module's power switch, when it has one: its controls read as off while it is off.
    powerParam_ = own.module.empty() ? -1 : reg.indexOf(own.module + ".on");
    if (powerParam_ == param_)
        powerParam_ = -1;
    bypassed_ = powerParam_ >= 0 && ctx_.processor.plainValue(powerParam_) < 0.5;
    repaint();
}

const zed::ParamDef& ParamWidget::def() const { return ctx_.processor.effectiveDef(param_); }
double ParamWidget::value() const { return ctx_.processor.plainValue(param_); }
double ParamWidget::norm() const { return def().toNormalized(value()); }
juce::String ParamWidget::valueText() const {
    if (valueOverride)
        if (const juce::String text = valueOverride(); text.isNotEmpty())
            return text;
    return str(zed::formatValueText(def(), value()));
}

juce::Colour ParamWidget::accent() const { return theme().accent(family); }

juce::Colour ParamWidget::ink() const {
    const Theme& t = theme();
    if (!isEnabled())
        return t.textFaint.withAlpha(0.6f);
    return bypassed_ ? t.lineStrong.brighter(0.25f) : accent();
}

juce::Colour ParamWidget::valueInk() const {
    const Theme& t = theme();
    if (!isEnabled())
        return t.textFaint;
    return bypassed_ ? t.textDim : t.text;
}

bool ParamWidget::sourceHighlight() const {
    if (ctx_.hoveredSource.isEmpty())
        return false;
    for (const auto* r : ctx_.routesTo(param_))
        if (str(r->route.source) == ctx_.hoveredSource)
            return true;
    return false;
}

void ParamWidget::beginEdit() {
    if (!dragging_) {
        dragging_ = true;
        ctx_.session.beginGesture(param_);
        repaint(); // the value shows in the accent while it is held
    }
}

void ParamWidget::editNorm(double n) {
    const auto& d = def();
    ctx_.session.setFromGesture(param_, d.fromNormalized(juce::jlimit(0.0, 1.0, n)));
    shown_ = value();
    repaint();
}

void ParamWidget::endEdit() {
    if (dragging_) {
        dragging_ = false;
        ctx_.session.endGesture(param_);
        repaint(); // a click without a move changes no value, so nothing else would clear the accent
    }
}

void ParamWidget::setOneStep(double plain) {
    ctx_.session.setValue(param_, plain);
    shown_ = value();
    repaint();
}

void ParamWidget::resetToDefault() {
    ctx_.session.resetValue(param_);
    shown_ = value();
    repaint();
}

void ParamWidget::stepBy(int steps, bool fine) {
    const auto& d = def();
    double next;
    if (d.stepped())
        next = d.clamp(value() + steps);
    else
        next = d.fromNormalized(juce::jlimit(0.0, 1.0, norm() + steps * (fine ? 0.002 : 0.02)));
    ctx_.session.nudgeValue(param_, next);
    shown_ = value();
    repaint();
}

bool ParamWidget::typeValue(const juce::String& text, juce::String* error) {
    const auto& d = def();
    zed::ParsedValue parsed;
    std::string problem;
    if (!zed::parseValueText(d, text.toStdString(), ctx_.valueContext(), parsed, problem)) {
        if (error != nullptr)
            *error = str(problem);
        if (ctx_.notify)
            ctx_.notify(str(problem));
        return false;
    }
    const auto& reg = zed::Registry::get();
    const juce::String id = str(reg.at(param_).id);
    // Envelope times typed as note lengths stay linked to the tempo (plan 3.7).
    static const char* const segments[5] = {"delay", "attack", "hold", "decay", "release"};
    for (int env = 1; env <= zed::kNumEnvs; ++env)
        for (int s = 0; s < 5; ++s)
            if (id == "env" + juce::String(env) + "." + segments[s]) {
                const std::string token = parsed.noteLength;
                ctx_.session.edit(str(reg.at(param_).name) + ": " + (token.empty() ? str(zed::formatValueText(d, parsed.plain)) : str(token)),
                                  [&](zed::Patch& p) {
                                      p.envSync[static_cast<std::size_t>(env - 1)][static_cast<std::size_t>(s)] =
                                          token.size() < 8 && zed::divisionInBeats(token) > 0.0 ? token : std::string();
                                      if (!token.empty() && zed::divisionInBeats(token) <= 0.0) // "3/16": not a listed division
                                          p.envSync[static_cast<std::size_t>(env - 1)][static_cast<std::size_t>(s)] = token;
                                      p.set(param_, parsed.plain);
                                      return true;
                                  });
                return true;
            }
    // A rate typed as a note length switches the LFO or Flux to its synced rate.
    if (!parsed.noteLength.empty() && (id.startsWith("lfo") || id.startsWith("flux")) && id.endsWith(".rate")) {
        const juce::String module = id.upToFirstOccurrenceOf(".", false, false);
        const auto* division = reg.find((module + ".division").toStdString());
        const int choice = division != nullptr ? division->choiceIndex(parsed.noteLength) : -1;
        if (choice >= 0) {
            ctx_.session.edit(str(reg.at(param_).name) + ": " + str(parsed.noteLength), [&](zed::Patch& p) {
                p.set((module + ".sync").toStdString(), 1.0);
                p.set((module + ".division").toStdString(), choice);
                return true;
            });
            return true;
        }
    }
    setOneStep(parsed.plain);
    return true;
}

void ParamWidget::showTypeIn() {
    typeIn_ = std::make_unique<juce::TextEditor>("value");
    auto& editor = *typeIn_;
    editor.setTitle("Type a value for " + getTitle());
    editor.setFont(font(kTextBody));
    editor.setJustification(juce::Justification::centred);
    editor.setText(valueText(), false);
    editor.selectAll();
    const int w = juce::jmax(getWidth(), 76);
    editor.setBounds((getWidth() - w) / 2, juce::jmax(0, getHeight() / 2 - 11), w, 22);
    addAndMakeVisible(editor);
    editor.grabKeyboardFocus();
    const auto close = [this](bool accept) {
        if (typeIn_ == nullptr)
            return;
        const juce::String text = typeIn_->getText();
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<ParamWidget>(this)] {
            if (safe != nullptr)
                safe->typeIn_.reset();
        });
        if (accept)
            typeValue(text);
        grabKeyboardFocus();
    };
    editor.onReturnKey = [close] { close(true); };
    editor.onEscapeKey = [close] { close(false); };
    editor.onFocusLost = [close] { close(false); };
}

bool ParamWidget::tick() {
    bool dirty = false;
    const double v = value();
    if (v != shown_) {
        shown_ = v;
        dirty = true;
    }
    const auto& d = def();
    if (arcsGeneration_ != ctx_.routeGeneration()) {
        arcsGeneration_ = ctx_.routeGeneration();
        arcs_.clear();
        for (const auto* r : ctx_.routesTo(param_)) {
            Arc a;
            a.route = r->index;
            a.colour = r->colour;
            arcs_.push_back(a);
        }
        dirty = true;
    }
    for (auto& a : arcs_) {
        const auto* r = ctx_.route(a.route);
        if (r == nullptr)
            continue;
        const double amount = ctx_.routeAmount(a.route);
        double low = 0.0, high = amount;
        if (r->route.polarity == zed::Polarity::Bi) {
            low = -std::abs(amount);
            high = std::abs(amount);
        } else if (amount < 0.0) {
            low = amount;
            high = 0.0;
        }
        const bool muted = !ctx_.routeAudible(*r);
        const bool selected = str(r->route.source) == ctx_.selectedSource || r->index == ctx_.focusRoute;
        if (low != a.low || high != a.high || muted != a.muted || selected != a.selected) {
            a.low = low;
            a.high = high;
            a.muted = muted;
            a.selected = selected;
            dirty = true;
        }
    }
    bool liveMoved = false;
    if (!arcs_.empty()) {
        const double liveValue = ctx_.processor.engine().liveValue(param_);
        const double live = d.toNormalized(d.clamp(liveValue));
        if (std::abs(live - live_) > 0.0015) {
            liveBefore_ = live_;
            live_ = live;
            liveMoved = true;
        }
    } else if (live_ >= 0.0) {
        live_ = -1.0;
        dirty = true;
    }
    const bool locked = ctx_.isLocked(param_);
    const bool learning = ctx_.processor.midiLearnTarget() == param_;
    const bool highlighted = sourceHighlight();
    const bool bypassed = powerParam_ >= 0 && ctx_.processor.plainValue(powerParam_) < 0.5;
    if (locked != locked_ || learning != learning_ || highlighted != highlighted_ || bypassed != bypassed_) {
        locked_ = locked;
        learning_ = learning;
        highlighted_ = highlighted;
        bypassed_ = bypassed;
        dirty = true;
    }
    liveOnly_ = liveMoved && !dirty;
    return dirty || liveMoved;
}

bool ParamWidget::keyPressed(const juce::KeyPress& key) {
    const bool fine = key.getModifiers().isShiftDown();
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey) { stepBy(1, fine); return true; }
    if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey) { stepBy(-1, fine); return true; }
    if (code == juce::KeyPress::pageUpKey) { stepBy(def().stepped() ? 1 : 10, false); return true; }
    if (code == juce::KeyPress::pageDownKey) { stepBy(def().stepped() ? -1 : -10, false); return true; }
    if (code == juce::KeyPress::homeKey) { setOneStep(def().min); return true; }
    if (code == juce::KeyPress::endKey) { setOneStep(def().max); return true; }
    if (code == juce::KeyPress::returnKey) {
        if (role() == juce::AccessibilityRole::slider)
            showTypeIn();
        else
            clicked({}, true);
        return true;
    }
    if (code == juce::KeyPress::spaceKey && role() != juce::AccessibilityRole::slider) {
        clicked({}, true);
        return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { resetToDefault(); return true; }
    if ((code == juce::KeyPress::F10Key && key.getModifiers().isShiftDown()) || key == juce::KeyPress('m', juce::ModifierKeys::commandModifier, 0)) {
        if (ctx_.openParamMenu)
            ctx_.openParamMenu(*this);
        return true;
    }
    return false;
}

void ParamWidget::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {
        if (ctx_.openParamMenu)
            ctx_.openParamMenu(*this);
        return;
    }
    if (e.mods.isAltDown() && def().modulatable) {
        // The depth of the selected source's route (made if it does not exist yet), else of the
        // highlighted or first route.
        int route = -1;
        ctx_.refresh(); // the routes as they are now, not as last drawn
        const auto mine = ctx_.routesTo(param_);
        if (ctx_.selectedSource.isNotEmpty()) {
            for (const auto* r : mine)
                if (str(r->route.source) == ctx_.selectedSource)
                    route = r->index;
            if (route < 0 && !e.mods.isShiftDown())
                route = ctx_.session.addRoute(ctx_.selectedSource, param_, 0.0);
        }
        if (route < 0 && !mine.empty()) {
            route = mine.front()->index;
            for (const auto* r : mine)
                if (r->index == ctx_.focusRoute)
                    route = r->index;
        }
        if (route < 0)
            return;
        if (e.mods.isShiftDown()) { // Shift+Alt-click flips the polarity
            ctx_.session.flipPolarity(route);
            return;
        }
        depthDrag_ = true;
        depthRoute_ = route;
        ctx_.focusRoute = route;
        depthStart_ = ctx_.routeAmount(route);
        dragStartPos_ = e.position;
        ctx_.session.beginGesture(zed::Registry::get().modAmount(route));
        return;
    }
    if (e.mods.isCommandDown()) {
        resetToDefault();
        return;
    }
    if (dragsValue()) {
        beginEdit();
        dragStartNorm_ = norm();
        dragStartPos_ = e.position;
    } else {
        clicked(e.position, false);
    }
}

void ParamWidget::mouseDrag(const juce::MouseEvent& e) {
    const bool fine = e.mods.isShiftDown();
    if (depthDrag_) {
        const double delta = (dragStartPos_.y - e.position.y) / 150.0 * (fine ? 0.1 : 1.0);
        const int amountParam = zed::Registry::get().modAmount(depthRoute_);
        ctx_.session.setFromGesture(amountParam, juce::jlimit(-1.0, 1.0, depthStart_ + delta));
        tick();
        repaint();
        return;
    }
    if (!dragging_)
        return;
    double n;
    if (auto* knob = dynamic_cast<Knob*>(this); ctx_.prefs.rotaryDrag && knob != nullptr) {
        const auto c = knob->knobCentre();
        float angle = std::atan2(e.position.x - c.x, c.y - e.position.y);
        angle = juce::jlimit(kStartAngle, kEndAngle, angle);
        n = (angle - kStartAngle) / (kEndAngle - kStartAngle);
    } else {
        const double dy = dragStartPos_.y - e.position.y;
        const double dx = dynamic_cast<Fader*>(this) != nullptr ? e.position.x - dragStartPos_.x : 0.0;
        n = dragStartNorm_ + (dy + dx) * dragSpeed() * (fine ? 0.1 : 1.0);
        if (fine) { // keep the fine drag continuous from where it is now
            dragStartNorm_ = n;
            dragStartPos_ = e.position;
        }
    }
    editNorm(n);
}

void ParamWidget::mouseUp(const juce::MouseEvent&) {
    if (depthDrag_) {
        depthDrag_ = false;
        ctx_.session.endGesture(zed::Registry::get().modAmount(depthRoute_));
        repaint();
        return;
    }
    endEdit();
}

void ParamWidget::mouseDoubleClick(const juce::MouseEvent& e) {
    if (e.mods.isAltDown() || e.mods.isPopupMenu() || !dragsValue())
        return;
    if (ctx_.prefs.doubleClickTypes)
        showTypeIn();
    else
        resetToDefault();
}

void ParamWidget::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) {
    if (ctx_.prefs.wheel == 0 || w.deltaY == 0.0f) {
        Component::mouseWheelMove(e, w);
        return;
    }
    stepBy(w.deltaY > 0 ? 1 : -1, ctx_.prefs.wheel == 2 || e.mods.isShiftDown());
}

// Widgets do not repaint on mouse activity by themselves (it would repaint every control the
// pointer crosses), but a control draws its hover ring and its held value: those repaint here.
void ParamWidget::mouseEnter(const juce::MouseEvent&) {
    repaint();
    if (ctx_.hoverParam)
        ctx_.hoverParam(this);
}

void ParamWidget::mouseExit(const juce::MouseEvent&) {
    repaint();
    if (ctx_.hoverParam)
        ctx_.hoverParam(nullptr);
}

bool ParamWidget::isInterestedInDragSource(const SourceDetails& details) {
    return details.description.toString().startsWith("mod:") && zed::Registry::get().at(param_).modulatable;
}

void ParamWidget::itemDragEnter(const SourceDetails&) {
    dropTarget_ = true;
    repaint();
}

void ParamWidget::itemDragExit(const SourceDetails&) {
    dropTarget_ = false;
    repaint();
}

void ParamWidget::itemDropped(const SourceDetails& details) {
    dropTarget_ = false;
    const juce::String source = details.description.toString().fromFirstOccurrenceOf("mod:", false, false);
    const int route = ctx_.session.addRoute(source, param_, 0.25);
    if (route >= 0) {
        ctx_.selectedSource = source;
        ctx_.focusRoute = route;
    } else if (ctx_.notify) {
        ctx_.notify("All 64 routes are in use; remove one on the MOD page first.");
    }
    repaint();
}

std::unique_ptr<juce::AccessibilityHandler> ParamWidget::createAccessibilityHandler() {
    juce::AccessibilityActions actions;
    actions.addAction(juce::AccessibilityActionType::showMenu, [this] {
        if (ctx_.openParamMenu)
            ctx_.openParamMenu(*this);
    });
    if (role() != juce::AccessibilityRole::slider)
        actions.addAction(juce::AccessibilityActionType::press, [this] {
            clicked({}, true);
        });
    return std::make_unique<juce::AccessibilityHandler>(*this, role(), std::move(actions),
                                                        juce::AccessibilityHandler::Interfaces{std::make_unique<ParamValue>(*this)});
}

void ParamWidget::drawModulation(juce::Graphics& g, juce::Point<float> centre, float radius, float, float, bool live) const {
    const double base = norm();
    int ring = 0;
    for (const auto& a : arcs_) {
        const float r = radius + 3.5f + 2.6f * static_cast<float>(std::min(ring, 2));
        const float from = angleFor(base + a.low), to = angleFor(base + a.high);
        juce::Colour c = a.colour.withAlpha(a.muted ? 0.35f : a.selected ? 1.0f : 0.75f);
        if (a.muted) { // muted routes are dashed: a cue that does not rely on colour
            juce::Path p = arc(centre, r, std::min(from, to), std::max(from, to));
            juce::Path dashed;
            const float dashes[] = {2.0f, 2.0f};
            juce::PathStrokeType(1.5f).createDashedStroke(dashed, p, dashes, 2);
            g.setColour(c);
            g.fillPath(dashed);
        } else {
            strokeArc(g, centre, r, from, to, a.selected ? 2.4f : 1.6f, c);
        }
        ++ring;
    }
    if (live)
        drawLiveMarker(g, centre, radius);
}

void ParamWidget::drawLiveMarker(juce::Graphics& g, juce::Point<float> centre, float radius) const {
    if (live_ >= 0.0) { // the live value: a bright dot on the track
        const float a = angleFor(live_);
        const juce::Point<float> p = centre.getPointOnCircumference(radius, a);
        g.setColour(theme().displayLine.withAlpha(0.35f));
        g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre(p));
        g.setColour(theme().displayLine);
        g.fillEllipse(juce::Rectangle<float>(3.2f, 3.2f).withCentre(p));
    }
}

void ParamWidget::drawDecorations(juce::Graphics& g, juce::Rectangle<float> area) const {
    const Theme& t = theme();
    if (highlighted_) {
        g.setColour(ctx_.sourceColour(ctx_.hoveredSource).withAlpha(0.18f));
        g.fillRoundedRectangle(area, 4.0f);
        g.setColour(ctx_.sourceColour(ctx_.hoveredSource));
        g.drawRoundedRectangle(area.reduced(0.5f), 4.0f, 1.2f);
    }
    if (dropTarget_) {
        g.setColour(t.focus.withAlpha(0.2f));
        g.fillRoundedRectangle(area, 4.0f);
        g.setColour(t.focus);
        g.drawRoundedRectangle(area.reduced(0.5f), 4.0f, 1.5f);
    }
    if (learning_) {
        juce::Path outline;
        outline.addRoundedRectangle(area.reduced(1.0f), 4.0f);
        juce::Path dashed;
        const float dashes[] = {3.0f, 2.0f};
        juce::PathStrokeType(1.4f).createDashedStroke(dashed, outline, dashes, 2);
        g.setColour(t.warning);
        g.fillPath(dashed);
    }
    if (locked_) {
        const auto badge = juce::Rectangle<float>(9.0f, 9.0f).withPosition(area.getRight() - 10.0f, area.getY() + 1.0f);
        g.setColour(t.warning);
        g.fillPath(glyphPath(Glyph::Lock, badge));
    }
    drawFocus(g, area);
}

// --- Knob --------------------------------------------------------------------------------------------

juce::Rectangle<float> Knob::knobArea() const {
    return getLocalBounds().toFloat().withTrimmedBottom((showValue ? valueRow() : 0.0f) + (showLabel ? labelRow : 0.0f));
}

float Knob::radius() const {
    const auto area = knobArea();
    return std::max(5.0f, std::min(area.getWidth(), area.getHeight()) * 0.5f - (trackInset >= 0.0f ? trackInset : small ? 5.0f : 7.0f));
}

juce::RectangleList<int> Knob::dirtyArea() {
    if (!liveOnly_ || liveBefore_ < 0.0)
        return ParamWidget::dirtyArea();
    // The marker is a dot on the track: repaint where it was and where it is.
    const auto area = knobArea();
    const float r = radius();
    const auto dot = [&](double n) {
        return juce::Rectangle<float>(9.0f, 9.0f).withCentre(area.getCentre().getPointOnCircumference(r, angleFor(n))).getSmallestIntegerContainer();
    };
    juce::RectangleList<int> list(dot(liveBefore_));
    list.add(dot(live_));
    return list;
}

void Knob::draw(juce::Graphics& g) {
    if (arcs_.empty()) {
        drawFace(g);
        return;
    }
    // A modulated knob repaints every frame for its live marker: its face is kept as an image and
    // only the marker is drawn over it.
    std::uint64_t key = static_cast<std::uint64_t>(label_.hashCode64()) ^ (static_cast<std::uint64_t>(valueText().hashCode64()) << 1);
    const auto add = [&key](double v) { key = (key ^ std::hash<double>{}(v)) * 1099511628211ull; };
    add(norm());
    for (const bool b : {isEnabled(), isMouseOverOrDragging(), dragging_, depthDrag_, highlighted_, dropTarget_, learning_, locked_, hasKeyboardFocus(true), bypassed_})
        add(b ? 1.0 : 0.0);
    for (const auto& a : arcs_) {
        add(a.low);
        add(a.high);
        add((a.muted ? 1.0 : 0.0) + (a.selected ? 2.0 : 0.0));
        add(static_cast<double>(a.colour.getARGB()));
    }
    face_.draw(g, getLocalBounds(), key, false, [this](juce::Graphics& fg) { drawFace(fg); });
    drawLiveMarker(g, knobArea().getCentre(), radius());
}

void Knob::drawFace(juce::Graphics& g) {
    const Theme& t = theme();
    const auto& d = def();
    const auto area = knobArea();
    const float radius = this->radius();
    const auto c = area.getCentre();
    const double n = norm();
    const bool active = isEnabled() && !bypassed_;
    // Track and value arc: the value in the control ink, graphite while the module is off.
    strokeArc(g, c, radius, kStartAngle, kEndAngle, 2.4f, t.metal > 0.0f && t.dark ? t.sunken : t.line);
    const float zero = angleFor(isBipolar(d) ? d.toNormalized(0.0) : 0.0);
    strokeArc(g, c, radius, zero, angleFor(n), 2.6f, ink());
    if (isBipolar(d)) { // the centre of a bipolar range, marked on the scale
        g.setColour(t.textDim.withAlpha(0.8f));
        g.drawLine(juce::Line<float>(c.getPointOnCircumference(radius + 2.0f, zero), c.getPointOnCircumference(radius + 4.0f, zero)), 1.0f);
    }
    // Body and pointer.
    const float body = radius - 4.5f;
    const auto bodyBox = juce::Rectangle<float>(body * 2.0f, body * 2.0f).withCentre(c);
    const float a = angleFor(n);
    if (t.metal > 0.0f) {
        // A machined cap: shadow, knurled skirt, brushed top, all lit from straight above like the
        // panels (drawn once, kept as an image: it does not turn), then the pointer line.
        const bool hot = isMouseOverOrDragging();
        cap_.draw(g, bodyBox.expanded(2.0f, 3.0f).getSmallestIntegerContainer(), hot ? 1u : 0u, false, [&](juce::Graphics& cg) {
            cg.setColour(juce::Colours::black.withAlpha(t.dark ? 0.5f : 0.22f));
            cg.fillEllipse(bodyBox.expanded(1.0f).translated(0.0f, 1.6f));
            const juce::Colour skirt = t.dark ? t.raised.darker(0.25f) : t.raised.darker(0.12f);
            cg.setGradientFill(juce::ColourGradient(skirt.brighter(0.45f * t.metal), c.x, bodyBox.getY(), skirt.darker(0.35f * t.metal), c.x,
                                                    bodyBox.getBottom(), false));
            cg.fillEllipse(bodyBox);
            if (body > 9.0f) { // the knurling
                cg.setColour(juce::Colours::black.withAlpha(0.28f));
                constexpr int kTeeth = 28;
                for (int i = 0; i < kTeeth; ++i) {
                    const float ta = juce::MathConstants<float>::twoPi * static_cast<float>(i) / kTeeth;
                    cg.drawLine(juce::Line<float>(c.getPointOnCircumference(body - 2.2f, ta), c.getPointOnCircumference(body - 0.5f, ta)), 0.8f);
                }
            }
            const auto cap = bodyBox.reduced(body > 9.0f ? 2.6f : 1.5f);
            const juce::Colour top = t.dark ? t.raised.brighter(0.18f) : t.panel.brighter(0.05f);
            cg.setGradientFill(juce::ColourGradient(top.brighter(0.35f * t.metal), cap.getCentreX(), cap.getY(),
                                                    top.darker(0.25f * t.metal), cap.getCentreX(), cap.getBottom() + cap.getHeight() * 0.3f, true));
            cg.fillEllipse(cap);
            cg.setColour(hot ? t.control.withAlpha(0.55f) : juce::Colours::black.withAlpha(t.dark ? 0.6f : 0.3f));
            cg.drawEllipse(bodyBox, 1.0f);
            // The light catches the upper rim only.
            juce::Path rim;
            rim.addCentredArc(c.x, c.y, cap.getWidth() * 0.5f - 0.5f, cap.getHeight() * 0.5f - 0.5f, 0.0f, -1.2f, 1.2f, true);
            cg.setColour(juce::Colours::white.withAlpha(t.dark ? 0.16f : 0.5f));
            cg.strokePath(rim, juce::PathStrokeType(0.8f));
        });
        g.setColour(active ? (t.dark ? juce::Colours::white.withAlpha(0.95f) : t.text) : t.textDim);
        g.drawLine(juce::Line<float>(c.getPointOnCircumference(body * 0.35f, a), c.getPointOnCircumference(body - 1.5f, a)), 2.0f);
    } else {
        g.setColour(t.raised);
        g.fillEllipse(bodyBox);
        g.setColour(isMouseOverOrDragging() ? t.lineStrong.brighter(0.25f) : t.lineStrong);
        g.drawEllipse(bodyBox, 1.0f);
        g.setColour(active ? t.text : t.textDim);
        g.drawLine(juce::Line<float>(c.getPointOnCircumference(body * 0.25f, a), c.getPointOnCircumference(body - 1.5f, a)), 2.0f);
    }
    drawModulation(g, c, radius, kStartAngle, kEndAngle, arcs_.empty());
    // The name as a small legend, the value a step larger under it; a held value is bold.
    const auto bounds = getLocalBounds().toFloat();
    auto text = bounds.withTop(area.getBottom());
    if (showLabel)
        drawText(g, label_, text.removeFromTop(labelRow), kTextSmall, t.textDim, juce::Justification::centred);
    if (showValue) {
        const juce::String v = valueText();
        const auto box = text.removeFromTop(valueRow());
        drawText(g, v, box, fittingSize(v, box.getWidth(), valueSize, dragging_ || depthDrag_), valueInk(), juce::Justification::centred, dragging_ || depthDrag_);
    }
    drawDecorations(g, bounds);
}

// --- Fader -------------------------------------------------------------------------------------------

void Fader::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto& d = def();
    auto area = getLocalBounds().toFloat();
    const float labelW = labelLeft ? (labelWidth > 0 ? static_cast<float>(labelWidth) : std::min(textWidth(label_, kTextSmall) + 6.0f, area.getWidth() * 0.4f)) : 0.0f;
    juce::Rectangle<float> labelBox, valueBox, bar;
    if (labelLeft) {
        labelBox = area.removeFromLeft(labelW);
        valueBox = area.removeFromRight(std::min(56.0f, area.getWidth() * 0.4f));
        bar = area.reduced(2.0f, 0.0f).withSizeKeepingCentre(area.getWidth() - 4.0f, 6.0f);
    } else {
        labelBox = area.removeFromTop(13.0f);
        valueBox = labelBox.removeFromRight(56.0f);
        bar = area.withSizeKeepingCentre(area.getWidth(), 6.0f);
    }
    if (label_.isNotEmpty())
        drawText(g, label_, labelBox, kTextSmall, t.textDim);
    // A recessed slot with a quiet fill up to the value, and a thumb to take hold of (an editable
    // track never reads as a meter); a bipolar range has its centre marked.
    const float thumbW = 7.0f;
    const auto slot = bar.reduced(thumbW * 0.5f, 0.0f);
    g.setColour(t.sunken);
    g.fillRoundedRectangle(slot, 2.0f);
    g.setColour(t.line);
    g.drawRoundedRectangle(slot, 2.0f, 1.0f);
    const double n = norm();
    const bool bipolar = isBipolar(d);
    const double zero = bipolar ? d.toNormalized(0.0) : 0.0;
    const auto xAt = [&](double v) { return slot.getX() + slot.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, v)); };
    const float x0 = xAt(std::min(zero, n)), x1 = xAt(std::max(zero, n));
    g.setColour(ink().withAlpha(0.38f));
    g.fillRect(juce::Rectangle<float>(x0, slot.getY() + 1.0f, std::max(0.0f, x1 - x0), slot.getHeight() - 2.0f));
    if (bipolar) {
        g.setColour(t.textDim);
        g.fillRect(juce::Rectangle<float>(xAt(zero) - 0.5f, bar.getY() - 3.0f, 1.0f, bar.getHeight() + 6.0f));
    }
    const auto thumb = juce::Rectangle<float>(thumbW, std::min(14.0f, area.getHeight() - 2.0f)).withCentre({xAt(n), bar.getCentreY()});
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.fillRoundedRectangle(thumb.translated(0.0f, 1.0f), 2.0f);
    g.setColour(ink());
    g.fillRoundedRectangle(thumb, 2.0f);
    g.setColour(t.bg.withAlpha(0.7f));
    g.fillRect(thumb.withSizeKeepingCentre(1.0f, thumb.getHeight() - 6.0f)); // the grip line
    // Modulation: thin bars under the track, one per route.
    float y = bar.getBottom() + 1.5f;
    for (const auto& a : arcs_) {
        const float m0 = bar.getX() + bar.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, n + a.low));
        const float m1 = bar.getX() + bar.getWidth() * static_cast<float>(juce::jlimit(0.0, 1.0, n + a.high));
        g.setColour(a.colour.withAlpha(a.muted ? 0.35f : a.selected ? 1.0f : 0.75f));
        g.fillRect(juce::Rectangle<float>(m0, y, std::max(1.0f, m1 - m0), 1.6f));
        y += 2.2f;
        if (y > static_cast<float>(getHeight()) - 1.0f)
            break;
    }
    if (live_ >= 0.0) {
        const float x = xAt(live_);
        g.setColour(t.displayLine);
        g.fillRect(juce::Rectangle<float>(x - 1.0f, bar.getY() - 2.0f, 2.0f, bar.getHeight() + 4.0f));
    }
    const juce::String v = valueText();
    drawText(g, v, valueBox, fittingSize(v, valueBox.getWidth(), kValueText, dragging_), valueInk(), juce::Justification::centredRight, dragging_);
    drawDecorations(g, getLocalBounds().toFloat());
}

// --- NumberBox ---------------------------------------------------------------------------------------

double NumberBox::dragSpeed() const {
    const auto& d = def();
    return d.stepped() ? 1.0 / std::max(4.0, (d.max - d.min) * 8.0) * 1.0 : 1.0 / 300.0;
}

void NumberBox::draw(juce::Graphics& g) {
    const Theme& t = theme();
    auto area = getLocalBounds().toFloat();
    if (labelAbove)
        drawText(g, label_, area.removeFromTop(13.0f), kTextSmall, t.textDim, juce::Justification::centred);
    else
        drawText(g, label_, area.removeFromLeft(std::min(textWidth(label_, kTextSmall) + 6.0f, area.getWidth() * 0.55f)), kTextSmall,
                 t.textDim);
    const auto box = area.reduced(1.0f, 1.0f).withSizeKeepingCentre(area.getWidth() - 2.0f, std::min(area.getHeight() - 2.0f, 22.0f));
    g.setColour(t.sunken);
    g.fillRoundedRectangle(box, 4.0f);
    g.setColour(isMouseOverOrDragging() || dragging_ ? t.control.withAlpha(0.6f) : t.line);
    g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
    juce::Colour valueColour = valueInk();
    if (costWarning) { // more voices cost more: amber from 8, red from 12
        const double v = value();
        valueColour = v >= 12.0 ? t.danger : v >= 8.0 ? t.warning : valueColour;
    }
    drawText(g, valueText(), box.reduced(4.0f, 0.0f), kTextBody, valueColour, juce::Justification::centred);
    if (!arcs_.empty()) {
        g.setColour(arcs_.front().colour);
        g.fillRect(box.withHeight(2.0f).withY(box.getBottom() - 2.0f).reduced(3.0f, 0.0f));
    }
    drawDecorations(g, getLocalBounds().toFloat());
}

// --- Toggle ------------------------------------------------------------------------------------------

void Toggle::clicked(juce::Point<float>, bool) { setOneStep(value() >= 0.5 ? 0.0 : 1.0); }

void Toggle::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const bool on = value() >= 0.5;
    auto area = getLocalBounds().toFloat();
    if (power) {
        // On: a lit lamp with a filled glyph. Off: a hollow ring with the glyph in graphite, so the
        // state reads without colour (the module's header also says OFF).
        const auto circle = area.withSizeKeepingCentre(std::min(area.getWidth(), area.getHeight()) - 2.0f,
                                                       std::min(area.getWidth(), area.getHeight()) - 2.0f);
        if (on) {
            if (t.metal > 0.0f) { // a lit lamp glows
                g.setColour(t.lamp.withAlpha(0.28f));
                g.fillEllipse(circle.expanded(1.0f));
            }
            g.setColour(t.lamp);
            g.fillEllipse(circle);
            g.setColour(t.bg);
            g.strokePath(glyphPath(Glyph::Power, circle.reduced(circle.getWidth() * 0.22f)), juce::PathStrokeType(1.6f));
        } else {
            g.setColour(t.sunken);
            g.fillEllipse(circle);
            g.setColour(t.textFaint);
            g.drawEllipse(circle.reduced(0.75f), 1.2f);
            g.strokePath(glyphPath(Glyph::Power, circle.reduced(circle.getWidth() * 0.24f)), juce::PathStrokeType(1.2f));
        }
        drawDecorations(g, area);
        return;
    }
    const auto pill = juce::Rectangle<float>(26.0f, 14.0f).withPosition(area.getX() + 1.0f, area.getCentreY() - 7.0f);
    g.setColour(on ? ink() : t.sunken);
    g.fillRoundedRectangle(pill, 7.0f);
    g.setColour(on ? ink() : t.lineStrong);
    g.drawRoundedRectangle(pill.reduced(0.5f), 7.0f, 1.0f);
    const auto knob = juce::Rectangle<float>(10.0f, 10.0f).withCentre({on ? pill.getRight() - 7.0f : pill.getX() + 7.0f, pill.getCentreY()});
    g.setColour(on ? t.bg : t.textDim);
    g.fillEllipse(knob);
    drawText(g, label_, area.withTrimmedLeft(32.0f), kTextSmall, on && !bypassed_ ? t.text : t.textDim);
    drawDecorations(g, area);
}

// --- ChoiceBox ---------------------------------------------------------------------------------------

juce::String choiceName(const zed::ParamDef& d, int index) {
    if (index < 0 || index >= static_cast<int>(d.choices.size()))
        return "?";
    return str(zed::choiceDisplayName(d.choices[static_cast<std::size_t>(index)]));
}

void ChoiceBox::clicked(juce::Point<float>, bool) { openMenu(); }

void ChoiceBox::openMenu() {
    const auto& d = def();
    const int current = static_cast<int>(std::lround(value() - d.min));
    juce::PopupMenu menu;
    if (groupOf) {
        std::vector<std::pair<juce::String, juce::PopupMenu>> groups;
        for (int i = 0; i < static_cast<int>(d.choices.size()); ++i) {
            const juce::String group = groupOf(str(d.choices[static_cast<std::size_t>(i)]));
            auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& p) { return p.first == group; });
            if (it == groups.end()) {
                groups.emplace_back(group, juce::PopupMenu());
                it = std::prev(groups.end());
            }
            it->second.addItem(i + 1, choiceName(d, i), true, i == current);
        }
        for (auto& [name, sub] : groups)
            menu.addSubMenu(name, sub);
    } else {
        for (int i = 0; i < static_cast<int>(d.choices.size()); ++i)
            menu.addItem(i + 1, choiceName(d, i), true, i == current);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMinimumWidth(getWidth()),
                       [safe = juce::Component::SafePointer<ChoiceBox>(this)](int result) {
                           if (safe != nullptr && result > 0)
                               safe->setOneStep(safe->def().min + result - 1);
                       });
}

void ChoiceBox::draw(juce::Graphics& g) {
    const Theme& t = theme();
    auto area = getLocalBounds().toFloat();
    if (showLabel) {
        if (labelAbove)
            drawText(g, label_, area.removeFromTop(13.0f), kTextSmall, t.textDim);
        else
            drawText(g, label_, area.removeFromLeft(std::min(textWidth(label_, kTextSmall) + 6.0f, area.getWidth() * 0.45f)), kTextSmall, t.textDim);
    }
    const auto box = area.withSizeKeepingCentre(area.getWidth(), std::min(area.getHeight(), 22.0f));
    g.setColour(t.raised);
    g.fillRoundedRectangle(box, 4.0f);
    g.setColour(isMouseOver() ? t.lineStrong.brighter(0.2f) : t.line);
    g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
    const auto& d = def();
    const int index = static_cast<int>(std::lround(value() - d.min));
    auto inner = box.reduced(6.0f, 0.0f);
    const auto arrow = inner.removeFromRight(10.0f);
    g.setColour(t.textDim);
    g.fillPath(glyphPath(Glyph::ChevronDown, arrow.withSizeKeepingCentre(9.0f, 9.0f)));
    const juce::String text = choiceName(d, index);
    drawText(g, text, inner, fittingSize(text, inner.getWidth(), kTextBody), valueInk());
    drawDecorations(g, getLocalBounds().toFloat());
}

// --- Segmented ---------------------------------------------------------------------------------------

juce::Rectangle<float> Segmented::segmentArea() const {
    auto area = getLocalBounds().toFloat();
    if (showLabel)
        area.removeFromTop(13.0f);
    return area.withSizeKeepingCentre(area.getWidth(), std::min(area.getHeight(), 22.0f));
}

void Segmented::clicked(juce::Point<float> position, bool keyboard) {
    const auto& d = def();
    const int count = static_cast<int>(d.max - d.min) + 1;
    const auto area = segmentArea();
    if (keyboard || position.x < area.getX() || position.x > area.getRight() || position.y < area.getY()) {
        setOneStep(d.min + std::fmod(value() - d.min + 1.0, count)); // keyboard: the next segment
        return;
    }
    const int index = juce::jlimit(0, count - 1, static_cast<int>((position.x - area.getX()) / area.getWidth() * count));
    setOneStep(d.min + index);
}

void Segmented::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto& d = def();
    if (showLabel)
        drawText(g, label_, getLocalBounds().toFloat().removeFromTop(13.0f), kTextSmall, t.textDim);
    const auto area = segmentArea();
    const int count = static_cast<int>(d.max - d.min) + 1;
    const int current = static_cast<int>(std::lround(value() - d.min));
    g.setColour(t.sunken);
    g.fillRoundedRectangle(area, 4.0f);
    const float w = area.getWidth() / static_cast<float>(count);
    // The chosen segment is a raised key in the control ink with dark bold text, the others recessed
    // with dim text: the choice reads by brightness alone. In a module that is off it is graphite.
    const bool active = isEnabled() && !bypassed_;
    for (int i = 0; i < count; ++i) {
        const auto seg = juce::Rectangle<float>(area.getX() + w * static_cast<float>(i), area.getY(), w, area.getHeight());
        if (i == current) {
            g.setColour(active ? t.control : t.lineStrong);
            g.fillRoundedRectangle(seg.reduced(1.5f), 3.0f);
        }
        const juce::String name = i < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(i)] : choiceName(d, i);
        drawText(g, name, seg.reduced(3.0f, 0.0f), fittingSize(name, seg.getWidth() - 6.0f, kTextSmall + 0.5f, i == current),
                 i == current ? (active ? t.bg : t.text) : t.textDim, juce::Justification::centred, i == current);
    }
    g.setColour(t.line);
    g.drawRoundedRectangle(area.reduced(0.5f), 4.0f, 1.0f);
    drawDecorations(g, getLocalBounds().toFloat());
}

// --- Glyphs ------------------------------------------------------------------------------------------

juce::Path glyphPath(Glyph glyph, juce::Rectangle<float> b) {
    juce::Path p;
    const float x = b.getX(), y = b.getY(), w = b.getWidth(), h = b.getHeight();
    const auto P = [&](float fx, float fy) { return juce::Point<float>(x + fx * w, y + fy * h); };
    const auto line = [&](float x1, float y1, float x2, float y2, float t = 0.12f) {
        juce::Path l;
        l.startNewSubPath(P(x1, y1));
        l.lineTo(P(x2, y2));
        juce::PathStrokeType(t * std::min(w, h), juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath(l, l);
        p.addPath(l);
    };
    switch (glyph) {
    case Glyph::Prev: p.addTriangle(P(0.75f, 0.15f), P(0.75f, 0.85f), P(0.2f, 0.5f)); break;
    case Glyph::Next: p.addTriangle(P(0.25f, 0.15f), P(0.25f, 0.85f), P(0.8f, 0.5f)); break;
    case Glyph::ChevronDown: line(0.15f, 0.35f, 0.5f, 0.7f, 0.16f); line(0.5f, 0.7f, 0.85f, 0.35f, 0.16f); break;
    case Glyph::ChevronUp: line(0.15f, 0.65f, 0.5f, 0.3f, 0.16f); line(0.5f, 0.3f, 0.85f, 0.65f, 0.16f); break;
    case Glyph::ChevronRight: line(0.35f, 0.15f, 0.7f, 0.5f, 0.16f); line(0.7f, 0.5f, 0.35f, 0.85f, 0.16f); break;
    case Glyph::Undo:
    case Glyph::Redo: {
        juce::Path a;
        a.addCentredArc(x + w * 0.5f, y + h * 0.58f, w * 0.32f, h * 0.3f, 0.0f, -2.2f, 1.6f, true);
        juce::PathStrokeType(0.12f * w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath(a, a);
        p.addPath(a);
        p.addTriangle(P(0.02f, 0.38f), P(0.36f, 0.16f), P(0.34f, 0.52f));
        if (glyph == Glyph::Redo)
            p.applyTransform(juce::AffineTransform::scale(-1.0f, 1.0f, x + w * 0.5f, y));
        break;
    }
    case Glyph::Menu: line(0.15f, 0.25f, 0.85f, 0.25f); line(0.15f, 0.5f, 0.85f, 0.5f); line(0.15f, 0.75f, 0.85f, 0.75f); break;
    case Glyph::Close: line(0.2f, 0.2f, 0.8f, 0.8f, 0.14f); line(0.8f, 0.2f, 0.2f, 0.8f, 0.14f); break;
    case Glyph::Plus: line(0.5f, 0.15f, 0.5f, 0.85f, 0.14f); line(0.15f, 0.5f, 0.85f, 0.5f, 0.14f); break;
    case Glyph::Minus: line(0.15f, 0.5f, 0.85f, 0.5f, 0.14f); break;
    case Glyph::Power: {
        p.addCentredArc(x + w * 0.5f, y + h * 0.55f, w * 0.38f, h * 0.38f, 0.0f, 0.6f, 2.0f * juce::MathConstants<float>::pi - 0.6f, true);
        p.startNewSubPath(P(0.5f, 0.05f));
        p.lineTo(P(0.5f, 0.5f));
        break;
    }
    case Glyph::Lock:
        p.addRoundedRectangle(x + w * 0.15f, y + h * 0.45f, w * 0.7f, h * 0.55f, w * 0.08f);
        {
            juce::Path shackle;
            shackle.addCentredArc(x + w * 0.5f, y + h * 0.45f, w * 0.24f, h * 0.32f, 0.0f, -1.5708f, 1.5708f, true);
            juce::PathStrokeType(0.13f * w).createStrokedPath(shackle, shackle);
            p.addPath(shackle);
        }
        break;
    case Glyph::Search: {
        juce::Path c;
        c.addEllipse(x + w * 0.1f, y + h * 0.1f, w * 0.55f, h * 0.55f);
        juce::PathStrokeType(0.12f * w).createStrokedPath(c, c);
        p.addPath(c);
        line(0.6f, 0.6f, 0.9f, 0.9f, 0.14f);
        break;
    }
    case Glyph::Keyboard:
        for (int i = 0; i < 5; ++i)
            p.addRectangle(x + w * (0.05f + 0.18f * static_cast<float>(i)), y + h * 0.2f, w * 0.15f, h * 0.6f);
        break;
    case Glyph::Map:
        p.addEllipse(x + w * 0.02f, y + h * 0.1f, w * 0.26f, h * 0.26f);
        p.addEllipse(x + w * 0.72f, y + h * 0.64f, w * 0.26f, h * 0.26f);
        line(0.2f, 0.3f, 0.8f, 0.7f, 0.1f);
        p.addEllipse(x + w * 0.72f, y + h * 0.1f, w * 0.26f, h * 0.26f);
        line(0.25f, 0.23f, 0.75f, 0.23f, 0.1f);
        break;
    case Glyph::Speaker:
        p.addRectangle(x + w * 0.08f, y + h * 0.35f, w * 0.2f, h * 0.3f);
        p.addTriangle(P(0.28f, 0.35f), P(0.55f, 0.1f), P(0.55f, 0.9f));
        p.addTriangle(P(0.28f, 0.65f), P(0.28f, 0.35f), P(0.55f, 0.9f));
        line(0.7f, 0.3f, 0.85f, 0.2f, 0.09f);
        line(0.7f, 0.5f, 0.9f, 0.5f, 0.09f);
        line(0.7f, 0.7f, 0.85f, 0.8f, 0.09f);
        break;
    case Glyph::Star:
    case Glyph::StarFilled: {
        p.addStar(b.getCentre(), 5, w * 0.2f, w * 0.48f, 0.0f);
        if (glyph == Glyph::Star) {
            juce::PathStrokeType(0.09f * w).createStrokedPath(p, p);
        }
        break;
    }
    case Glyph::Folder:
        p.addRoundedRectangle(x + w * 0.05f, y + h * 0.3f, w * 0.9f, h * 0.55f, w * 0.06f);
        p.addRoundedRectangle(x + w * 0.05f, y + h * 0.2f, w * 0.38f, h * 0.2f, w * 0.05f);
        break;
    case Glyph::Save:
        p.addRoundedRectangle(x + w * 0.1f, y + h * 0.1f, w * 0.8f, h * 0.8f, w * 0.08f);
        p.setUsingNonZeroWinding(false);
        p.addRectangle(x + w * 0.28f, y + h * 0.1f, w * 0.44f, h * 0.28f);
        p.addRectangle(x + w * 0.25f, y + h * 0.55f, w * 0.5f, h * 0.35f);
        break;
    case Glyph::Copy:
    case Glyph::Paste: {
        juce::Path a;
        a.addRoundedRectangle(x + w * 0.1f, y + h * 0.1f, w * 0.55f, h * 0.65f, w * 0.06f);
        a.addRoundedRectangle(x + w * 0.35f, y + h * 0.3f, w * 0.55f, h * 0.65f, w * 0.06f);
        juce::PathStrokeType(0.09f * w).createStrokedPath(a, a);
        p.addPath(a);
        break;
    }
    case Glyph::Dice:
        p.addRoundedRectangle(x + w * 0.1f, y + h * 0.1f, w * 0.8f, h * 0.8f, w * 0.15f);
        p.setUsingNonZeroWinding(false);
        for (const auto& [fx, fy] : {std::pair{0.32f, 0.32f}, std::pair{0.68f, 0.68f}, std::pair{0.5f, 0.5f}, std::pair{0.68f, 0.32f}, std::pair{0.32f, 0.68f}})
            p.addEllipse(x + w * fx - w * 0.07f, y + h * fy - h * 0.07f, w * 0.14f, h * 0.14f);
        break;
    case Glyph::Wand:
        line(0.15f, 0.85f, 0.65f, 0.35f, 0.12f);
        p.addStar(P(0.75f, 0.25f), 4, w * 0.05f, w * 0.18f, 0.0f);
        break;
    case Glyph::Blend:
        p.addEllipse(x + w * 0.05f, y + h * 0.2f, w * 0.55f, h * 0.55f);
        p.addEllipse(x + w * 0.4f, y + h * 0.2f, w * 0.55f, h * 0.55f);
        p.setUsingNonZeroWinding(false);
        break;
    case Glyph::Grip:
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 2; ++j)
                p.addEllipse(x + w * (0.3f + 0.3f * static_cast<float>(j)) - w * 0.08f, y + h * (0.2f + 0.3f * static_cast<float>(i)) - h * 0.08f, w * 0.16f, h * 0.16f);
        break;
    case Glyph::Pencil:
        line(0.2f, 0.8f, 0.8f, 0.2f, 0.18f);
        p.addTriangle(P(0.08f, 0.92f), P(0.14f, 0.7f), P(0.3f, 0.86f));
        break;
    case Glyph::Erase:
        p.addRoundedRectangle(x + w * 0.15f, y + h * 0.3f, w * 0.7f, h * 0.4f, w * 0.06f);
        p.applyTransform(juce::AffineTransform::rotation(-0.6f, b.getCentreX(), b.getCentreY()));
        break;
    case Glyph::Play: p.addTriangle(P(0.25f, 0.15f), P(0.25f, 0.85f), P(0.85f, 0.5f)); break;
    case Glyph::Stop: p.addRectangle(x + w * 0.2f, y + h * 0.2f, w * 0.6f, h * 0.6f); break;
    case Glyph::History: {
        juce::Path c;
        c.addEllipse(x + w * 0.1f, y + h * 0.1f, w * 0.8f, h * 0.8f);
        juce::PathStrokeType(0.1f * w).createStrokedPath(c, c);
        p.addPath(c);
        line(0.5f, 0.5f, 0.5f, 0.25f, 0.1f);
        line(0.5f, 0.5f, 0.68f, 0.6f, 0.1f);
        break;
    }
    case Glyph::Gear:
        p.addStar(b.getCentre(), 8, w * 0.3f, w * 0.46f, 0.0f);
        p.setUsingNonZeroWinding(false);
        p.addEllipse(b.withSizeKeepingCentre(w * 0.3f, h * 0.3f));
        break;
    case Glyph::Link:
        line(0.3f, 0.7f, 0.7f, 0.3f, 0.12f);
        {
            juce::Path c;
            c.addRoundedRectangle(x + w * 0.05f, y + h * 0.45f, w * 0.45f, h * 0.3f, h * 0.15f);
            c.addRoundedRectangle(x + w * 0.5f, y + h * 0.25f, w * 0.45f, h * 0.3f, h * 0.15f);
            juce::PathStrokeType(0.09f * w).createStrokedPath(c, c);
            p.addPath(c);
        }
        break;
    case Glyph::Trash:
        p.addRectangle(x + w * 0.2f, y + h * 0.3f, w * 0.6f, h * 0.6f);
        p.addRectangle(x + w * 0.12f, y + h * 0.18f, w * 0.76f, h * 0.08f);
        break;
    case Glyph::Help: {
        juce::Path c;
        c.addEllipse(x + w * 0.1f, y + h * 0.1f, w * 0.8f, h * 0.8f);
        juce::PathStrokeType(0.1f * w).createStrokedPath(c, c);
        p.addPath(c);
        p.addEllipse(x + w * 0.45f, y + h * 0.66f, w * 0.1f, h * 0.1f);
        line(0.5f, 0.56f, 0.5f, 0.5f, 0.1f);
        juce::Path q;
        q.addCentredArc(x + w * 0.5f, y + h * 0.38f, w * 0.14f, h * 0.12f, 0.0f, -1.6f, 2.4f, true);
        juce::PathStrokeType(0.1f * w).createStrokedPath(q, q);
        p.addPath(q);
        break;
    }
    case Glyph::Check: line(0.15f, 0.55f, 0.42f, 0.8f, 0.16f); line(0.42f, 0.8f, 0.88f, 0.22f, 0.16f); break;
    // The drawing shapes, as the line each draws across a segment.
    case Glyph::ShapeLine: line(0.1f, 0.8f, 0.9f, 0.2f, 0.13f); break;
    case Glyph::ShapeFlat: line(0.1f, 0.5f, 0.9f, 0.5f, 0.13f); line(0.1f, 0.25f, 0.1f, 0.75f, 0.08f); line(0.9f, 0.25f, 0.9f, 0.75f, 0.08f); break;
    case Glyph::ShapeRampUp: line(0.1f, 0.85f, 0.75f, 0.15f, 0.13f); line(0.75f, 0.15f, 0.75f, 0.85f, 0.13f); line(0.75f, 0.85f, 0.92f, 0.85f, 0.1f); break;
    case Glyph::ShapeRampDown: line(0.25f, 0.85f, 0.25f, 0.15f, 0.13f); line(0.25f, 0.15f, 0.9f, 0.85f, 0.13f); line(0.08f, 0.85f, 0.25f, 0.85f, 0.1f); break;
    case Glyph::ShapeSine:
    case Glyph::ShapeCurve: {
        juce::Path s;
        for (int i = 0; i <= 24; ++i) {
            const float u = static_cast<float>(i) / 24.0f;
            const float v = glyph == Glyph::ShapeSine ? 0.5f - 0.36f * std::sin(u * juce::MathConstants<float>::twoPi)
                                                      : 0.85f - 0.7f * u * u; // a bend that leaves slowly and arrives steep
            if (i == 0) s.startNewSubPath(P(0.1f + 0.8f * u, v)); else s.lineTo(P(0.1f + 0.8f * u, v));
        }
        juce::PathStrokeType(0.12f * std::min(w, h), juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath(s, s);
        p.addPath(s);
        break;
    }
    case Glyph::None: break;
    }
    return p;
}

// --- Button ------------------------------------------------------------------------------------------

Button::Button(juce::String text, Glyph g) : glyph(g), text_(std::move(text)) {
    setWantsKeyboardFocus(true);
    setTitle(text_);
}

void Button::draw(juce::Graphics& g) {
    const Theme& t = theme();
    auto area = getLocalBounds().toFloat().reduced(0.5f);
    const bool over = isMouseOver(true) && isEnabled();
    const bool down = isMouseButtonDown() && over;
    if (checkbox || switchStyle) {
        // A state, not an action: a box with a check, or a sliding pill, and the label beside it.
        auto row = area;
        if (checkbox) {
            const auto box = row.removeFromLeft(16.0f).withSizeKeepingCentre(15.0f, 15.0f);
            g.setColour(toggled_ ? t.control : t.sunken);
            g.fillRoundedRectangle(box, 3.0f);
            g.setColour(toggled_ ? t.control : over ? t.textDim : t.textFaint);
            g.drawRoundedRectangle(box.reduced(0.5f), 3.0f, 1.0f);
            if (toggled_) {
                g.setColour(t.bg);
                g.fillPath(glyphPath(Glyph::Check, box.reduced(2.5f)));
            }
            row.removeFromLeft(8.0f);
        } else {
            const auto pill = row.removeFromLeft(30.0f).withSizeKeepingCentre(30.0f, 16.0f);
            g.setColour(toggled_ ? t.control : t.sunken);
            g.fillRoundedRectangle(pill, 8.0f);
            g.setColour(toggled_ ? t.control : over ? t.textDim : t.lineStrong);
            g.drawRoundedRectangle(pill.reduced(0.5f), 8.0f, 1.0f);
            g.setColour(toggled_ ? t.bg : t.textDim);
            g.fillEllipse(juce::Rectangle<float>(11.0f, 11.0f).withCentre({toggled_ ? pill.getRight() - 8.0f : pill.getX() + 8.0f, pill.getCentreY()}));
            row.removeFromLeft(8.0f);
        }
        drawText(g, text_, row, fittingSize(text_, row.getWidth(), textSize, boldText), !isEnabled() ? t.textFaint : toggled_ || over ? t.text : t.textDim,
                 juce::Justification::centredLeft, boldText);
        drawFocus(g, getLocalBounds().toFloat());
        return;
    }
    if (accentStyle) { // the primary action: the control ink, dark text
        g.setColour(!isEnabled() ? t.raised : down ? t.control.darker(0.2f) : over ? t.control.brighter(0.1f) : t.control);
        g.fillRoundedRectangle(area, 4.0f);
    } else if (toggled_ && !tab) {
        g.setColour(t.selection);
        g.fillRoundedRectangle(area, 4.0f);
        g.setColour(t.control.withAlpha(0.75f));
        g.drawRoundedRectangle(area, 4.0f, 1.0f);
    } else if (!ghost && !tab) { // ghost buttons (list rows, icon buttons) and tabs draw no box, not even under the pointer
        g.setColour(down ? t.sunken : over ? t.raised.brighter(0.08f) : t.raised);
        g.fillRoundedRectangle(area, 4.0f);
        g.setColour(over && isEnabled() ? t.lineStrong : t.line);
        g.drawRoundedRectangle(area, 4.0f, 1.0f);
    }
    const juce::Colour ink = !isEnabled() ? (accentStyle ? t.textDim : t.textFaint.withAlpha(0.7f))
                             : accentStyle ? t.bg
                             : tab         ? (toggled_ || over ? t.text : t.textDim)
                             : dimText && !over ? t.textDim
                                                : t.text;
    auto content = area.reduced(6.0f, 0.0f);
    if (latch) { // a lamp that is lit while the latch is on
        const auto lamp = content.removeFromLeft(7.0f).withSizeKeepingCentre(7.0f, 7.0f);
        g.setColour(toggled_ ? t.lamp : t.sunken);
        g.fillEllipse(lamp);
        g.setColour(toggled_ ? t.lamp : t.textFaint);
        g.drawEllipse(lamp.reduced(0.5f), 1.0f);
        content.removeFromLeft(5.0f);
    }
    if (glyph != Glyph::None) {
        const float s = std::min(14.0f, area.getHeight() - 8.0f);
        const auto box = text_.isEmpty() ? area.withSizeKeepingCentre(s, s)
                                         : juce::Rectangle<float>(s, s).withPosition(content.getX(), area.getCentreY() - s * 0.5f);
        g.setColour(ink);
        g.fillPath(glyphPath(glyph, box));
        if (text_.isNotEmpty())
            content.removeFromLeft(s + 5.0f);
    }
    if (text_.isNotEmpty()) {
        const bool bold = boldText || (tab && toggled_) || accentStyle;
        drawText(g, text_, content, fittingSize(text_, content.getWidth(), textSize, bold), ink,
                 glyph != Glyph::None || leftAlign || latch ? juce::Justification::centredLeft : juce::Justification::centred, bold);
    }
    drawFocus(g, getLocalBounds().toFloat());
}

void Button::setToggled(bool on) {
    if (on == toggled_)
        return;
    toggled_ = on;
    repaint();
    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}

void Button::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu() && onRightClick) {
        onRightClick(e);
        return;
    }
    repaint();
}

void Button::mouseUp(const juce::MouseEvent& e) {
    repaint();
    if (e.mods.isPopupMenu() || !isEnabled() || !getLocalBounds().contains(e.getPosition()))
        return;
    if (onClick)
        onClick();
}

bool Button::keyPressed(const juce::KeyPress& key) {
    if ((key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) && isEnabled() && onClick) {
        onClick();
        return true;
    }
    const int code = key.getKeyCode();
    if (onArrow && (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && !key.getModifiers().isAnyModifierKeyDown()) {
        onArrow(code == juce::KeyPress::rightKey ? 1 : -1);
        return true;
    }
    return false;
}

namespace {
// A button's handler that reports its state: a toggle, checkbox or switch is checkable and checked
// while on; a tab is one of a set (a radio button to screen readers), selected while it is the one shown.
class ButtonHandler final : public juce::AccessibilityHandler {
public:
    ButtonHandler(Button& b, juce::AccessibilityRole role, juce::AccessibilityActions actions)
        : juce::AccessibilityHandler(b, role, std::move(actions)), button_(b) {}
    juce::AccessibleState getCurrentState() const override {
        auto state = juce::AccessibilityHandler::getCurrentState();
        if (button_.toggleRole || button_.tab || button_.checkbox || button_.switchStyle) {
            state = state.withCheckable();
            if (button_.toggled())
                state = state.withChecked();
        }
        return state;
    }

private:
    Button& button_;
};
} // namespace

std::unique_ptr<juce::AccessibilityHandler> Button::createAccessibilityHandler() {
    juce::AccessibilityActions actions;
    actions.addAction(juce::AccessibilityActionType::press, [this] {
        if (isEnabled() && onClick)
            onClick();
    });
    const bool checkable = toggleRole || tab || checkbox || switchStyle;
    if (checkable)
        actions.addAction(juce::AccessibilityActionType::toggle, [this] {
            if (isEnabled() && onClick)
                onClick();
        });
    const auto role = tab ? juce::AccessibilityRole::radioButton : checkable ? juce::AccessibilityRole::toggleButton : juce::AccessibilityRole::button;
    return std::make_unique<ButtonHandler>(*this, role, std::move(actions));
}

// --- TabStrip ----------------------------------------------------------------------------------------

TabStrip::TabStrip(juce::StringArray names) {
    setAccessible(true);
    for (int i = 0; i < names.size(); ++i) {
        auto tab = std::make_unique<Button>(names[i]);
        tab->tab = true;
        tab->textSize = kTextSmall + 0.5f;
        tab->onClick = [this, i] { setSelected(i); };
        tab->onArrow = [this, i](int step) { // the next visible tab that way takes the focus and shows
            for (int j = i + step; j >= 0 && j < count(); j += step)
                if (tabs_[static_cast<std::size_t>(j)]->isVisible()) {
                    tabs_[static_cast<std::size_t>(j)]->grabKeyboardFocus();
                    setSelected(j);
                    return;
                }
        };
        addAndMakeVisible(*tab);
        tabs_.push_back(std::move(tab));
    }
    setSelected(0, false);
}

void TabStrip::setSelected(int index, bool notify) {
    if (index < 0 || index >= count())
        return;
    selected_ = index;
    for (int i = 0; i < count(); ++i) // the selected tab is checked: screen readers announce which one is shown
        tabs_[static_cast<std::size_t>(i)]->setToggled(i == index);
    repaint();
    if (notify && onChange)
        onChange(index);
}

void TabStrip::setTabVisible(int index, bool visible) {
    tabs_[static_cast<std::size_t>(index)]->setVisible(visible);
    resized();
}

void TabStrip::setName(int index, const juce::String& name) { tabs_[static_cast<std::size_t>(index)]->setText(name); }

void TabStrip::resized() {
    auto area = getLocalBounds();
    const float size = large ? kTextBody + 0.5f : kTextSmall + 0.5f;
    // Widths for the bold label the selected tab shows, so no tab changes size when chosen.
    int total = 0;
    for (const auto& tab : tabs_)
        if (tab->isVisible())
            total += static_cast<int>(textWidth(tab->text(), size, true)) + (large ? 30 : 16);
    const float stretch = total > area.getWidth() ? static_cast<float>(area.getWidth()) / static_cast<float>(total) : 1.0f;
    for (auto& tab : tabs_) {
        if (!tab->isVisible())
            continue;
        tab->textSize = size;
        const int w = static_cast<int>((textWidth(tab->text(), size, true) + (large ? 30.0f : 16.0f)) * stretch);
        tab->setBounds(area.removeFromLeft(w));
        if (large)
            area.removeFromLeft(2);
    }
}

void TabStrip::draw(juce::Graphics& g) {
    const Theme& t = theme();
    if (selected_ < 0 || selected_ >= count())
        return;
    const auto b = tabs_[static_cast<std::size_t>(selected_)]->getBounds().toFloat();
    if (large) {
        // The page on show sits in a recessed seat: dark, shaded from the top, with a lit lower lip.
        const auto seat = b.reduced(0.0f, 2.0f);
        g.setColour(t.sunken);
        g.fillRoundedRectangle(seat, 4.0f);
        g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(0.5f), 0.0f, seat.getY(), juce::Colours::transparentBlack, 0.0f,
                                               seat.getY() + 6.0f, false));
        g.fillRoundedRectangle(seat.withHeight(6.0f), 4.0f);
        g.setColour(juce::Colours::black.withAlpha(0.7f));
        g.drawRoundedRectangle(seat.reduced(0.5f), 4.0f, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.fillRect(seat.getX() + 4.0f, seat.getBottom(), seat.getWidth() - 8.0f, 1.0f);
        return;
    }
    // A module's own groups: a hairline under the strip, a light underline under the chosen group.
    g.setColour(t.line);
    g.fillRect(getLocalBounds().removeFromBottom(1));
    g.setColour(t.control.withAlpha(0.85f));
    g.fillRect(juce::Rectangle<float>(b.getX() + 4.0f, static_cast<float>(getHeight()) - 2.0f, b.getWidth() - 8.0f, 2.0f));
}

// --- Card --------------------------------------------------------------------------------------------

Card::Card(juce::String title, Family f) : title_(std::move(title)) {
    family = f;
    setTitle(title_);
    setAccessible(true);
    setOpaque(true); // it paints every pixel (its corners in the chassis colour)
}

juce::Rectangle<int> Card::header() const { return getLocalBounds().removeFromTop(headerHeight); }

juce::Rectangle<int> Card::body() const {
    if (collapsed)
        return {};
    return getLocalBounds().withTrimmedTop(headerHeight).reduced(8, 6);
}

void Card::resized() {
    auto h = header().reduced(6, 4);
    if (collapsible)
        h.removeFromLeft(14);
    if (powerSwitch != nullptr)
        powerSwitch->setBounds(h.removeFromLeft(h.getHeight()).reduced(1));
    for (auto* c : headerRight) {
        if (c == nullptr || !c->isVisible())
            continue;
        const int w = c->getWidth() > 0 ? c->getWidth() : 60;
        c->setBounds(h.removeFromRight(w).withSizeKeepingCentre(w, juce::jmin(h.getHeight(), 20)));
        h.removeFromRight(4);
    }
    layoutBody(body());
}

bool Card::bypassed() const { return powerSwitch != nullptr && powerSwitch->value() < 0.5; }

bool Card::tick() {
    const bool off = bypassed();
    if (off == bypassShown_)
        return false;
    bypassShown_ = off;
    return true;
}

void Card::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    bypassShown_ = bypassed();
    // Where the title may run: up to the first header part on the right.
    float right = static_cast<float>(getWidth());
    for (auto* c : headerRight)
        if (c != nullptr && c->isVisible())
            right = std::min(right, static_cast<float>(c->getX()) - 4.0f);
    if (drawHeaderExtra && !headerExtraArea.isEmpty())
        right = std::min(right, headerExtraArea.getX() - 4.0f);
    // Everything here changes only with the title, the header's layout and the power state: one
    // copy per repaint.
    std::uint64_t key = static_cast<std::uint64_t>(title_.hashCode64());
    for (const int v : {collapsed ? 1 : 0, screws ? 1 : 0, headerHeight, collapsible ? 1 : 0, powerSwitch != nullptr ? 1 : 0,
                        juce::roundToInt(right), static_cast<int>(family), bypassShown_ ? 1 : 0})
        key = key * 1099511628211ull + static_cast<std::uint64_t>(v + 1);
    background_.draw(g, getLocalBounds(), key, true, [&](juce::Graphics& bg) {
        bg.fillAll(t.bg); // the corners outside the rounded panel: the chassis
        drawPanel(bg, area, t.panel);
        if (t.metal > 0.0f) {
            drawPlate(bg, header().toFloat().reduced(1.0f, 0.0f).withTrimmedTop(1.0f), t.radius);
            if (screws && !collapsed && getHeight() > 90 && getWidth() > 120) {
                const float inset = 7.0f;
                drawScrew(bg, {inset, area.getBottom() - inset}, 2.8f, 0.3f);
                drawScrew(bg, {area.getRight() - inset, area.getBottom() - inset}, 2.8f, 0.9f);
            }
        }
        auto h = header().toFloat().reduced(6.0f, 4.0f);
        if (collapsible) {
            bg.setColour(t.textDim);
            bg.fillPath(glyphPath(collapsed ? Glyph::ChevronRight : Glyph::ChevronDown, h.removeFromLeft(12.0f).withSizeKeepingCentre(9.0f, 9.0f)));
            h.removeFromLeft(2.0f);
        }
        if (powerSwitch != nullptr)
            h.removeFromLeft(h.getHeight() + 5.0f); // the title starts 5 px after the power control
        else
            h.removeFromLeft(2.0f);
        const juce::String title = title_.toUpperCase();
        h = h.withRight(std::min(right, h.getRight()));
        const float titleW = std::min(textWidth(title, kTextSmall + 0.5f, true) + 2.0f, h.getWidth());
        drawText(bg, title, h.removeFromLeft(titleW), kTextSmall + 0.5f, bypassShown_ ? t.textDim : t.text, juce::Justification::centredLeft, true);
        if (bypassShown_ && h.getWidth() > 40.0f) { // a module that is off says so in words, beside its name
            h.removeFromLeft(8.0f);
            const auto badge = h.removeFromLeft(textWidth("OFF", kTextSmall, true) + 12.0f).withSizeKeepingCentre(textWidth("OFF", kTextSmall, true) + 12.0f, 16.0f);
            bg.setColour(t.textFaint);
            bg.drawRoundedRectangle(badge.reduced(0.5f), 3.0f, 1.0f);
            drawText(bg, "OFF", badge, kTextSmall, t.textDim, juce::Justification::centred, true);
        }
        if (!collapsed) {
            bg.setColour(t.line);
            bg.fillRect(juce::Rectangle<float>(area.getX() + 1.0f, static_cast<float>(headerHeight) - 1.0f, area.getWidth() - 2.0f, 1.0f));
        }
    });
    if (drawHeaderExtra && !headerExtraArea.isEmpty())
        drawHeaderExtra(g, headerExtraArea);
}

void Card::mouseDown(const juce::MouseEvent& e) {
    if (e.y >= headerHeight)
        return;
    if (collapsible && e.x < 26) {
        collapsed = !collapsed;
        if (onCollapse)
            onCollapse(collapsed);
        return;
    }
    if (onHeaderClick)
        onHeaderClick();
}

// --- DragChip ----------------------------------------------------------------------------------------

DragChip::DragChip(Context& ctx, juce::String source) : ctx_(ctx), source_(std::move(source)) {
    text = Context::sourceName(source_);
    setWantsKeyboardFocus(true);
    setTitle(text + " modulation source");
    setHelp(text, "Drag onto any control to modulate it. Click to select it: Alt-drag on a control then sets its depth.");
    family = Family::Modulators;
}

bool DragChip::tick() {
    if ((++frames_ & 1u) != 0) // the live bar moves at half the frame rate
        return false;
    const int index = Context::sourceIndex(source_);
    const float v = index >= 0 ? ctx_.processor.engine().sourceValue(index) : 0.0f;
    if (std::abs(v - value_) > 0.004f) {
        value_ = v;
        return true;
    }
    return false;
}

void DragChip::setOpen(bool open) {
    if (open != open_) {
        open_ = open;
        repaint();
    }
}

void DragChip::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat().reduced(0.5f);
    const juce::Colour c = ctx_.sourceColour(source_);
    const bool selected = ctx_.selectedSource == source_;
    // The chip itself changes only with its states and its name: kept as an image, so the live
    // value's bar repaints over a copy. Two states, two cues: the source chosen for routing has a
    // ring in its own colour around its dot and grip (what is dragged and Alt-dragged); the one
    // whose graph the dock shows is a seated tab, lighter and bold, with a notch towards the graph.
    const std::uint64_t key = ((static_cast<std::uint64_t>(text.hashCode64()) ^ static_cast<std::uint64_t>(c.getARGB())) << 2) | (selected ? 1u : 0u) |
                              (open_ ? 2u : 0u);
    body_.draw(g, getLocalBounds(), key, false, [&](juce::Graphics& cg) {
        const float radius = area.getHeight() * 0.5f;
        cg.setColour(open_ ? t.raised.brighter(0.22f) : t.raised);
        cg.fillRoundedRectangle(area, radius);
        if (selected) {
            cg.setColour(c);
            cg.drawRoundedRectangle(area.reduced(0.8f), radius - 0.8f, 1.6f);
        } else {
            cg.setColour(open_ ? t.control.withAlpha(0.55f) : t.line);
            cg.drawRoundedRectangle(area, radius, 1.0f);
        }
        auto inner = area.reduced(area.getHeight() * 0.35f, 0.0f);
        cg.setColour(c);
        cg.fillEllipse(inner.removeFromLeft(7.0f).withSizeKeepingCentre(7.0f, 7.0f));
        inner.removeFromLeft(4.0f);
        cg.setColour(selected ? c : t.textDim);
        cg.fillPath(glyphPath(Glyph::Grip, inner.removeFromRight(8.0f).withSizeKeepingCentre(8.0f, 11.0f)));
        drawText(cg, text, inner, fittingSize(text, inner.getWidth(), kTextSmall + 0.5f, open_), open_ ? t.text : t.textDim, juce::Justification::centredLeft,
                 open_);
        if (open_) { // the notch: the tab points down at the graph it opens
            juce::Path notch;
            const float cx = area.getCentreX(), b = area.getBottom();
            notch.addTriangle(cx - 4.0f, b - 3.5f, cx + 4.0f, b - 3.5f, cx, b + 0.5f);
            cg.setColour(t.control);
            cg.fillPath(notch);
        }
    });
    // The source's live value as a thin bar along the bottom edge.
    const float v = juce::jlimit(-1.0f, 1.0f, value_);
    const float mid = area.getCentreX(), half = area.getWidth() * 0.5f - 8.0f;
    const float x0 = v >= 0 ? (source_.startsWith("lfo") || source_.startsWith("flux") ? mid : area.getX() + 8.0f) : mid + v * half;
    const float x1 = v >= 0 ? (source_.startsWith("lfo") || source_.startsWith("flux") ? mid + v * half : area.getX() + 8.0f + v * (area.getWidth() - 16.0f)) : mid;
    g.setColour(c);
    g.fillRect(juce::Rectangle<float>(std::min(x0, x1), area.getBottom() - 2.5f, std::abs(x1 - x0), 1.5f));
    drawFocus(g, area, area.getHeight() * 0.5f);
}

void DragChip::mouseDown(const juce::MouseEvent&) { dragging_ = false; }

void DragChip::mouseDrag(const juce::MouseEvent& e) {
    if (dragging_ || e.getDistanceFromDragStart() < 4)
        return;
    dragging_ = true;
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
        container->startDragging("mod:" + source_, this);
}

void DragChip::mouseUp(const juce::MouseEvent&) {
    if (!dragging_) {
        ctx_.selectedSource = ctx_.selectedSource == source_ ? juce::String() : source_;
        if (onSelect)
            onSelect();
        repaint();
    }
    dragging_ = false;
}

void DragChip::mouseEnter(const juce::MouseEvent&) {
    ctx_.hoveredSource = source_;
    repaint();
}

void DragChip::mouseExit(const juce::MouseEvent&) {
    if (ctx_.hoveredSource == source_)
        ctx_.hoveredSource.clear();
    repaint();
}

bool DragChip::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) {
        ctx_.selectedSource = ctx_.selectedSource == source_ ? juce::String() : source_;
        if (onSelect)
            onSelect();
        repaint();
        return true;
    }
    return false;
}

std::unique_ptr<juce::AccessibilityHandler> DragChip::createAccessibilityHandler() {
    juce::AccessibilityActions actions;
    actions.addAction(juce::AccessibilityActionType::press, [this] {
        ctx_.selectedSource = ctx_.selectedSource == source_ ? juce::String() : source_;
        if (onSelect)
            onSelect();
        repaint();
    });
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::button, std::move(actions));
}

// --- Meter -------------------------------------------------------------------------------------------

Meter::Meter(Context& ctx) : ctx_(ctx) {
    setTitle("Output level");
    setHelp("Output", "Peak level of the output, left and right; the mark is 0 dBFS.");
    setInterceptsMouseClicks(false, false);
}

bool Meter::tick() {
    if ((++frames_ & 1u) != 0) // 15 updates a second are plenty for a meter; the peaks wait in the processor
        return false;
    bool dirty = false;
    for (int ch = 0; ch < 2; ++ch) {
        const float peak = ctx_.processor.meterPeak(ch);
        const float next = std::max(peak, level_[ch] * 0.67f); // the fall of two frames
        const float shown = next < 1e-4f ? 0.0f : next;
        if (std::abs(shown - level_[ch]) > 1e-4f) {
            level_[ch] = shown;
            dirty = true;
        }
        if (peak >= hold_[ch]) {
            hold_[ch] = peak;
            holdFrames_[ch] = 23;
        } else if (holdFrames_[ch] > 0 && --holdFrames_[ch] == 0) {
            hold_[ch] = 0.0f;
            dirty = true;
        }
    }
    return dirty;
}

void Meter::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    const auto fraction = [](float gain) {
        const float db = juce::Decibels::gainToDecibels(gain, -60.0f);
        return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 66.0f);
    };
    for (int ch = 0; ch < 2; ++ch) {
        juce::Rectangle<float> bar = vertical ? area.withWidth(area.getWidth() * 0.5f - 1.0f).withX(area.getX() + static_cast<float>(ch) * area.getWidth() * 0.5f)
                                              : area.withHeight(area.getHeight() * 0.5f - 1.0f).withY(area.getY() + static_cast<float>(ch) * area.getHeight() * 0.5f);
        g.setColour(t.sunken);
        g.fillRoundedRectangle(bar, 1.5f);
        const float f = fraction(level_[ch]);
        const bool over = level_[ch] > 1.0f;
        g.setColour(over ? t.danger : t.ok);
        g.fillRoundedRectangle(vertical ? bar.withTop(bar.getBottom() - bar.getHeight() * f) : bar.withWidth(bar.getWidth() * f), 1.5f);
        if (hold_[ch] > 0.0f) {
            const float h = fraction(hold_[ch]);
            g.setColour(hold_[ch] > 1.0f ? t.danger : t.text);
            if (vertical)
                g.fillRect(bar.getX(), bar.getBottom() - bar.getHeight() * h, bar.getWidth(), 1.0f);
            else
                g.fillRect(bar.getX() + bar.getWidth() * h - 1.0f, bar.getY(), 1.0f, bar.getHeight());
        }
    }
    g.setColour(t.textDim);
    const float zero = 60.0f / 66.0f;
    if (vertical)
        g.fillRect(area.getX(), area.getBottom() - area.getHeight() * zero, area.getWidth(), 1.0f);
    else
        g.fillRect(area.getX() + area.getWidth() * zero, area.getY(), 1.0f, area.getHeight());
}

// --- TextField ---------------------------------------------------------------------------------------

TextField::TextField(const juce::String& placeholderText, const juce::String& accessibleName) : placeholder(placeholderText) {
    setTitle(accessibleName);
    setFont(font(kTextBody));
    setTextToShowWhenEmpty(placeholderText, theme().textFaint);
    setIndents(8, 4);
    setJustification(juce::Justification::centredLeft);
}

void TextField::paintOverChildren(juce::Graphics& g) { juce::TextEditor::paintOverChildren(g); }

void TextField::lookAndFeelChanged() {
    juce::TextEditor::lookAndFeelChanged();
    setFont(font(kTextBody));
    applyFontToAllText(font(kTextBody));
}

// --- Helpers -----------------------------------------------------------------------------------------

juce::Rectangle<int> placeRow(juce::Rectangle<int> area, std::initializer_list<std::pair<juce::Component*, int>> items, int gap) {
    auto row = area;
    for (const auto& [c, w] : items) {
        if (c != nullptr)
            c->setBounds(row.removeFromLeft(w));
        else
            row.removeFromLeft(w);
        row.removeFromLeft(gap);
    }
    return area.withRight(row.getX());
}

// --- Cached drawing -------------------------------------------------------------------------------------

void CachedDrawing::draw(juce::Graphics& g, juce::Rectangle<int> area, std::uint64_t key, bool opaque, const std::function<void(juce::Graphics&)>& paint) {
    if (area.isEmpty())
        return;
    if (AuditRecorder::active()) { // the snapshot checks see every text drawn, so nothing comes from a copy
        paint(g);
        return;
    }
    const float scale = juce::jmax(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    const int w = juce::jmax(1, juce::roundToInt(static_cast<float>(area.getWidth()) * scale));
    const int h = juce::jmax(1, juce::roundToInt(static_cast<float>(area.getHeight()) * scale));
    if (!image_.isValid() || image_.getWidth() != w || image_.getHeight() != h || area != area_ || key != key_ || theme_ != theme().name ||
        textScale_ != textScale()) {
        image_ = juce::Image(opaque ? juce::Image::RGB : juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
        juce::Graphics ig(image_);
        ig.addTransform(juce::AffineTransform::translation(static_cast<float>(-area.getX()), static_cast<float>(-area.getY()))
                            .scaled(static_cast<float>(w) / static_cast<float>(area.getWidth()), static_cast<float>(h) / static_cast<float>(area.getHeight())));
        paint(ig);
        area_ = area;
        key_ = key;
        theme_ = theme().name;
        textScale_ = textScale();
    }
    g.drawImage(image_, area.toFloat()); // at the display's density: one pixel of the image per pixel
}

// --- The hardware finish -------------------------------------------------------------------------------

namespace {

// A tile of fine grain with faint horizontal brushing, made once. Panels and the chassis draw it
// over their fill at a low opacity; tiles are anchored at the editor's origin so neighbouring
// panels line up.
const juce::Image& grainTile(bool dark) {
    static juce::Image tiles[2];
    juce::Image& tile = tiles[dark ? 1 : 0];
    if (tile.isNull()) {
        constexpr int kSize = 192;
        tile = juce::Image(juce::Image::ARGB, kSize, kSize, true, juce::SoftwareImageType());
        juce::Random random(dark ? 0x5eed : 0x5eee);
        std::vector<float> streak(kSize);
        for (int y = 0; y < kSize; ++y) // brushing: each row a little lighter or darker than the next
            streak[static_cast<std::size_t>(y)] = (random.nextFloat() - 0.5f) * 0.5f;
        juce::Image::BitmapData data(tile, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x) {
                const float v = (random.nextFloat() - 0.5f) + streak[static_cast<std::size_t>(y)];
                const auto alpha = static_cast<juce::uint8>(juce::jlimit(0, 255, static_cast<int>(std::abs(v) * 255.0f)));
                data.setPixelColour(x, y, (v > 0 ? juce::Colours::white : juce::Colours::black).withAlpha(alpha));
            }
    }
    return tile;
}

} // namespace

void drawGrain(juce::Graphics& g, const juce::Path& shape, float strength) {
    const Theme& t = theme();
    const float alpha = strength * t.texture;
    if (alpha <= 0.0f)
        return;
    g.saveState();
    g.setTiledImageFill(grainTile(t.dark), 0, 0, juce::jlimit(0.0f, 1.0f, alpha));
    g.fillPath(shape);
    g.restoreState();
}

void drawChassis(juce::Graphics& g, juce::Rectangle<float> area) {
    const Theme& t = theme();
    g.setColour(t.bg);
    g.fillRect(area);
    juce::Path p;
    p.addRectangle(area);
    drawGrain(g, p, t.dark ? 0.05f : 0.04f); // the enclosure is the one textured surface
}

void drawScrew(juce::Graphics& g, juce::Point<float> c, float radius, float angle) {
    const Theme& t = theme();
    if (t.metal <= 0.0f)
        return;
    const auto head = juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(c);
    // Countersink shadow, then a domed head lit from the top left.
    g.setColour(juce::Colours::black.withAlpha(t.dark ? 0.55f : 0.25f));
    g.fillEllipse(head.expanded(1.0f).translated(0.0f, 0.6f));
    const juce::Colour metal = t.dark ? juce::Colour(0xff9aa2ac) : juce::Colour(0xffc4c9cf);
    juce::ColourGradient dome(metal.brighter(0.5f), head.getX() + radius * 0.6f, head.getY() + radius * 0.5f, metal.darker(0.9f),
                              head.getRight(), head.getBottom(), true);
    g.setGradientFill(dome);
    g.fillEllipse(head);
    // A hex socket.
    juce::Path hex;
    const float r = radius * 0.48f;
    for (int i = 0; i < 6; ++i) {
        const float a = angle + juce::MathConstants<float>::pi / 3.0f * static_cast<float>(i);
        const juce::Point<float> p(c.x + r * std::cos(a), c.y + r * std::sin(a));
        if (i == 0) hex.startNewSubPath(p); else hex.lineTo(p);
    }
    hex.closeSubPath();
    g.setColour(juce::Colour(0xff15171a).withAlpha(0.85f));
    g.fillPath(hex);
    g.setColour(juce::Colours::white.withAlpha(0.18f));
    g.drawEllipse(head.reduced(0.4f), 0.6f);
}

void drawPanel(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour fill, bool border) {
    const Theme& t = theme();
    // A soft drop shadow under machined panels.
    if (t.metal > 0.0f && border) {
        g.setColour(juce::Colours::black.withAlpha((t.dark ? 0.35f : 0.12f) * t.metal));
        g.fillRoundedRectangle(area.translated(0.0f, 1.5f), t.radius);
    }
    if (t.metal > 0.0f)
        g.setGradientFill(juce::ColourGradient(fill.brighter(0.07f * t.metal), area.getX(), area.getY(), fill.darker(0.10f * t.metal), area.getX(),
                                               area.getBottom(), false));
    else
        g.setColour(fill);
    g.fillRoundedRectangle(area, t.radius); // a labelled work surface: a quiet fill, no grain
    if (border) {
        g.setColour(t.line);
        g.drawRoundedRectangle(area.reduced(0.5f), t.radius, 1.0f);
        if (t.metal > 0.0f) { // the lit top edge of a bevel
            g.setColour(juce::Colours::white.withAlpha((t.dark ? 0.07f : 0.5f) * t.metal));
            g.drawHorizontalLine(static_cast<int>(area.getY() + 1.0f), area.getX() + t.radius, area.getRight() - t.radius);
        }
    }
}

void drawPlate(juce::Graphics& g, juce::Rectangle<float> area, float radius) {
    const Theme& t = theme();
    const juce::Colour base = t.dark ? t.bg.brighter(0.05f) : t.raised.darker(0.04f);
    g.setGradientFill(juce::ColourGradient(base.brighter(0.08f * t.metal), area.getX(), area.getY(), base.darker(0.08f * t.metal), area.getX(),
                                           area.getBottom(), false));
    juce::Path plate;
    plate.addRoundedRectangle(area.getX(), area.getY(), area.getWidth(), area.getHeight(), radius, radius, true, true, false, false);
    g.fillPath(plate);
    // An engraved line under the plate: dark groove, light lip.
    g.setColour(juce::Colours::black.withAlpha(t.dark ? 0.5f : 0.18f));
    g.fillRect(area.getX() + 1.0f, area.getBottom() - 1.0f, area.getWidth() - 2.0f, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(t.dark ? 0.05f : 0.4f));
    g.fillRect(area.getX() + 1.0f, area.getBottom(), area.getWidth() - 2.0f, 1.0f);
}

void drawWell(juce::Graphics& g, juce::Rectangle<float> area) {
    const Theme& t = theme();
    g.setColour(t.display);
    g.fillRoundedRectangle(area, 4.0f);
    // One inset edge: the rim, and the shade the panel casts into the well from above.
    if (t.metal > 0.0f) {
        g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(0.5f * t.metal), area.getX(), area.getY(), juce::Colours::transparentBlack,
                                               area.getX(), area.getY() + 4.0f, false));
        g.fillRoundedRectangle(area.withHeight(std::min(4.0f, area.getHeight())), 4.0f);
    }
    g.setColour(t.dark ? juce::Colours::black.withAlpha(0.75f) : t.line);
    g.drawRoundedRectangle(area.reduced(0.5f), 4.0f, 1.0f);
}

void strokeGlow(juce::Graphics& g, const juce::Path& path, juce::Colour colour, float width) {
    const float glow = theme().glow;
    // Bevelled joints: on these dense polylines they look the same as round ones and cost a
    // fraction to build.
    if (glow > 0.0f) {
        g.setColour(colour.withAlpha(glow * 0.45f));
        g.strokePath(path, juce::PathStrokeType(width * 3.5f, juce::PathStrokeType::beveled, juce::PathStrokeType::rounded));
    }
    g.setColour(colour);
    g.strokePath(path, juce::PathStrokeType(width, juce::PathStrokeType::beveled, juce::PathStrokeType::rounded));
}

} // namespace zui

namespace zui {

int flowHeight(int width, const std::vector<FlowItem>& items, int rowHeight, int gap, const std::function<bool(const FlowItem&)>& shown) {
    int x = 0, y = 0, h = 0;
    bool rowEmpty = true;
    for (const auto& item : items) {
        if (item.c == nullptr || !shown(item))
            continue;
        const int ih = item.height > 0 ? item.height : rowHeight;
        if (!rowEmpty && (item.newRow || x + item.width > width + 1)) {
            y += h + gap;
            x = 0;
            h = 0;
        }
        rowEmpty = false;
        h = std::max(h, ih);
        x += item.width + gap;
    }
    return rowEmpty ? 0 : y + h;
}

int flow(juce::Rectangle<int> area, const std::vector<FlowItem>& items, int rowHeight, int gap) {
    // Rows first (which items share a row), then each row as tall as its tallest item, items
    // centred in it.
    std::vector<std::vector<const FlowItem*>> rows(1);
    int x = area.getX();
    for (const auto& item : items) {
        if (item.c == nullptr || !item.c->isVisible())
            continue;
        if (!rows.back().empty() && (item.newRow || x + item.width > area.getRight() + 1)) {
            rows.emplace_back();
            x = area.getX();
        }
        rows.back().push_back(&item);
        x += item.width + gap;
    }
    int y = area.getY();
    for (const auto& row : rows) {
        if (row.empty())
            continue;
        int h = 0;
        for (const auto* item : row)
            h = std::max(h, item->height > 0 ? item->height : rowHeight);
        x = area.getX();
        for (const auto* item : row) {
            const int ih = item->height > 0 ? item->height : rowHeight;
            item->c->setBounds(x, y + (h - ih) / 2, item->width, ih);
            x += item->width + gap;
        }
        y += h + gap;
    }
    return std::max(0, y - gap - area.getY());
}

} // namespace zui

namespace zui {
bool visibleWithin(const juce::Component& c, const juce::Component* root) {
    for (const juce::Component* p = &c; p != nullptr && p != root; p = p->getParentComponent())
        if (!p->isVisible())
            return false;
    return true;
}
} // namespace zui
