#include "FxPage.h"

#include "CurveEditor.h"
#include "Displays.h"

#include "zederia/patch/Modules.h"

#include <map>
#include <nlohmann/json.hpp>

namespace zui {

namespace {

const zed::Registry& reg() { return zed::Registry::get(); }
const char* const kLaneNames[3] = {"Lane A", "Lane B", "Master"};

juce::String slotId(int lane, int slot) { return "fx." + juce::String(lane == 0 ? "a" : lane == 1 ? "b" : "m") + juce::String(slot + 1); }

juce::String effectName(int type) { return type >= 0 ? str(reg().effectTypes()[static_cast<std::size_t>(type)].name) : juce::String("Empty"); }

// Shorter words for knob labels; the full name is in the help strip.
juce::String shortLabel(const juce::String& name) {
    juce::String s = name;
    for (const auto& [from, to] : std::initializer_list<std::pair<const char*, const char*>>{
             {"Frequency", "Freq"}, {"Threshold", "Thresh"}, {"Crossover", "Xover"}, {"Modulation", "Mod"}, {"Saturation", "Satur."},
             {"Feedback", "Fdbk"}, {"Resonance", "Res"}, {"Character", "Charact."}, {"Division", "Div"}, {"Position", "Pos"},
             {"Stereo Phase", "Phase"}, {"Pre-delay", "Pre-dly"}, {"Predelay", "Pre-dly"}, {"Release", "Rel"}, {"Attack", "Att"},
             {"Symmetry", "Symm"}, {"Offset", "Ofs"}, {"Amount", "Amt"}, {"Mono Below", "Mono <"}, {"Center", "Centre"}})
        s = s.replace(from, to);
    return s;
}

void addEffectsMenu(juce::PopupMenu& menu, const std::function<void(int)>& pick) {
    for (int t = 0; t < static_cast<int>(reg().effectTypes().size()); ++t)
        menu.addItem(effectName(t), [pick, t] { pick(t); });
}

// The Widener's mono-compatibility reading (plan 3.8).
class CorrelationMeter final : public Widget {
public:
    CorrelationMeter(Context& ctx, int lane, int slot) : ctx_(ctx), lane_(lane), slot_(slot) {
        setTitle("Mono compatibility");
        setHelp("Mono compatibility", "Correlation of left and right: +1 is mono, 0 unrelated, below 0 parts cancel when summed to mono.");
    }
    bool tick() override {
        const float v = ctx_.processor.engine().fxSlotStatus(lane_, slot_).meter;
        if (std::abs(v - value_) > 0.005f) {
            value_ = v;
            return true;
        }
        return false;
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        auto area = getLocalBounds().toFloat();
        drawWell(g, area);
        auto inner = area.reduced(12.0f);
        drawText(g, "MONO COMPATIBILITY", inner.removeFromTop(16.0f), kTextSmall, t.textDim, juce::Justification::centredLeft, true);
        const auto bar = inner.removeFromTop(14.0f);
        g.setColour(t.sunken);
        g.fillRoundedRectangle(bar, 3.0f);
        const float mid = bar.getCentreX();
        const float x = mid + bar.getWidth() * 0.5f * juce::jlimit(-1.0f, 1.0f, value_);
        g.setColour(value_ < 0.0f ? t.danger : t.ok);
        g.fillRect(juce::Rectangle<float>(std::min(mid, x), bar.getY(), std::abs(x - mid), bar.getHeight()));
        g.setColour(t.textDim);
        g.fillRect(mid, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f);
        auto labels = inner.removeFromTop(14.0f);
        drawText(g, "-1", labels.removeFromLeft(30.0f), kTextSmall, t.textDim);
        drawText(g, "+1", labels.removeFromRight(30.0f), kTextSmall, t.textDim, juce::Justification::centredRight);
        drawText(g, juce::String(value_, 2), labels, kTextSmall, t.text, juce::Justification::centred);
    }

private:
    Context& ctx_;
    int lane_, slot_;
    float value_ = 0.0f;
};

} // namespace

// --- A slot in a lane ----------------------------------------------------------------------------------

class SlotRow final : public Widget {
public:
    SlotRow(Context& ctx, FxPage& page, int lane, int slot, int type) : ctx_(ctx), page_(page), lane_(lane), slot_(slot), type_(type) {
        setWantsKeyboardFocus(true);
        keepsClickFocus = true; // Delete removes the slot, Alt with the arrows moves it
        family = Family::Effects;
        setTitle(juce::String(kLaneNames[lane]) + " slot " + juce::String(slot + 1) + ": " + effectName(type));
        setHelp(effectName(type), "Click to edit. Drag to reorder or move to another lane. Right-click for presets, copy, paste, replace and remove.");
    }
    bool tick() override {
        const auto status = ctx_.processor.engine().fxSlotStatus(lane_, slot_);
        const bool on = ctx_.processor.plainValue(reg().fxSlotOn(lane_, slot_)) >= 0.5;
        const double mix = ctx_.processor.plainValue(reg().fxSlotMix(lane_, slot_));
        const int state = (on ? 1 : 0) | (status.idle ? 2 : 0) | (status.running ? 4 : 0);
        if (state != state_ || mix != mix_) {
            state_ = state;
            mix_ = mix;
            return true;
        }
        return false;
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        auto area = getLocalBounds().toFloat().reduced(0.5f);
        const bool selected = page_.selectedLane() == lane_ && page_.selectedSlot() == slot_;
        g.setColour(selected ? t.selection : isMouseOver() ? t.raised.brighter(0.05f) : t.raised);
        g.fillRoundedRectangle(area, 4.0f);
        g.setColour(selected ? t.control.withAlpha(0.75f) : t.line); // the edited slot: a neutral edge
        g.drawRoundedRectangle(area, 4.0f, 1.0f);
        auto inner = area.reduced(6.0f, 0.0f);
        g.setColour(t.textDim);
        g.fillPath(glyphPath(Glyph::Grip, inner.removeFromLeft(8.0f).withSizeKeepingCentre(8.0f, 12.0f)));
        inner.removeFromLeft(6.0f);
        // The same power glyph as the strip's: a lit lamp when on, a hollow ring when bypassed.
        const bool on = (state_ & 1) != 0;
        const auto lamp = inner.removeFromLeft(12.0f).withSizeKeepingCentre(12.0f, 12.0f);
        if (on) {
            g.setColour((state_ & 2) ? t.lamp.withAlpha(0.6f) : t.lamp);
            g.fillEllipse(lamp);
            g.setColour(t.bg);
            g.strokePath(glyphPath(Glyph::Power, lamp.reduced(2.6f)), juce::PathStrokeType(1.2f));
        } else {
            g.setColour(t.textFaint);
            g.drawEllipse(lamp.reduced(0.6f), 1.1f);
            g.strokePath(glyphPath(Glyph::Power, lamp.reduced(2.8f)), juce::PathStrokeType(1.0f));
        }
        inner.removeFromLeft(6.0f);
        const juce::String where = juce::String(lane_ == 0 ? "A" : lane_ == 1 ? "B" : "M") + juce::String(slot_ + 1);
        drawText(g, where, inner.removeFromLeft(22.0f), kTextSmall, t.textDim);
        const juce::String mix = on ? juce::String(juce::roundToInt(mix_ * 100.0)) + " %" : juce::String("off");
        drawText(g, mix, inner.removeFromRight(40.0f), kTextSmall, t.textDim, juce::Justification::centredRight);
        const juce::String name = effectName(type_) + ((state_ & 2) && on ? "  idle" : "");
        drawText(g, name, inner, fittingSize(name, inner.getWidth(), kTextBody), on ? t.text : t.textDim);
        drawFocus(g, getLocalBounds().toFloat());
    }
    void mouseDown(const juce::MouseEvent& e) override {
        dragging_ = false;
        if (e.mods.isPopupMenu()) {
            showMenu();
            return;
        }
        page_.select(lane_, slot_);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (dragging_ || e.getDistanceFromDragStart() < 5)
            return;
        dragging_ = true;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
            container->startDragging("fxslot:" + juce::String(lane_) + ":" + juce::String(slot_), this);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        const auto mods = key.getModifiers();
        if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey) { page_.select(lane_, slot_); return true; }
        if (key == juce::KeyPress::deleteKey) { page_.removeEffect(lane_, slot_); return true; }
        if (mods.isAltDown() && key.getKeyCode() == juce::KeyPress::upKey && slot_ > 0) { page_.moveEffect(lane_, slot_, lane_, slot_ - 1); return true; }
        if (mods.isAltDown() && key.getKeyCode() == juce::KeyPress::downKey) { page_.moveEffect(lane_, slot_, lane_, slot_ + 1); return true; }
        if (key.getKeyCode() == juce::KeyPress::F10Key && mods.isShiftDown()) { showMenu(); return true; }
        return false;
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        juce::AccessibilityActions actions;
        actions.addAction(juce::AccessibilityActionType::press, [this] { page_.select(lane_, slot_); });
        actions.addAction(juce::AccessibilityActionType::showMenu, [this] { showMenu(); });
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::listItem, std::move(actions));
    }
    void showMenu() {
        juce::PopupMenu menu;
        const int lane = lane_, slot = slot_;
        FxPage* page = &page_;
        menu.addItem("Edit", [page, lane, slot] { page->select(lane, slot); });
        if (slot > 0)
            menu.addItem("Move up", [page, lane, slot] { page->moveEffect(lane, slot, lane, slot - 1); });
        if (slot + 1 < page->slotCount(lane))
            menu.addItem("Move down", [page, lane, slot] { page->moveEffect(lane, slot, lane, slot + 1); });
        juce::PopupMenu to;
        for (int l = 0; l < 3; ++l)
            if (l != lane)
                to.addItem(kLaneNames[l], page->slotCount(l) < zed::kNumFxSlots, false, [page, lane, slot, l] { page->moveEffect(lane, slot, l, page->slotCount(l)); });
        menu.addSubMenu("Move to", to);
        juce::PopupMenu replace;
        addEffectsMenu(replace, [page, lane, slot](int t) {
            page->removeEffect(lane, slot);
            page->insertEffect(lane, slot, t);
        });
        menu.addSubMenu("Replace with", replace);
        menu.addSeparator();
        Context* ctx = &ctx_;
        const juce::String id = slotId(lane, slot);
        menu.addItem("Copy effect", [ctx, id] { ctx->session.copyModule(id); });
        menu.addItem("Paste effect here", ctx->session.clipboardClass() == "fx_slot", false, [ctx, id] {
            juce::String error;
            if (!ctx->session.pasteModule(id, error) && ctx->notify) ctx->notify(error);
        });
        menu.addItem("Remove", [page, lane, slot] { page->removeEffect(lane, slot); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
    }
    int lane() const { return lane_; }
    int slot() const { return slot_; }

private:
    Context& ctx_;
    FxPage& page_;
    int lane_, slot_, type_;
    int state_ = -1;
    double mix_ = -1.0;
    bool dragging_ = false;
};

// --- An effect in the rack: one strip with its controls in named groups ------------------------------------

namespace {

struct StripItem {
    const char* id;
    const char* label;
};
struct StripGroup {
    const char* caption;
    std::vector<StripItem> items;
};

// The controls of each effect, grouped the way they are used: the main character first, then
// tone and time, then what shapes the tail. Any parameter not listed here joins a last group, so
// a new parameter is never unreachable.
const std::vector<StripGroup>& groupsFor(const std::string& id) {
    static const std::map<std::string, std::vector<StripGroup>> groups = {
        {"drive", {{"Drive", {{"mode", "Mode"}, {"amount", "Amount"}, {"level", "Level"}}},
                   {"Filter", {{"filter_position", "Filter"}, {"filter_morph", "Morph"}, {"filter_freq", "Freq"}, {"filter_q", "Q"}}},
                   {"Curves", {{"curve_morph", "Morph"}, {"curve_symmetry", "Symmetry"}}}}},
        {"compressor", {{"Mode", {{"mode", "Mode"}}},
                        {"Dynamics", {{"threshold", "Thresh"}, {"ratio", "Ratio"}, {"attack", "Attack"}, {"release", "Release"}, {"makeup", "Make-up"}}},
                        {"Bands", {{"upward", "Upward"}, {"low_threshold", "Lo thr"}, {"low_amount", "Lo amt"}, {"mid_threshold", "Mid thr"},
                                   {"mid_amount", "Mid amt"}, {"high_threshold", "Hi thr"}, {"high_amount", "Hi amt"}, {"crossover_low", "Lo X"},
                                   {"crossover_high", "Hi X"}}}}},
        {"eq", {{"Low", {{"low_type", "Type"}, {"low_freq", "Freq"}, {"low_gain", "Gain"}, {"low_q", "Q"}}},
                {"Mid 1", {{"mid1_freq", "Freq"}, {"mid1_gain", "Gain"}, {"mid1_q", "Q"}}},
                {"Mid 2", {{"mid2_freq", "Freq"}, {"mid2_gain", "Gain"}, {"mid2_q", "Q"}}},
                {"High", {{"high_type", "Type"}, {"high_freq", "Freq"}, {"high_gain", "Gain"}, {"high_q", "Q"}}}}},
        {"filter", {{"Filter", {{"filter_type", "Type"}, {"cutoff", "Cutoff"}, {"resonance", "Res"}}},
                    {"Colour", {{"drive", "Drive"}, {"character", "Character"}, {"stereo_offset", "Stereo"}}}}},
        {"chorus", {{"Motion", {{"voices", "Voices"}, {"sync", "Sync"}, {"rate", "Rate"}, {"division", "Division"}, {"depth", "Depth"}}},
                    {"Delays", {{"delay1", "Delay 1"}, {"delay2", "Delay 2"}, {"feedback", "Feedback"}}},
                    {"Wet filter", {{"filter_type", "Type"}, {"filter_cutoff", "Cutoff"}}}}},
        {"flanger", {{"Motion", {{"sync", "Sync"}, {"rate", "Rate"}, {"division", "Division"}, {"depth", "Depth"}}},
                     {"Tone", {{"feedback", "Feedback"}, {"stereo_phase", "Phase"}}}}},
        {"phaser", {{"Motion", {{"sync", "Sync"}, {"rate", "Rate"}, {"division", "Division"}, {"depth", "Depth"}}},
                    {"Tone", {{"stages", "Stages"}, {"center", "Centre"}, {"feedback", "Feedback"}, {"stereo_phase", "Phase"}}}}},
        {"delay", {{"Delay", {{"mode", "Mode"}, {"sync", "Sync"}, {"link", "Link"}}},
                   {"Left", {{"time_l", "Time"}, {"division_l", "Division"}, {"offset_l", "Offset"}}},
                   {"Right", {{"time_r", "Time"}, {"division_r", "Division"}, {"offset_r", "Offset"}}},
                   {"Feedback", {{"feedback", "Amount"}, {"band_center", "Centre"}, {"band_width", "Width"}, {"saturation", "Satur."}}},
                   {"Movement", {{"modulation", "Mod"}, {"ducking", "Ducking"}}}}},
        {"reverb", {{"Space", {{"mode", "Mode"}, {"size", "Size"}, {"decay", "Decay"}, {"predelay", "Pre-delay"}}},
                    {"Tone", {{"low_cut", "Low cut"}, {"high_cut", "High cut"}, {"damping", "Damping"}}},
                    {"Tail", {{"modulation", "Mod"}, {"width", "Width"}, {"freeze", "Freeze"}}}}},
        {"swarm", {{"Swarm", {{"voices", "Voices"}, {"rate", "Rate"}, {"detune", "Detune"}, {"restart", "Restart"}}}}},
        {"widener", {{"Width", {{"size", "Size"}}}}},
        {"utility", {{"Level", {{"gain", "Gain"}, {"pan", "Pan"}}},
                     {"Stereo", {{"width", "Width"}, {"mono_below", "Mono <"}, {"polarity", "Polarity"}}},
                     {"Clean", {{"dc_filter", "DC filter"}}}}},
        {"limiter", {{"Limiter", {{"ceiling", "Ceiling"}, {"release", "Release"}}}}},
    };
    static const std::vector<StripGroup> none;
    const auto it = groups.find(id);
    return it != groups.end() ? it->second : none;
}

int paramIndexOf(const zed::EffectType& type, const std::string& id) {
    for (int k = 0; k < static_cast<int>(type.params.size()); ++k)
        if (const auto& p = type.params[static_cast<std::size_t>(k)]; p.pointer == id || p.id.substr(p.id.rfind('.') + 1) == id)
            return k;
    return -1;
}

void showSlotMenu(Context& ctx, FxPage& page, int lane, int slot, juce::Component* target) {
    juce::PopupMenu menu;
    FxPage* p = &page;
    Context* c = &ctx;
    if (slot > 0)
        menu.addItem("Move up", [p, lane, slot] { p->moveEffect(lane, slot, lane, slot - 1); });
    if (slot + 1 < page.slotCount(lane))
        menu.addItem("Move down", [p, lane, slot] { p->moveEffect(lane, slot, lane, slot + 1); });
    juce::PopupMenu to;
    for (int l = 0; l < 3; ++l)
        if (l != lane)
            to.addItem(kLaneNames[l], page.slotCount(l) < zed::kNumFxSlots, false, [p, lane, slot, l] { p->moveEffect(lane, slot, l, p->slotCount(l)); });
    menu.addSubMenu("Move to", to);
    juce::PopupMenu replace;
    addEffectsMenu(replace, [p, lane, slot](int t) {
        p->removeEffect(lane, slot);
        p->insertEffect(lane, slot, t);
    });
    menu.addSubMenu("Replace with", replace);
    menu.addSeparator();
    // The file dialogs outlive the menu and may outlive the editor: their callbacks check the page.
    const juce::Component::SafePointer<FxPage> safe(&page);
    menu.addItem("Save effect preset...", [c, lane, slot, safe] {
        const juce::File folder = c->prefs.userContentFolder().getChildFile("Racks").getChildFile("Effects");
        folder.createDirectory();
        auto chooser = std::make_shared<juce::FileChooser>("Save effect preset", folder.getChildFile("Effect.zfx"), "*.zfx");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [c, lane, slot, chooser, safe](const juce::FileChooser& fc) {
            if (safe != nullptr && fc.getResult() != juce::File())
                fc.getResult().replaceWithText(str(c->processor.slotPresetJson(lane, slot)));
        });
    });
    menu.addItem("Load effect preset...", [c, p, lane, slot, safe] {
        auto chooser = std::make_shared<juce::FileChooser>("Load effect preset", c->prefs.userContentFolder().getChildFile("Racks"), "*.zfx");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [c, p, lane, slot, chooser, safe](const juce::FileChooser& fc) {
            if (safe == nullptr || !fc.getResult().existsAsFile())
                return;
            const std::string json = fc.getResult().loadFileAsString().toStdString();
            std::vector<zed::Issue> issues;
            c->session.perform("Load effect preset " + fc.getResult().getFileNameWithoutExtension(), [&] {
                return c->processor.loadSlotPresetJson(json, lane, slot, issues);
            });
            if (!issues.empty() && c->notify)
                c->notify(str(issues.front().message));
            p->select(lane, slot);
        });
    });
    const juce::String id = slotId(lane, slot);
    menu.addItem("Copy effect", [c, id] { c->session.copyModule(id); });
    menu.addItem("Paste effect here", ctx.session.clipboardClass() == "fx_slot", false, [c, id] {
        juce::String error;
        if (!c->session.pasteModule(id, error) && c->notify) c->notify(error);
    });
    menu.addSeparator();
    menu.addItem("Remove", [p, lane, slot] { p->removeEffect(lane, slot); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target));
}

} // namespace

