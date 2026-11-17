#include "FxPage.h"

#include "CurveEditor.h"
#include "Displays.h"

#include "zederia/patch/Modules.h"

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
        g.setColour(selected ? t.effects : t.line);
        g.drawRoundedRectangle(area, 4.0f, 1.0f);
        auto inner = area.reduced(6.0f, 0.0f);
        g.setColour(t.textDim);
        g.fillPath(glyphPath(Glyph::Grip, inner.removeFromLeft(8.0f).withSizeKeepingCentre(8.0f, 12.0f)));
        inner.removeFromLeft(6.0f);
        const bool on = (state_ & 1) != 0;
        const auto dot = inner.removeFromLeft(8.0f).withSizeKeepingCentre(8.0f, 8.0f);
        g.setColour(!on ? t.textFaint : (state_ & 2) ? t.textDim : t.effects);
        if (on)
            g.fillEllipse(dot);
        else
            g.drawEllipse(dot, 1.2f);
        inner.removeFromLeft(6.0f);
        const juce::String mix = juce::String(juce::roundToInt(mix_ * 100.0)) + " %";
        drawText(g, mix, inner.removeFromRight(40.0f), kTextSmall, t.textDim, juce::Justification::centredRight);
        drawText(g, effectName(type_) + ((state_ & 2) && on ? "  idle" : ""), inner, kTextBody, on ? t.text : t.textFaint);
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

// --- A lane --------------------------------------------------------------------------------------------

class LaneColumn final : public Card, public juce::DragAndDropTarget {
public:
    LaneColumn(Context& ctx, FxPage& page, int lane) : Card(kLaneNames[lane], Family::Effects), ctx_(ctx), page_(page), lane_(lane) {
        add_.textSize = kTextSmall + 0.5f;
        add_.setTitle("Add an effect to " + juce::String(kLaneNames[lane]));
        add_.setHelp("Add effect", "Any effect, any number of times; up to eight per lane.");
        add_.onClick = [this] {
            juce::PopupMenu menu;
            addEffectsMenu(menu, [this](int t) { page_.insertEffect(lane_, page_.slotCount(lane_), t); });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&add_));
        };
        addAndMakeVisible(add_);
        if (lane == 1) {
            routing_ = std::make_unique<Segmented>(ctx, "fx.b_routing", "Lane B");
            routing_->names = {"Parallel", "Series"};
            routing_->showLabel = false;
            routing_->labelledBy = "Lane B";
            routing_->family = Family::Effects;
            routing_->setSize(130, 20);
            addAndMakeVisible(*routing_);
            headerRight.push_back(routing_.get());
        }
        rebuild();
    }
    void rebuild() {
        rows_.clear();
        for (int s = 0; s < page_.slotCount(lane_); ++s) {
            rows_.push_back(std::make_unique<SlotRow>(ctx_, page_, lane_, s, page_.slotType(lane_, s)));
            addAndMakeVisible(*rows_.back());
        }
        add_.setEnabled(page_.slotCount(lane_) < zed::kNumFxSlots);
        resized();
    }
    void layoutBody(juce::Rectangle<int> body) override {
        for (auto& r : rows_) {
            r->setBounds(body.removeFromTop(34));
            body.removeFromTop(4);
        }
        add_.setBounds(body.removeFromTop(28));
    }
    void paintOverChildren(juce::Graphics& g) override {
        if (dropAt_ < 0)
            return;
        const auto b = body();
        const float y = static_cast<float>(b.getY() + dropAt_ * 38) - 2.0f;
        g.setColour(theme().focus);
        g.fillRoundedRectangle(static_cast<float>(b.getX()), y - 1.0f, static_cast<float>(b.getWidth()), 3.0f, 1.5f);
    }
    bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("fxslot:"); }
    void itemDragMove(const SourceDetails& d) override {
        const int at = juce::jlimit(0, page_.slotCount(lane_), (d.localPosition.y - body().getY() + 19) / 38);
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
    Context& ctx_;
    FxPage& page_;
    int lane_;
    Button add_{"Add effect", Glyph::Plus};
    std::unique_ptr<Segmented> routing_;
    std::vector<std::unique_ptr<SlotRow>> rows_;
    int dropAt_ = -1;
};