class EffectStrip final : public Widget {
public:
    EffectStrip(Context& ctx, FxPage& page, int lane, int slot) : ctx_(ctx), page_(page), lane_(lane), slot_(slot), type_(page.slotType(lane, slot)) {
        family = Family::Effects;
        setTitle(juce::String(kLaneNames[lane]) + " " + juce::String(slot + 1) + ": " + effectName(type_));
        setOpaque(true);
        setHelp(effectName(type_), "Click to select. Right-click (or the menu button) for presets, copy, paste, moving and removing.");
        auto& power = parts_.add<Toggle>(*this, ctx, reg().fxSlotOn(lane, slot), "On");
        power.power = true;
        power.family = Family::Effects;
        power.labelledBy = effectName(type_);
        power_ = &power;
        mix_ = &parts_.add<Knob>(*this, ctx, reg().fxSlotMix(lane, slot), "Mix");
        mix_->family = Family::Effects;
        menu_ = &parts_.add<Button>(*this, "", Glyph::Menu);
        remove_ = &parts_.add<Button>(*this, "", Glyph::Trash);
        menu_->setTitle("Effect menu");
        remove_->setTitle("Remove " + effectName(type_));
        menu_->setTooltip("Presets, copy, paste, move");
        remove_->setTooltip("Remove");
        menu_->ghost = remove_->ghost = true;
        menu_->onClick = [this] { showSlotMenu(ctx_, page_, lane_, slot_, menu_); };
        remove_->onClick = [this] { page_.removeEffect(lane_, slot_); };
        if (type_ < 0)
            return;
        const auto& type = reg().effectTypes()[static_cast<std::size_t>(type_)];
        const auto widgetFor = [&](int k, const juce::String& label) -> std::pair<Widget*, juce::Point<int>> {
            const auto& d = type.params[static_cast<std::size_t>(k)];
            const int index = reg().fxSlotParam(lane, slot, k);
            if (d.type == zed::ParamType::Choice) {
                auto& c = parts_.add<ChoiceBox>(*this, ctx_, index, label);
                c.family = Family::Effects;
                if (d.pointer == "filter_type" && d.choices.size() > 8)
                    c.groupOf = [](const juce::String& t) { return t.upToFirstOccurrenceOf("_", false, false).toUpperCase(); };
                return {&c, {d.choices.size() > 8 ? 132 : 90, kChoiceH}};
            }
            if (d.type == zed::ParamType::Bool) {
                auto& tg = parts_.add<Toggle>(*this, ctx_, index, label);
                tg.family = Family::Effects;
                return {&tg, {static_cast<int>(textWidth(label, kTextSmall)) + 40, 22}};
            }
            if (d.type == zed::ParamType::Int) {
                auto& nb = parts_.add<NumberBox>(*this, ctx_, index, label);
                nb.family = Family::Effects;
                return {&nb, {56, kChoiceH}};
            }
            auto& kn = parts_.add<Knob>(*this, ctx_, index, label);
            kn.family = Family::Effects;
            return {&kn, {kKnobW, kKnobH}};
        };
        std::vector<bool> used(type.params.size(), false);
        const juce::String id = str(type.id);
        for (const auto& spec : groupsFor(type.id)) {
            Group group;
            group.caption = spec.caption;
            for (const auto& item : spec.items) {
                const int k = paramIndexOf(type, item.id);
                if (k < 0 || used[static_cast<std::size_t>(k)])
                    continue;
                used[static_cast<std::size_t>(k)] = true;
                auto [w, size] = widgetFor(k, item.label);
                group.items.push_back({w, size, juce::String(item.id)});
            }
            if (!group.items.empty())
                groups_.push_back(std::move(group));
        }
        Group rest; // whatever the table above does not name
        rest.caption = "More";
        for (int k = 0; k < static_cast<int>(type.params.size()); ++k)
            if (!used[static_cast<std::size_t>(k)]) {
                auto [w, size] = widgetFor(k, shortLabel(str(type.params[static_cast<std::size_t>(k)].name)));
                rest.items.push_back({w, size, str(type.params[static_cast<std::size_t>(k)].pointer)});
            }
        if (!rest.items.empty())
            groups_.push_back(std::move(rest));
        // A display where one helps: the response of EQ and Filter, Drive's curves, the Widener's correlation.
        if (id == "eq" || id == "filter") {
            auto& spectrum = parts_.add<SpectrumView>(*this, ctx_);
            spectrum.curve = [this](const std::vector<double>& hz, std::vector<float>& db) { return ctx_.processor.engine().fxResponse(lane_, slot_, hz, db); };
            display_ = &spectrum;
        } else if (id == "widener") {
            display_ = &parts_.add<CorrelationMeter>(*this, ctx_, lane, slot);
        } else if (id == "drive") {
            for (int c = 0; c < 2; ++c) {
                auto& editor = parts_.add<CurveEditor>(*this, ctx_, c == 0 ? "Drive curve A" : "Drive curve B");
                CurveEditor::Config config;
                config.bipolar = false;
                config.fixedEnds = true;
                editor.setConfig(config);
                editor.family = Family::Effects;
                editor.onChange = [this, c](const zed::Shape& s) { ctx_.processor.setFxCurveLive(lane_, slot_, c, s); };
                editor.onCommit = [this](const zed::Shape&, const juce::String& label) { ctx_.session.commit(label); };
                editor.onCopy = [this, &editor] { ctx_.session.copyText(str(zed::copyShape(editor.shape(), zed::ShapeKind::Curve))); };
                editor.onPaste = [this, &editor, c] {
                    zed::Shape shape;
                    std::vector<zed::Issue> issues;
                    if (zed::pasteShape(ctx_.session.clipboardText().toStdString(), zed::ShapeKind::Curve, shape, issues)) {
                        ctx_.session.edit("Paste drive curve", [&](zed::Patch& p) {
                            p.fxCurves[static_cast<std::size_t>(lane_)][static_cast<std::size_t>(slot_)][static_cast<std::size_t>(c)] = shape;
                            return true;
                        });
                        editor.setShape(shape);
                    }
                };
                curves_[c] = &editor;
            }
            reloadCurves();
        }
        state_ = conditions();
    }

    int lane() const { return lane_; }
    int slot() const { return slot_; }
    int type() const { return type_; }

    void reloadCurves() {
        if (curves_[0] == nullptr)
            return;
        ctx_.processor.readDocument([&](const zed::Patch& p) {
            for (int c = 0; c < 2; ++c)
                curves_[c]->setShape(p.fxCurves[static_cast<std::size_t>(lane_)][static_cast<std::size_t>(slot_)][static_cast<std::size_t>(c)]);
        });
    }

    // Lays the strip out at a width and returns the height it needs.
    int layoutAt(int width) {
        applyConditions();
        constexpr int kPlate = 122, kMix = 62, kDisplay = 236, kRow = 80, kPad = 10, kGroupGap = 18;
        const bool display = displayShown();
        int x0 = kPlate + kPad;
        int right = width - kPad - kMix;
        if (display)
            right -= kDisplay + 10;
        // Groups left to right, wrapping to a second row when they do not fit.
        int x = x0, row = 0;
        captions_.clear();
        dividers_.clear();
        for (auto& g : groups_) {
            int w = 0;
            for (const auto& item : g.items)
                if (item.widget->isVisible())
                    w += item.size.x + 4;
            if (w == 0)
                continue;
            w -= 4;
            if (x > x0 && x + w > right) {
                ++row;
                x = x0;
            }
            if (x > x0)
                dividers_.push_back(juce::Rectangle<int>(x - kGroupGap / 2, kPad + row * kRow + 4, 1, kRow - 8));
            const int top = kPad + row * kRow;
            captions_.push_back({juce::Rectangle<int>(x, top, w, 14), g.caption});
            int ix = x;
            for (auto& item : g.items) {
                if (!item.widget->isVisible())
                    continue;
                item.widget->setBounds(ix, top + 14 + (kKnobH - item.size.y) / 2, item.size.x, item.size.y);
                ix += item.size.x + 4;
            }
            x += w + kGroupGap;
        }
        const int rows = std::max(row + 1, display ? 2 : 1);
        const int height = kPad * 2 + rows * kRow - 4;
        // The name plate: power, then the menu and remove buttons along its foot.
        power_->setBounds(10, 10, 20, 20);
        menu_->setBounds(10, height - 30, 24, 22);
        remove_->setBounds(38, height - 30, 24, 22);
        mix_->setBounds(width - kPad - kMix, (height - kKnobH) / 2, kMix, kKnobH);
        const auto displayArea = juce::Rectangle<int>(width - kPad - kMix - 10 - kDisplay, kPad, kDisplay, height - kPad * 2);
        if (display_ != nullptr) {
            display_->setVisible(display && !curveModes());
            display_->setBounds(displayArea);
        }
        if (curves_[0] != nullptr) { // Custom curve draws with curve A alone; Dual curve morphs from A to B
            const bool dual = param("mode") >= 14.0;
            curves_[0]->setVisible(curveModes());
            curves_[1]->setVisible(curveModes() && dual);
            auto area = displayArea;
            curves_[0]->setBounds(dual ? area.removeFromLeft((displayArea.getWidth() - 6) / 2) : area);
            area.removeFromLeft(6);
            curves_[1]->setBounds(area);
        }
        return height;
    }