// --- The selected slot ---------------------------------------------------------------------------------

class SlotDetail final : public Card {
public:
    SlotDetail(Context& ctx, FxPage& page) : Card("Effect", Family::Effects), ctx_(ctx), page_(page) {
        for (Button* b : {&presets_, &remove_}) {
            b->textSize = kTextSmall;
            addAndMakeVisible(*b);
        }
        presets_.setSize(84, 20);
        remove_.setSize(78, 20);
        headerRight = {&remove_, &presets_};
        presets_.setHelp("Effect presets", "Save or load this effect with its settings; copy and paste it.");
        remove_.setHelp("Remove", "Remove this effect from its lane.");
        presets_.onClick = [this] { showPresets(); };
        remove_.onClick = [this] { page_.removeEffect(lane_, slot_); };
    }
    void show(int lane, int slot) {
        lane_ = lane;
        slot_ = slot;
        type_ = page_.slotType(lane, slot);
        parts_.clear();
        items_.clear();
        display_ = nullptr;
        curves_[0] = curves_[1] = nullptr;
        power_ = nullptr;
        setTitleText(type_ >= 0 ? juce::String(kLaneNames[lane]) + " " + juce::String(slot + 1) + "  " + effectName(type_) : juce::String("No effect"));
        setTitle(getTitle());
        presets_.setVisible(type_ >= 0);
        remove_.setVisible(type_ >= 0);
        moduleId = slotId(lane, slot);
        if (type_ < 0) {
            powerSwitch = nullptr;
            resized();
            return;
        }
        auto& power = parts_.add<Toggle>(*this, ctx_, reg().fxSlotOn(lane, slot), "On");
        power.power = true;
        power.family = Family::Effects;
        powerSwitch = &power;
        power_ = &power;
        auto& mix = parts_.add<Knob>(*this, ctx_, reg().fxSlotMix(lane, slot), "Mix");
        mix.family = Family::Effects;
        items_.push_back({&mix, 64});
        const auto& type = reg().effectTypes()[static_cast<std::size_t>(type_)];
        for (int k = 0; k < static_cast<int>(type.params.size()); ++k) {
            const auto& d = type.params[static_cast<std::size_t>(k)];
            const int index = reg().fxSlotParam(lane, slot, k);
            const juce::String label = shortLabel(str(d.name));
            if (d.type == zed::ParamType::Choice) {
                auto& c = parts_.add<ChoiceBox>(*this, ctx_, index, label);
                c.family = Family::Effects;
                if (d.id == "filter_type")
                    c.groupOf = [](const juce::String& t) { return t.upToFirstOccurrenceOf("_", false, false).toUpperCase(); };
                items_.push_back({&c, d.choices.size() > 8 ? 150 : 110, kChoiceH});
            } else if (d.type == zed::ParamType::Bool) {
                auto& tg = parts_.add<Toggle>(*this, ctx_, index, label);
                tg.family = Family::Effects;
                items_.push_back({&tg, 100, 22});
            } else if (d.type == zed::ParamType::Int) {
                auto& nb = parts_.add<NumberBox>(*this, ctx_, index, label);
                nb.family = Family::Effects;
                items_.push_back({&nb, 64, kChoiceH});
            } else {
                auto& kn = parts_.add<Knob>(*this, ctx_, index, label);
                kn.family = Family::Effects;
                items_.push_back({&kn, 64});
            }
        }
        const juce::String id = str(type.id);
        if (id == "widener") {
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
            auto& spectrum = parts_.add<SpectrumView>(*this, ctx_);
            display_ = &spectrum;
        } else {
            auto& spectrum = parts_.add<SpectrumView>(*this, ctx_);
            if (id == "eq" || id == "filter")
                spectrum.curve = [this](const std::vector<double>& hz, std::vector<float>& db) {
                    return ctx_.processor.engine().fxResponse(lane_, slot_, hz, db);
                };
            display_ = &spectrum;
        }
        resized();
    }
    void reloadCurves() {
        if (curves_[0] == nullptr)
            return;
        ctx_.processor.readDocument([&](const zed::Patch& p) {
            for (int c = 0; c < 2; ++c)
                curves_[c]->setShape(p.fxCurves[static_cast<std::size_t>(lane_)][static_cast<std::size_t>(slot_)][static_cast<std::size_t>(c)]);
        });
    }
    void layoutBody(juce::Rectangle<int> body) override {
        if (type_ < 0)
            return;
        const int used = flow(body, items_, kKnobH + 4, 6);
        body.removeFromTop(used + 10);
        const bool curveModes = curves_[0] != nullptr && ctx_.processor.plainValue(reg().fxSlotParam(lane_, slot_, 0)) >= 13.0;
        for (auto* c : curves_)
            if (c != nullptr)
                c->setVisible(curveModes);
        if (curveModes) {
            auto row = body;
            curves_[0]->setBounds(row.removeFromLeft((row.getWidth() - 8) / 2));
            row.removeFromLeft(8);
            curves_[1]->setBounds(row);
            if (display_ != nullptr)
                display_->setVisible(false);
        } else if (display_ != nullptr) {
            display_->setVisible(true);
            display_->setBounds(body);
        }
    }
    bool tickDetail() {
        if (curves_[0] == nullptr)
            return false;
        const bool curveModes = ctx_.processor.plainValue(reg().fxSlotParam(lane_, slot_, 0)) >= 13.0;
        if (curveModes != curves_[0]->isVisible()) {
            resized();
            return true;
        }
        return false;
    }
    void draw(juce::Graphics& g) override {
        Card::draw(g);
        if (type_ < 0)
            drawText(g, "Choose a slot, or add an effect to a lane.", body().toFloat(), kTextBody, theme().textDim, juce::Justification::centred);
    }
    int lane() const { return lane_; }
    int slot() const { return slot_; }

private:
    void showPresets() {
        juce::PopupMenu menu;
        const int lane = lane_, slot = slot_;
        Context* ctx = &ctx_;
        FxPage* page = &page_;
        menu.addItem("Save effect preset...", [ctx, lane, slot] {
            const juce::File folder = ctx->prefs.userContentFolder().getChildFile("Racks").getChildFile("Effects");
            folder.createDirectory();
            auto chooser = std::make_shared<juce::FileChooser>("Save effect preset", folder.getChildFile("Effect.zfx"), "*.zfx");
            chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [ctx, lane, slot, chooser](const juce::FileChooser& fc) {
                if (fc.getResult() != juce::File())
                    fc.getResult().replaceWithText(str(ctx->processor.slotPresetJson(lane, slot)));
            });
        });
        menu.addItem("Load effect preset...", [ctx, page, lane, slot] {
            auto chooser = std::make_shared<juce::FileChooser>("Load effect preset", ctx->prefs.userContentFolder().getChildFile("Racks"), "*.zfx");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [ctx, page, lane, slot, chooser](const juce::FileChooser& fc) {
                if (!fc.getResult().existsAsFile())
                    return;
                const std::string json = fc.getResult().loadFileAsString().toStdString();
                std::vector<zed::Issue> issues;
                ctx->session.perform("Load effect preset " + fc.getResult().getFileNameWithoutExtension(), [&] {
                    return ctx->processor.loadSlotPresetJson(json, lane, slot, issues);
                });
                if (!issues.empty() && ctx->notify)
                    ctx->notify(str(issues.front().message));
                page->select(lane, slot);
            });
        });
        menu.addSeparator();
        const juce::String id = slotId(lane, slot);
        menu.addItem("Copy effect", [ctx, id] { ctx->session.copyModule(id); });
        menu.addItem("Paste effect here", ctx_.session.clipboardClass() == "fx_slot", false, [ctx, id] {
            juce::String error;
            if (!ctx->session.pasteModule(id, error) && ctx->notify) ctx->notify(error);
        });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presets_));
    }

    Context& ctx_;
    FxPage& page_;
    Parts parts_;
    std::vector<FlowItem> items_;
    Button presets_{"Presets"}, remove_{"Remove", Glyph::Trash};
    Widget* display_ = nullptr;
    CurveEditor* curves_[2] = {nullptr, nullptr};
    Toggle* power_ = nullptr;
    int lane_ = 0, slot_ = 0, type_ = -1;
};