    // A switch that changes what shows (sync, link, a mode) asks the page to lay out again.
    bool tickStrip() {
        const auto now = conditions();
        if (now == state_)
            return false;
        state_ = now;
        return true;
    }

    bool tick() override {
        const auto status = ctx_.processor.engine().fxSlotStatus(lane_, slot_);
        const bool idle = status.idle;
        const bool on = ctx_.processor.plainValue(reg().fxSlotOn(lane_, slot_)) >= 0.5;
        if (idle != idle_ || on != onShown_) {
            idle_ = idle;
            onShown_ = on;
            return true;
        }
        return false;
    }
    juce::RectangleList<int> dirtyArea() override { return juce::Rectangle<int>(0, 0, 122, getHeight()); }

    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        const auto area = getLocalBounds().toFloat();
        background_.draw(g, getLocalBounds(), 0, true, [&](juce::Graphics& bg) {
            bg.fillAll(t.bg);
            drawPanel(bg, area, t.panel);
            // The name plate: the effect's name, its address in the rack and its power.
            drawPlate(bg, area.withWidth(122.0f).reduced(3.0f), t.radius - 1.0f);
        });
        const bool on = ctx_.processor.plainValue(reg().fxSlotOn(lane_, slot_)) >= 0.5;
        drawText(g, effectName(type_).toUpperCase(), juce::Rectangle<float>(36.0f, 10.0f, 82.0f, 20.0f), fittingSize(effectName(type_).toUpperCase(), 82.0f, kTextBody, true), on ? t.text : t.textDim,
                 juce::Justification::centredLeft, true);
        juce::String where = juce::String(lane_ == 0 ? "A" : lane_ == 1 ? "B" : "M") + juce::String(slot_ + 1);
        if (!on)
            where << "  bypassed";
        else if (idle_)
            where << "  idle";
        drawText(g, where, juce::Rectangle<float>(14.0f, 34.0f, 100.0f, 14.0f), kTextSmall, t.textDim);
        // Group names and the grooves between groups.
        for (const auto& [box, caption] : captions_)
            drawText(g, caption.toUpperCase(), box.toFloat(), kTextSmall, t.textFaint, juce::Justification::centredLeft, true);
        for (const auto& d : dividers_) {
            g.setColour(juce::Colours::black.withAlpha(t.dark ? 0.45f : 0.15f));
            g.fillRect(d);
            g.setColour(juce::Colours::white.withAlpha(t.dark ? 0.05f : 0.4f));
            g.fillRect(d.translated(1, 0));
        }
        if (page_.selectedLane() == lane_ && page_.selectedSlot() == slot_) { // the slot being edited: a neutral edge
            g.setColour(t.control.withAlpha(0.7f));
            g.drawRoundedRectangle(area.reduced(0.5f), t.radius, 1.4f);
        }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (e.mods.isPopupMenu()) {
            showSlotMenu(ctx_, page_, lane_, slot_, this);
            return;
        }
        page_.select(lane_, slot_);
    }