// --- Rack card: balances, FX off, lock, rack presets ----------------------------------------------------

class RackCard final : public Card {
public:
    RackCard(Context& ctx) : Card("Rack", Family::Effects), ctx_(ctx) {
        moduleId = "fx";
        for (const auto& [id, label] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"fx.filter1_balance", "Filter 1: A to B"}, {"fx.filter2_balance", "Filter 2: A to B"}, {"fx.direct_balance", "Direct: A to B"}}) {
            auto& f = parts_.add<Fader>(*this, ctx, id, label);
            f.labelWidth = 116;
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
            chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, chooser](const juce::FileChooser& fc) {
                if (fc.getResult() != juce::File())
                    fc.getResult().replaceWithText(str(ctx_.processor.rackPresetJson()));
            });
        };
        load_.onClick = [this] {
            auto chooser = std::make_shared<juce::FileChooser>("Load rack preset", ctx_.prefs.userContentFolder().getChildFile("Racks"), "*.zrack");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, chooser](const juce::FileChooser& fc) {
                if (!fc.getResult().existsAsFile())
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
            f->setBounds(body.removeFromTop(22).withWidth(juce::jmin(body.getWidth(), 420)));
            body.removeFromTop(4);
        }
        body.removeFromTop(4);
        placeRow(body.removeFromTop(26), {{fxOff_, 90}, {&lock_, 90}, {&save_, 96}, {&load_, 96}, {&copy_, 90}, {&paste_, 96}}, 6);
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
    for (int lane = 0; lane < 3; ++lane) {
        lanes_.push_back(std::make_unique<LaneColumn>(ctx, *this, lane));
        addAndMakeVisible(*lanes_.back());
    }
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

void FxPage::paint(juce::Graphics& g) { g.fillAll(theme().bg); }

void FxPage::resized() {
    auto area = getLocalBounds().reduced(8);
    auto left = area.removeFromLeft(732);
    area.removeFromLeft(8);
    detail_->setBounds(area);
    rackCard_->setBounds(left.removeFromBottom(150));
    left.removeFromBottom(8);
    const int w = (left.getWidth() - 16) / 3;
    for (auto& lane : lanes_) {
        lane->setBounds(left.removeFromLeft(w));
        left.removeFromLeft(8);
    }
}

void FxPage::select(int lane, int slot) {
    lane_ = lane;
    slot_ = slot;
    detail_->show(lane, slot);
    for (auto& l : lanes_)
        l->repaint();
}

bool FxPage::insertEffect(int lane, int position, int type) {
    const bool ok = ctx_.session.perform("Add " + effectName(type) + " to " + kLaneNames[lane], [&] {
        return ctx_.processor.insertEffect(lane, position, str(reg().effectTypes()[static_cast<std::size_t>(type)].id).toStdString());
    });
    if (ok) { // the lanes rebuild at the next frame (never from inside the row that asked)
        select(lane, position);
    }
    return ok;
}

bool FxPage::removeEffect(int lane, int slot) {
    const juce::String name = effectName(slotType(lane, slot));
    const bool ok = ctx_.session.perform("Remove " + name + " from " + kLaneNames[lane], [&] { return ctx_.processor.removeEffect(lane, slot); });
    if (ok) { // the lanes rebuild at the next frame (never from inside the row that asked)
        select(lane, juce::jmax(0, juce::jmin(slot, slotCount(lane) - 1)));
    }
    return ok;
}

bool FxPage::moveEffect(int fromLane, int fromSlot, int toLane, int toPosition) {
    const juce::String name = effectName(slotType(fromLane, fromSlot));
    toPosition = juce::jlimit(0, slotCount(toLane) - (fromLane == toLane ? 1 : 0), toPosition);
    const bool ok = ctx_.session.perform("Move " + name + " to " + kLaneNames[toLane] + " " + juce::String(toPosition + 1),
                                         [&] { return ctx_.processor.moveEffect(fromLane, fromSlot, toLane, toPosition); });
    if (ok) { // the lanes rebuild at the next frame (never from inside the row that asked)
        select(toLane, toPosition);
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
    for (auto& lane : lanes_)
        lane->rebuild();
    detail_->show(lane_, juce::jlimit(0, juce::jmax(0, slotCount(lane_) - 1), slot_));
    static_cast<RackCard*>(rackCard_.get())->refresh();
}

void FxPage::applySimpleView() { resized(); }

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