private:
    struct Item {
        Widget* widget = nullptr;
        juce::Point<int> size;
        juce::String id;
    };
    struct Group {
        juce::String caption;
        std::vector<Item> items;
    };
    double param(const char* id) const {
        if (type_ < 0)
            return 0.0;
        const int k = paramIndexOf(reg().effectTypes()[static_cast<std::size_t>(type_)], id);
        return k >= 0 ? ctx_.processor.plainValue(reg().fxSlotParam(lane_, slot_, k)) : 0.0;
    }
    bool curveModes() const { return curves_[0] != nullptr && param("mode") >= 13.0; }
    bool displayShown() const { return display_ != nullptr || curveModes(); }
    // What decides which controls show: sync, link, the compressor's and Drive's modes, Drive's filter.
    std::array<int, 5> conditions() const {
        return {static_cast<int>(param("sync") >= 0.5), static_cast<int>(param("link") >= 0.5), static_cast<int>(param("mode")),
                static_cast<int>(param("filter_position")), 0};
    }
    void applyConditions() {
        if (type_ < 0)
            return;
        const juce::String id = str(reg().effectTypes()[static_cast<std::size_t>(type_)].id);
        const bool sync = param("sync") >= 0.5, link = param("link") >= 0.5;
        const bool simple = ctx_.simpleView();
        for (auto& g : groups_)
            for (auto& item : g.items) {
                bool show = true;
                if (item.id == "rate" && id != "swarm") show = !sync;
                if (item.id == "division") show = sync;
                if (item.id == "time_l" || item.id == "time_r") show = !sync;
                if (item.id.startsWith("division_") || item.id.startsWith("offset_")) show = sync;
                if (id == "delay" && item.id.endsWith("_r")) show = show && !link;
                if (id == "compressor" && g.caption == "Bands") show = param("mode") >= 0.5;
                if (id == "drive" && g.caption == "Curves") show = item.id == "curve_morph" ? param("mode") >= 14.0 : param("mode") >= 13.0;
                if (id == "drive" && (item.id == "filter_morph" || item.id == "filter_freq" || item.id == "filter_q")) show = param("filter_position") >= 0.5;
                item.widget->setVisible(show && shownInView(*item.widget, simple));
            }
    }

    Context& ctx_;
    FxPage& page_;
    int lane_, slot_, type_;
    Parts parts_;
    std::vector<Group> groups_;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> captions_;
    std::vector<juce::Rectangle<int>> dividers_;
    Toggle* power_ = nullptr;
    Knob* mix_ = nullptr;
    Button *menu_ = nullptr, *remove_ = nullptr;
    Widget* display_ = nullptr;
    CurveEditor* curves_[2] = {nullptr, nullptr};
    std::array<int, 5> state_{};
    bool idle_ = false;
    bool onShown_ = true;
    CachedDrawing background_;
};

// --- The chain: the selected lane's effects as a list -----------------------------------------------

class LaneTab final : public Button, public juce::DragAndDropTarget {
public:
    LaneTab(FxPage& page, int lane) : Button(lane == 2 ? "Master" : lane == 0 ? "Lane A" : "Lane B"), page_(page), lane_(lane) {
        toggleRole = true;
        textSize = kTextSmall + 0.5f;
        setHelp(getTitle(), "Show this lane's effects. Drop an effect here to move it to the end of this lane.");
    }
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("fxslot:"); }
    void itemDropped(const SourceDetails& d) override {
        const auto parts = juce::StringArray::fromTokens(d.description.toString(), ":", "");
        const int fromLane = parts[1].getIntValue(), fromSlot = parts[2].getIntValue();
        if (fromLane != lane_)
            page_.moveEffect(fromLane, fromSlot, lane_, page_.slotCount(lane_));
    }

private:
    FxPage& page_;
    int lane_;
};

class LaneColumn final : public Card, public juce::DragAndDropTarget {
public:
    LaneColumn(Context& ctx, FxPage& page) : Card("Chain", Family::Effects), ctx_(ctx), page_(page) {
        screws = false;
        for (int l = 0; l < 3; ++l) {
            tabs_.push_back(std::make_unique<LaneTab>(page, l));
            tabs_.back()->onClick = [this, l] { page_.select(l, 0); };
            addAndMakeVisible(*tabs_.back());
        }
        add_.textSize = kTextSmall + 0.5f;
        add_.setHelp("Add effect", "Any effect, any number of times; up to eight per lane.");
        add_.onClick = [this] {
            juce::PopupMenu menu;
            addEffectsMenu(menu, [this](int t) { page_.insertEffect(lane_, page_.slotCount(lane_), t); });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_));
        };
        addAndMakeVisible(add_);
        routing_ = std::make_unique<Segmented>(ctx, "fx.b_routing", "Lane B");
        routing_->names = {"B parallel", "B in series"};
        routing_->showLabel = false;
        routing_->labelledBy = "Lane B";
        routing_->family = Family::Effects;
        routing_->setHelp("Lane B", "Parallel: lane B runs beside lane A. In series: after it.");
        addAndMakeVisible(*routing_);
    }
    void setLane(int lane) {
        lane_ = lane;
        for (int l = 0; l < 3; ++l)
            tabs_[static_cast<std::size_t>(l)]->setToggled(l == lane);
        rebuild();
    }
    int lane() const { return lane_; }
    void rebuild() {
        rows_.clear();
        for (int s = 0; s < page_.slotCount(lane_); ++s) {
            rows_.push_back(std::make_unique<SlotRow>(ctx_, page_, lane_, s, page_.slotType(lane_, s)));
            addAndMakeVisible(*rows_.back());
        }
        add_.setEnabled(page_.slotCount(lane_) < zed::kNumFxSlots);
        add_.setText("Add effect to " + juce::String(kLaneNames[lane_]));
        resized();
        repaint();
    }
    void layoutBody(juce::Rectangle<int> body) override {
        auto tabs = body.removeFromTop(24);
        const int w = (tabs.getWidth() - 8) / 3;
        for (auto& t : tabs_) {
            t->setBounds(tabs.removeFromLeft(w));
            tabs.removeFromLeft(4);
        }
        body.removeFromTop(6);
        routing_->setBounds(body.removeFromTop(22));
        body.removeFromTop(8);
        listTop_ = body.getY();
        for (auto& r : rows_) {
            r->setBounds(body.removeFromTop(kRowH - 3));
            body.removeFromTop(3);
        }
        add_.setBounds(body.removeFromTop(26));
    }
    void paintOverChildren(juce::Graphics& g) override {
        if (dropAt_ < 0)
            return;
        const float y = static_cast<float>(listTop_ + dropAt_ * kRowH) - 2.0f;
        g.setColour(theme().focus);
        g.fillRoundedRectangle(8.0f, y - 1.0f, static_cast<float>(getWidth() - 16), 3.0f, 1.5f);
    }
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("fxslot:"); }
    void itemDragMove(const SourceDetails& d) override {
        const int at = juce::jlimit(0, page_.slotCount(lane_), (d.localPosition.y - listTop_ + kRowH / 2) / kRowH);
        if (at != dropAt_) {
            dropAt_ = at;
            repaint();
        }
    }
    void itemDragExit(const SourceDetails&) override {
        dropAt_ = -1;
        repaint();
    }
    void itemDropped(const SourceDetails& d) override {
        const auto parts = juce::StringArray::fromTokens(d.description.toString(), ":", "");
        const int fromLane = parts[1].getIntValue(), fromSlot = parts[2].getIntValue();
        int to = dropAt_ < 0 ? page_.slotCount(lane_) : dropAt_;
        dropAt_ = -1;
        if (fromLane == lane_ && to > fromSlot)
            --to; // the slot leaves its old place first
        if (fromLane != lane_ || to != fromSlot)
            page_.moveEffect(fromLane, fromSlot, lane_, to);
        repaint();
    }

private:
    static constexpr int kRowH = 29;
    Context& ctx_;
    FxPage& page_;
    int lane_ = 0, listTop_ = 0, dropAt_ = -1;
    std::vector<std::unique_ptr<LaneTab>> tabs_;
    Button add_{"Add effect", Glyph::Plus};
    std::unique_ptr<Segmented> routing_;
    std::vector<std::unique_ptr<SlotRow>> rows_;
};

// The first empty position of the lane in the rack: a quiet bay with the add action where the
// next effect will go (the chain's own menu of effects).
class AddBay final : public juce::Component {
public:
    explicit AddBay(FxPage& page) : page_(page) {
        setTitle("Empty rack position");
        add_.textSize = kTextSmall + 0.5f;
        add_.setHelp("Add effect", "Puts an effect at this position of the lane; any effect, any number of times, up to eight per lane.");
        add_.onClick = [this] {
            juce::PopupMenu menu;
            const int lane = lane_, at = page_.slotCount(lane_);
            FxPage* p = &page_;
            addEffectsMenu(menu, [p, lane, at](int t) { p->insertEffect(lane, at, t); });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_));
        };
        addAndMakeVisible(add_);
    }
    void setLane(int lane, int position) {
        lane_ = lane;
        position_ = position;
        add_.setText("Add effect to " + juce::String(kLaneNames[lane]));
        add_.setTitle("Add an effect at " + address() + " of " + juce::String(kLaneNames[lane]));
        repaint();
    }
    juce::String address() const { return juce::String(lane_ == 0 ? "A" : lane_ == 1 ? "B" : "M") + juce::String(position_ + 1); }
    Button& addButton() { return add_; }
    void resized() override { add_.setBounds(getLocalBounds().reduced(14, 0).withSizeKeepingCentre(200, 28).withX(122 + 10)); }
    void paint(juce::Graphics& g) override {
        const Theme& t = theme();
        juce::Path outline, dashed;
        outline.addRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), t.radius);
        const float dashes[] = {4.0f, 4.0f};
        juce::PathStrokeType(1.0f).createDashedStroke(dashed, outline, dashes, 2);
        g.setColour(t.lineStrong);
        g.fillPath(dashed);
        drawText(g, address(), juce::Rectangle<float>(14.0f, 0.0f, 100.0f, static_cast<float>(getHeight())), kTextSmall, t.textDim);
    }

private:
    FxPage& page_;
    int lane_ = 0, position_ = 0;
    Button add_{"Add effect", Glyph::Plus};
};

// The rack: the selected lane's effects as strips, top to bottom in signal order, and the bay of
// its first empty position. The rest of the rack is plain enclosure.
class SlotDetail final : public juce::Component {
public:
    SlotDetail(Context& ctx, FxPage& page) : ctx_(ctx), page_(page), bay_(page) {
        setTitle("Rack");
        viewport_.setViewedComponent(&content_, false);
        viewport_.setTitle("Effects of the lane");
        content_.setTitle("Effect strips");
        viewport_.setScrollBarsShown(true, false);
        viewport_.setScrollBarThickness(10);
        content_.addChildComponent(bay_);
        addAndMakeVisible(viewport_);
    }
    AddBay& bay() { return bay_; }
    void show(int lane, int slot) {
        if (lane != lane_ || strips_.empty() != (page_.slotCount(lane) == 0) || static_cast<int>(strips_.size()) != page_.slotCount(lane) || stale_) {
            lane_ = lane;
            stale_ = false;
            strips_.clear();
            for (int s = 0; s < page_.slotCount(lane); ++s) {
                strips_.push_back(std::make_unique<EffectStrip>(ctx_, page_, lane, s));
                content_.addAndMakeVisible(*strips_.back());
            }
            layoutStrips();
        }
        for (auto& s : strips_)
            s->repaint();
        if (slot >= 0 && slot < static_cast<int>(strips_.size())) {
            const auto b = strips_[static_cast<std::size_t>(slot)]->getBounds();
            if (b.getY() < viewport_.getViewPositionY() || b.getBottom() > viewport_.getViewPositionY() + viewport_.getViewHeight())
                viewport_.setViewPosition(0, b.getY() - 4);
        }
    }
    void markStale() { stale_ = true; }
    void reloadCurves() {
        for (auto& s : strips_)
            s->reloadCurves();
    }
    bool tickDetail() {
        bool relayout = false;
        for (auto& s : strips_)
            relayout = s->tickStrip() || relayout;
        if (relayout)
            layoutStrips();
        return relayout;
    }
    void resized() override {
        viewport_.setBounds(getLocalBounds());
        layoutStrips();
    }
    void paint(juce::Graphics&) override {} // the chassis shows through: an empty bay recedes
    int strips() const { return static_cast<int>(strips_.size()); }

private:
    void layoutStrips() {
        const int width = viewport_.getWidth() - viewport_.getScrollBarThickness() - 2;
        int y = 0;
        for (auto& s : strips_) {
            const int h = s->layoutAt(width);
            s->setBounds(0, y, width, h);
            y += h + 8;
        }
        const int count = static_cast<int>(strips_.size());
        const bool room = lane_ >= 0 && count < zed::kNumFxSlots;
        bay_.setVisible(room);
        if (room) {
            bay_.setLane(lane_, count);
            bay_.setBounds(0, y, width, 52);
            y += 52 + 8;
        }
        content_.setSize(juce::jmax(1, width), juce::jmax(1, y - 8));
        repaint();
    }
    Context& ctx_;
    FxPage& page_;
    juce::Viewport viewport_;
    juce::Component content_;
    AddBay bay_;
    std::vector<std::unique_ptr<EffectStrip>> strips_;
    int lane_ = -1;
    bool stale_ = true;
};

// --- Rack card: balances, FX off, lock, rack presets ----------------------------------------------------

class RackCard final : public Card {
public:
    RackCard(Context& ctx) : Card("Rack", Family::Effects), ctx_(ctx) {
        moduleId = "fx";
        for (const auto& [id, label] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"fx.filter1_balance", "Filter 1 to B"}, {"fx.filter2_balance", "Filter 2 to B"}, {"fx.direct_balance", "Direct to B"}}) {
            auto& f = parts_.add<Fader>(*this, ctx, id, label);
            f.labelWidth = 84;
            f.family = Family::Effects;
            faders_.push_back(&f);
        }
        fxOff_ = &parts_.add<Toggle>(*this, ctx, "global.fx_off", "FX off");
        for (Button* b : {&lock_, &save_, &load_, &copy_, &paste_}) {
            b->textSize = kTextSmall;
            addAndMakeVisible(*b);
        }
        lock_.toggleRole = true;
        lock_.setHelp("Lock rack", "Keeps this rack and its settings when presets load.");
        lock_.onClick = [this] {
            ctx_.processor.setRackLocked(!ctx_.processor.rackLocked());
            lock_.setToggled(ctx_.processor.rackLocked());
        };
        save_.setHelp("Save rack", "Save all three lanes as a rack preset.");
        load_.setHelp("Load rack", "Load a rack preset into all three lanes.");
        save_.onClick = [this] {
            const juce::File folder = ctx_.prefs.userContentFolder().getChildFile("Racks");
            folder.createDirectory();
            auto chooser = std::make_shared<juce::FileChooser>("Save rack preset", folder.getChildFile("Rack.zrack"), "*.zrack");
            chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                 [safe = juce::Component::SafePointer<RackCard>(this), chooser](const juce::FileChooser& fc) {
                                     if (safe != nullptr && fc.getResult() != juce::File())
                                         fc.getResult().replaceWithText(str(safe->ctx_.processor.rackPresetJson()));
                                 });
        };
        load_.onClick = [this] {
            auto chooser = std::make_shared<juce::FileChooser>("Load rack preset", ctx_.prefs.userContentFolder().getChildFile("Racks"), "*.zrack");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, safe = juce::Component::SafePointer<RackCard>(this), chooser](const juce::FileChooser& fc) {
                if (safe == nullptr || !fc.getResult().existsAsFile())
                    return;
                const std::string json = fc.getResult().loadFileAsString().toStdString();
                std::vector<zed::Issue> issues;
                ctx_.session.perform("Load rack " + fc.getResult().getFileNameWithoutExtension(), [&] { return ctx_.processor.loadRackPresetJson(json, issues); });
                if (!issues.empty() && ctx_.notify)
                    ctx_.notify(str(issues.front().message));
            });
        };
        copy_.setHelp("Copy rack", "Copy all three lanes; paste them into another instance or preset.");
        paste_.setHelp("Paste rack", "Replace the rack with a copied one.");
        copy_.onClick = [this] { ctx_.session.copyModule("fx"); };
        paste_.onClick = [this] {
            juce::String error;
            if (!ctx_.session.pasteModule("fx", error) && ctx_.notify)
                ctx_.notify(error);
        };
        lock_.setToggled(ctx_.processor.rackLocked());
    }
    void layoutBody(juce::Rectangle<int> body) override {
        for (auto* f : faders_) {
            f->setBounds(body.removeFromTop(22));
            body.removeFromTop(4);
        }
        body.removeFromTop(4);
        const int w = (body.getWidth() - 6) / 2;
        placeRow(body.removeFromTop(24), {{fxOff_, w}, {&lock_, w}}, 6);
        body.removeFromTop(4);
        placeRow(body.removeFromTop(24), {{&save_, w}, {&load_, w}}, 6);
        body.removeFromTop(4);
        placeRow(body.removeFromTop(24), {{&copy_, w}, {&paste_, w}}, 6);
    }
    void refresh() { lock_.setToggled(ctx_.processor.rackLocked()); }

private:
    Context& ctx_;
    Parts parts_;
    std::vector<Fader*> faders_;
    Toggle* fxOff_ = nullptr;
    Button lock_{"Lock rack", Glyph::Lock}, save_{"Save rack", Glyph::Save}, load_{"Load rack", Glyph::Folder}, copy_{"Copy rack", Glyph::Copy},
        paste_{"Paste rack", Glyph::Paste};
};

// --- The page ----------------------------------------------------------------------------------------

namespace {
class DetailTicker final : public Widget {
public:
    explicit DetailTicker(std::function<bool()> f) : f_(std::move(f)) {
        setInterceptsMouseClicks(false, false);
        setAccessible(false);
    }
    bool tick() override { return f_(); }
    void draw(juce::Graphics&) override {}
    bool hasVisibleLabel() const override { return false; }

private:
    std::function<bool()> f_;
};
} // namespace

FxPage::FxPage(Context& ctx) : Page(ctx) {
    setTitle("FX");
    lanes_.push_back(std::make_unique<LaneColumn>(ctx, *this));
    addAndMakeVisible(*lanes_.back());
    lanes_.front()->setLane(0);
    detail_ = std::make_unique<SlotDetail>(ctx, *this);
    addAndMakeVisible(*detail_);
    rackCard_ = std::make_unique<RackCard>(ctx);
    addAndMakeVisible(*rackCard_);
    auto ticker = std::make_unique<DetailTicker>([this] { return detail_->tickDetail(); });
    ticker->setBounds(0, 0, 1, 1);
    addAndMakeVisible(*ticker);
    ticker_ = std::move(ticker);
    select(0, 0);
}

FxPage::~FxPage() = default;

int FxPage::slotCount(int lane) const {
    int n = 0;
    ctx_.processor.readDocument([&](const zed::Patch& p) { n = p.fxSlotCount(lane); });
    return n;
}

int FxPage::slotType(int lane, int slot) const {
    int t = -1;
    ctx_.processor.readDocument([&](const zed::Patch& p) { t = p.fxSlotType(lane, slot); });
    return t;
}

void FxPage::paint(juce::Graphics&) {} // the chassis shows through

void FxPage::resized() {
    auto area = getLocalBounds().reduced(8);
    auto left = area.removeFromLeft(236);
    area.removeFromLeft(8);
    rackCard_->setBounds(left.removeFromBottom(214));
    left.removeFromBottom(8);
    lanes_.front()->setBounds(left);
    detail_->setBounds(area);
}

void FxPage::select(int lane, int slot) {
    const bool laneChanged = lane != lane_;
    lane_ = lane;
    slot_ = slot;
    if (laneChanged || lanes_.front()->lane() != lane)
        lanes_.front()->setLane(lane);
    detail_->show(lane, slot);
    lanes_.front()->repaint();
    for (auto* c : lanes_.front()->getChildren())
        c->repaint();
}

bool FxPage::insertEffect(int lane, int position, int type) {
    const bool ok = ctx_.session.perform("Add " + effectName(type) + " to " + kLaneNames[lane], [&] {
        return ctx_.processor.insertEffect(lane, position, str(reg().effectTypes()[static_cast<std::size_t>(type)].id).toStdString());
    });
    if (ok) { // the chain and the rack rebuild at the next frame (never from inside the part that asked)
        lane_ = lane;
        slot_ = position;
    }
    return ok;
}

bool FxPage::removeEffect(int lane, int slot) {
    const juce::String name = effectName(slotType(lane, slot));
    const bool ok = ctx_.session.perform("Remove " + name + " from " + kLaneNames[lane], [&] { return ctx_.processor.removeEffect(lane, slot); });
    if (ok) {
        lane_ = lane;
        slot_ = juce::jmax(0, juce::jmin(slot, slotCount(lane) - 1));
    }
    return ok;
}

bool FxPage::moveEffect(int fromLane, int fromSlot, int toLane, int toPosition) {
    const juce::String name = effectName(slotType(fromLane, fromSlot));
    toPosition = juce::jlimit(0, slotCount(toLane) - (fromLane == toLane ? 1 : 0), toPosition);
    const bool ok = ctx_.session.perform("Move " + name + " to " + kLaneNames[toLane] + " " + juce::String(toPosition + 1),
                                         [&] { return ctx_.processor.moveEffect(fromLane, fromSlot, toLane, toPosition); });
    if (ok) {
        lane_ = toLane;
        slot_ = toPosition;
    }
    return ok;
}

void FxPage::structureChanged() {
    // Rebuild only when the layout changed (a curve being drawn also changes the document).
    juce::String signature;
    ctx_.processor.readDocument([&](const zed::Patch& p) {
        for (int l = 0; l < 3; ++l) {
            for (int s = 0; s < p.fxSlotCount(l); ++s)
                signature << p.fxSlotType(l, s) << ",";
            signature << "|";
        }
    });
    if (signature == layout_) {
        detail_->reloadCurves();
        return;
    }
    layout_ = signature;
    detail_->markStale();
    lanes_.front()->setLane(lane_);
    detail_->show(lane_, juce::jlimit(0, juce::jmax(0, slotCount(lane_) - 1), slot_));
    static_cast<RackCard*>(rackCard_.get())->refresh();
}

void FxPage::applySimpleView() {
    // Lay the strips out again (each shows or hides its advanced controls); never rebuild them here,
    // which would drop a control in the middle of a drag.
    resized();
    detail_->resized();
}

bool FxPage::reveal(const juce::String& module) {
    // "fx.a3", "fx.m1.p2" ...
    const juce::String m = module.fromFirstOccurrenceOf("fx.", false, false);
    if (m.isEmpty())
        return true;
    const juce::juce_wchar l = m[0];
    const int lane = l == 'a' ? 0 : l == 'b' ? 1 : l == 'm' ? 2 : -1;
    const int slot = m.substring(1).getIntValue() - 1;
    if (lane >= 0 && slot >= 0 && slot < slotCount(lane))
        select(lane, slot);
    return true;
}

void FxPage::saveState(nlohmann::json& state) const {
    state["lane"] = lane_;
    state["slot"] = slot_;
}

void FxPage::loadState(const nlohmann::json& state) {
    const int lane = state.value("lane", 0), slot = state.value("slot", 0);
    if (lane >= 0 && lane < 3)
        select(lane, slot);
}

} // namespace zui
