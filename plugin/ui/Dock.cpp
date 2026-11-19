#include "Dock.h"

#include "zederia/patch/Modules.h"

namespace zui {

namespace {

const zed::Registry& reg() { return zed::Registry::get(); }
int idx(const juce::String& id) { return reg().indexOf(id.toStdString()); }

zed::Shape envToShape(const zed::EnvShape& env, double& totalMs) {
    zed::Shape s;
    totalMs = 0.0;
    for (const auto& p : env.points)
        totalMs += std::max(0.0, p.timeMs);
    if (totalMs <= 0.0)
        totalMs = 1000.0;
    double t = 0.0;
    s.points.push_back({0.0, 0.0, env.points.empty() ? 0.0 : env.points.front().curve});
    for (std::size_t i = 0; i < env.points.size(); ++i) {
        t += std::max(0.0, env.points[i].timeMs);
        const double curve = i + 1 < env.points.size() ? env.points[i + 1].curve : 0.0;
        s.points.push_back({t / totalMs, env.points[i].level, curve});
    }
    return s;
}

zed::EnvShape shapeToEnv(const zed::Shape& s, double totalMs, int sustain, int loopStart, int loopEnd) {
    zed::EnvShape env;
    for (std::size_t i = 1; i < s.points.size() && env.points.size() < static_cast<std::size_t>(zed::kMaxEnvPoints); ++i) {
        zed::EnvPoint p;
        p.timeMs = std::max(0.0, s.points[i].x - s.points[i - 1].x) * totalMs;
        p.level = juce::jlimit(0.0, 1.0, s.points[i].y);
        p.curve = s.points[i - 1].curve;
        env.points.push_back(p);
    }
    const int n = static_cast<int>(env.points.size());
    env.sustainPoint = sustain >= 1 && sustain - 1 < n ? sustain - 1 : -1;
    env.loopStart = loopStart >= 1 && loopStart - 1 < n ? loopStart - 1 : -1;
    env.loopEnd = loopEnd >= 1 && loopEnd - 1 < n ? loopEnd - 1 : -1;
    if (env.loopStart < 0 || env.loopEnd <= env.loopStart)
        env.loopStart = env.loopEnd = -1;
    return env;
}

} // namespace

// --- Macros ------------------------------------------------------------------------------------------

class MacroCard final : public Card {
public:
    explicit MacroCard(Context& ctx) : Card("Macros", Family::Modulators), ctx_(ctx) {
        moduleId = "macro";
        screws = false;
        headerHeight = 24;
        for (int m = 0; m < zed::kNumMacros; ++m) {
            auto& knob = parts_.add<Knob>(*this, ctx, reg().macroValue(m), "");
            knob.small = true;
            knob.showValue = false;
            knob.showLabel = false;
            knob.labelledBy = "Macro " + juce::String(m + 1);
            knob.family = Family::Modulators;
            auto& chip = parts_.add<DragChip>(*this, ctx, "macro" + juce::String(m + 1));
            chip.setHelp("Macro " + juce::String(m + 1), "Drag onto any control to let this macro move it. Double-click to rename.");
            chip.onDoubleClick = [this, m] { rename(m); };
            auto& bipolar = parts_.add<Segmented>(*this, ctx, reg().macroBipolar(m), "Bipolar");
            bipolar.names = {"+", juce::String::fromUTF8("\xc2\xb1")};
            bipolar.showLabel = false;
            bipolar.labelledBy = "Macro " + juce::String(m + 1);
            bipolar.family = Family::Modulators;
            bipolar.setHelp("Macro range", "0 to 1 (+), or -1 to 1 around the middle.");
            knobs_.push_back(&knob);
            chips_.push_back(&chip);
            polarity_.push_back(&bipolar);
        }
        refreshNames();
    }
    void refreshNames() {
        const auto list = ctx_.processor.macroNames();
        for (int m = 0; m < zed::kNumMacros; ++m) {
            const juce::String n = str(list[static_cast<std::size_t>(m)]);
            const juce::String shown = n.isEmpty() ? "Macro " + juce::String(m + 1) : n;
            knobs_[static_cast<std::size_t>(m)]->setTitle(shown);
            chips_[static_cast<std::size_t>(m)]->text = shown;
            chips_[static_cast<std::size_t>(m)]->setTitle(shown + " modulation source");
            chips_[static_cast<std::size_t>(m)]->repaint();
        }
    }
    void layoutBody(juce::Rectangle<int> body) override {
        const int cols = 4, gap = 4;
        const int w = (body.getWidth() - gap * (cols - 1)) / cols;
        const int h = (body.getHeight() - gap) / 2;
        for (int m = 0; m < zed::kNumMacros; ++m) {
            auto cell = juce::Rectangle<int>(body.getX() + (m % cols) * (w + gap), body.getY() + (m / cols) * (h + gap), w, h);
            chips_[static_cast<std::size_t>(m)]->setBounds(cell.removeFromTop(18));
            cell.removeFromTop(2);
            polarity_[static_cast<std::size_t>(m)]->setBounds(cell.removeFromRight(26).withSizeKeepingCentre(26, 16));
            knobs_[static_cast<std::size_t>(m)]->setBounds(cell.withSizeKeepingCentre(juce::jmin(cell.getWidth(), 40), cell.getHeight()));
        }
    }

private:
    void rename(int m) {
        auto editor = std::make_shared<juce::AlertWindow>("Rename macro", "A name for macro " + juce::String(m + 1), juce::MessageBoxIconType::NoIcon);
        const auto names = ctx_.processor.macroNames();
        editor->addTextEditor("name", str(names[static_cast<std::size_t>(m)]).isEmpty() ? "Macro " + juce::String(m + 1) : str(names[static_cast<std::size_t>(m)]));
        editor->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        editor->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        editor->enterModalState(true, juce::ModalCallbackFunction::create([this, editor, m](int result) {
            if (result != 1)
                return;
            const std::string text = editor->getTextEditorContents("name").substring(0, 32).toStdString();
            ctx_.session.edit("Rename macro " + juce::String(m + 1), [&](zed::Patch& p) {
                p.macroNames[static_cast<std::size_t>(m)] = text;
                return true;
            });
            refreshNames();
        }), false);
    }

    Context& ctx_;
    Parts parts_;
    std::vector<Knob*> knobs_;
    std::vector<DragChip*> chips_;
    std::vector<Segmented*> polarity_;
};

// --- Modulators ----------------------------------------------------------------------------------------

class ModulatorCard final : public Card {
public:
    explicit ModulatorCard(Context& ctx)
        : Card("Modulators", Family::Modulators), ctx_(ctx), env_(ctx, 0), scope_(ctx), lfo_(ctx, "LFO shape"), multi_(ctx, "Envelope shape") {
        screws = false;
        headerHeight = 24;
        juce::StringArray sources;
        for (int i = 1; i <= zed::kNumEnvs; ++i) sources.add("env" + juce::String(i));
        for (int i = 1; i <= zed::kNumLfos; ++i) sources.add("lfo" + juce::String(i));
        for (int i = 1; i <= zed::kNumFlux; ++i) sources.add("flux" + juce::String(i));
        for (const auto& s : sources) {
            auto& chip = parts_.add<DragChip>(*this, ctx, s);
            chip.onSelect = [this, s] { select(s); };
            chips_.push_back(&chip);
            buildControls(s);
        }
        for (juce::Component* c : std::initializer_list<juce::Component*>{&env_, &scope_, &lfo_, &multi_})
            addChildComponent(c);
        for (Button* b : {&shapes_, &steps_, &snap_, &copy_, &paste_}) {
            b->textSize = kTextSmall;
            addAndMakeVisible(*b);
        }
        shapes_.setSize(62, 18);
        steps_.setSize(52, 18);
        snap_.setSize(48, 18);
        copy_.setSize(24, 18);
        paste_.setSize(24, 18);
        headerRight = {&paste_, &copy_, &snap_, &steps_, &shapes_};
        steps_.toggleRole = snap_.toggleRole = true;
        copy_.setTitle("Copy shape");
        paste_.setTitle("Paste shape");
        copy_.setTooltip("Copy shape");
        paste_.setTooltip("Paste shape");
        copy_.setHelp("Copy shape", "Copy this shape; paste it into any LFO, envelope, warp curve or drive curve.");
        paste_.setHelp("Paste shape", "Paste a copied shape here.");
        shapes_.setHelp("Shapes", "Start from a basic shape.");
        steps_.setHelp("Step drawing", "Drag across the grid to draw steps.");
        snap_.setHelp("Snap", "Points snap to the grid (hold Alt while dragging to move freely).");
        steps_.onClick = [this] { if (auto* e = editor()) { e->stepMode = !e->stepMode; e->repaint(); } updateButtons(); };
        snap_.onClick = [this] { if (auto* e = editor()) e->snap = !e->snap; updateButtons(); };
        copy_.onClick = [this] { copyShape(); };
        paste_.onClick = [this] { pasteShape(); };
        shapes_.onClick = [this] {
            juce::PopupMenu menu;
            for (const auto& name : CurveEditor::presetNames())
                menu.addItem(name, [this, name] {
                    if (auto* e = editor()) {
                        e->applyPreset(name);
                        if (e->onChange) e->onChange(e->shape());
                        if (e->onCommit) e->onCommit(e->shape(), "Shape " + name);
                    }
                });
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&shapes_));
        };
        CurveEditor::Config lfoConfig;
        lfoConfig.loopPoint = true;
        lfo_.setConfig(lfoConfig);
        lfo_.family = Family::Modulators;
        lfo_.onChange = [this](const zed::Shape& s) { ctx_.processor.setLfoShapeLive(lfoIndex_, s); };
        lfo_.onCommit = [this](const zed::Shape&, const juce::String& label) { ctx_.session.commit(label); };
        CurveEditor::Config envConfig;
        envConfig.bipolar = false;
        envConfig.anchoredStart = true;
        envConfig.envelopeMarkers = true;
        envConfig.maxPoints = zed::kMaxEnvPoints + 1;
        envConfig.xUnit = "ms";
        multi_.setConfig(envConfig);
        multi_.family = Family::Modulators;
        multi_.onChange = [this](const zed::Shape& s) { pushEnv(s); };
        multi_.onCommit = [this](const zed::Shape&, const juce::String& label) { ctx_.session.commit(label); };
        multi_.onMarkers = [this](int, int, int) {
            pushEnv(multi_.shape());
            ctx_.session.commit("Envelope " + juce::String(envIndex_ + 1) + " sustain and loop");
        };
        for (CurveEditor* e : {&lfo_, &multi_}) {
            e->onCopy = [this] { copyShape(); };
            e->onPaste = [this] { pasteShape(); };
        }
        select("env1");
    }

    const juce::String& selected() const { return selected_; }
    void select(const juce::String& source) {
        selected_ = source;
        if (source.startsWith("env")) {
            envIndex_ = source.getTrailingIntValue() - 1;
            env_.setEnv(envIndex_);
        } else if (source.startsWith("lfo")) {
            lfoIndex_ = source.getTrailingIntValue() - 1;
        } else {
            scope_.source = source;
        }
        reloadShapes();
        for (auto* c : chips_)
            c->repaint();
        updateButtons();
        repaint();
    }

    void reloadShapes() {
        ctx_.processor.readDocument([&](const zed::Patch& p) {
            lfo_.setShape(p.lfoShapes[static_cast<std::size_t>(lfoIndex_)]);
            const zed::EnvShape& env = p.envShapes[static_cast<std::size_t>(envIndex_)];
            zed::EnvShape shown = env;
            if (shown.points.empty()) { // a sensible start: attack, decay to a sustain, release
                shown.points = {{10.0, "", 1.0, 0.0}, {250.0, "", 0.6, 0.5}, {400.0, "", 0.0, 0.5}};
                shown.sustainPoint = 1;
            }
            double total = 1000.0;
            multi_.setShape(envToShape(shown, total));
            envTotalMs_ = total;
            auto config = multi_.config();
            config.xScale = total;
            multi_.setConfig(config);
            multi_.sustainPoint = shown.sustainPoint >= 0 ? shown.sustainPoint + 1 : -1;
            multi_.loopStart = shown.loopStart >= 0 ? shown.loopStart + 1 : -1;
            multi_.loopEnd = shown.loopEnd >= 0 ? shown.loopEnd + 1 : -1;
        });
    }

    bool tickCard() {
        bool changed = false;
        for (auto& [id, sync] : syncParams_) {
            const bool on = ctx_.processor.plainValue(sync) >= 0.5;
            if (on != syncShown_[id]) {
                syncShown_[id] = on;
                changed = true;
            }
        }
        const bool multi = isMulti();
        if (multi != multiShown_) {
            multiShown_ = multi;
            reloadShapes();
            updateButtons();
            changed = true;
        }
        if (changed)
            resized();
        return changed;
    }

    void layoutBody(juce::Rectangle<int> body) override {
        auto chipRow = body.removeFromTop(20);
        const int n = static_cast<int>(chips_.size());
        const int w = (chipRow.getWidth() - (n - 1) * 4) / n;
        for (auto* c : chips_) {
            c->setBounds(chipRow.removeFromLeft(w));
            chipRow.removeFromLeft(4);
        }
        body.removeFromTop(4);
        const auto display = body.removeFromLeft(juce::jmin(330, body.getWidth() / 3));
        body.removeFromLeft(10);
        const bool env = selected_.startsWith("env"), lfo = selected_.startsWith("lfo");
        const bool multi = env && isMulti();
        env_.setVisible(env && !multi);
        multi_.setVisible(multi);
        lfo_.setVisible(lfo);
        scope_.setVisible(!env && !lfo);
        for (juce::Component* c : std::initializer_list<juce::Component*>{&env_, &multi_, &lfo_, &scope_})
            c->setBounds(display);
        const bool simple = ctx_.simpleView();
        for (auto& [source, items] : controls_)
            for (auto& item : items)
                if (auto* widget = dynamic_cast<Widget*>(item.c))
                    widget->setVisible(source == selected_ && shownInView(*widget, simple) && visibleNow(*widget));
        if (const auto it = controls_.find(selected_); it != controls_.end())
            flow(body, it->second, kKnobH, 4);
    }

    void draw(juce::Graphics& g) override { Card::draw(g); } // the chip on show is the pressed-in one

private:
    bool isMulti() const { return envIndex_ > 0 && ctx_.processor.plainValue(idx("env" + juce::String(envIndex_ + 1) + ".mode")) >= 0.5; }
    CurveEditor* editor() {
        if (selected_.startsWith("lfo"))
            return &lfo_;
        if (selected_.startsWith("env") && isMulti())
            return &multi_;
        return nullptr;
    }
    void updateButtons() {
        const bool curve = editor() != nullptr;
        for (Button* b : {&shapes_, &steps_, &snap_, &copy_, &paste_})
            b->setVisible(curve);
        if (auto* e = editor()) {
            steps_.setToggled(e->stepMode);
            snap_.setToggled(e->snap);
        }
        resized();
    }
    bool visibleNow(const Widget& w) const {
        // Rate or division, whichever the sync switch selects.
        const auto it = rateOrDivision_.find(&w);
        if (it == rateOrDivision_.end())
            return true;
        const bool synced = ctx_.processor.plainValue(it->second.first) >= 0.5;
        return it->second.second ? synced : !synced;
    }
    void pushEnv(const zed::Shape& s) {
        ctx_.processor.setEnvShapeLive(envIndex_, shapeToEnv(s, envTotalMs_, multi_.sustainPoint, multi_.loopStart, multi_.loopEnd));
    }
    void copyShape() {
        if (selected_.startsWith("env")) {
            zed::EnvShape env;
            ctx_.processor.readDocument([&](const zed::Patch& p) { env = p.envShapes[static_cast<std::size_t>(envIndex_)]; });
            ctx_.session.copyText(str(zed::copyEnvShape(env)));
        } else if (auto* e = editor()) {
            ctx_.session.copyText(str(zed::copyShape(e->shape(), zed::ShapeKind::Lfo)));
        }
        if (ctx_.notify)
            ctx_.notify("Shape copied: paste it into any LFO, envelope or curve.");
    }
    void pasteShape() {
        const std::string text = ctx_.session.clipboardText().toStdString();
        std::vector<zed::Issue> issues;
        bool ok = false;
        if (selected_.startsWith("env")) {
            zed::EnvShape env;
            if (zed::pasteEnvShape(text, env, issues))
                ok = ctx_.session.edit("Paste shape into Env " + juce::String(envIndex_ + 1), [&](zed::Patch& p) {
                    p.envShapes[static_cast<std::size_t>(envIndex_)] = env;
                    return true;
                });
        } else if (selected_.startsWith("lfo")) {
            zed::Shape shape;
            if (zed::pasteShape(text, zed::ShapeKind::Lfo, shape, issues))
                ok = ctx_.session.edit("Paste shape into LFO " + juce::String(lfoIndex_ + 1), [&](zed::Patch& p) {
                    p.lfoShapes[static_cast<std::size_t>(lfoIndex_)] = shape;
                    return true;
                });
        }
        if (ok)
            reloadShapes();
        else if (ctx_.notify)
            ctx_.notify(issues.empty() ? juce::String("Nothing to paste.") : str(issues.front().message));
    }

    void buildControls(const juce::String& s) {
        auto& items = controls_[s];
        const auto knob = [&](const juce::String& suffix, const char* label, bool adv = false) -> Knob& {
            auto& k = parts_.add<Knob>(*this, ctx_, (s + "." + suffix).toRawUTF8(), label);
            k.family = Family::Modulators;
            k.advanced = adv;
            return k;
        };
        const auto choice = [&](const juce::String& suffix, const char* label, bool adv = false) -> ChoiceBox& {
            auto& c = parts_.add<ChoiceBox>(*this, ctx_, (s + "." + suffix).toRawUTF8(), label);
            c.family = Family::Modulators;
            c.advanced = adv;
            return c;
        };
        const auto toggle = [&](const juce::String& suffix, const char* label, bool adv = false) -> Toggle& {
            auto& t = parts_.add<Toggle>(*this, ctx_, (s + "." + suffix).toRawUTF8(), label);
            t.family = Family::Modulators;
            t.advanced = adv;
            return t;
        };
        if (s.startsWith("env")) {
            const int env = s.getTrailingIntValue() - 1;
            static const char* const segments[5] = {"delay", "attack", "hold", "decay", "release"};
            const auto timeKnob = [&](int seg, const char* label, bool adv) -> Knob& {
                auto& k = knob(segments[seg], label, adv);
                k.valueOverride = [this, env, seg] {
                    std::string token;
                    ctx_.processor.readDocument([&](const zed::Patch& p) { token = p.envSync[static_cast<std::size_t>(env)][static_cast<std::size_t>(seg)]; });
                    return str(token);
                };
                k.setHelp(k.getTitle(), k.helpText + " Type a note length (1/8, 1/4T, 3/16) to follow the tempo.");
                return k;
            };
            items = {{&timeKnob(0, "Delay", true), kKnobW}, {&timeKnob(1, "Attack", false), kKnobW}, {&timeKnob(2, "Hold", true), kKnobW},
                     {&timeKnob(3, "Decay", false), kKnobW}, {&knob("sustain", "Sustain"), kKnobW}, {&timeKnob(4, "Release", false), kKnobW},
                     {&knob("attack_curve", "A curve", true), kKnobW}, {&knob("decay_curve", "D curve", true), kKnobW},
                     {&knob("release_curve", "R curve", true), kKnobW}};
            if (env > 0) {
                auto& mode = parts_.add<Segmented>(*this, ctx_, (s + ".mode").toRawUTF8(), "Mode");
                mode.names = {"Standard", "Multi"};
                mode.family = Family::Modulators;
                items.push_back({&mode, 150, kChoiceH, true});
            }
        } else if (s.startsWith("lfo")) {
            auto& rate = knob("rate", "Rate");
            auto& division = choice("division", "Division");
            const int sync = idx(s + ".sync");
            rateOrDivision_[&rate] = {sync, false};
            rateOrDivision_[&division] = {sync, true};
            syncParams_[s] = sync;
            auto& polarity = parts_.add<Segmented>(*this, ctx_, (s + ".polarity").toRawUTF8(), "Polarity");
            polarity.names = {"Bipolar", "Unipolar"};
            polarity.family = Family::Modulators;
            auto& scope = parts_.add<Segmented>(*this, ctx_, (s + ".scope").toRawUTF8(), "Scope");
            scope.names = {"Voice", "Global"};
            scope.family = Family::Modulators;
            scope.advanced = true;
            // Knobs on the first row, switches and menus on the second.
            items = {{&rate, kKnobW}, {&division, 76, kChoiceH}, {&knob("phase", "Phase"), kKnobW}, {&knob("delay", "Delay", true), kKnobW},
                     {&knob("rise", "Rise"), kKnobW}, {&knob("smooth", "Smooth", true), kKnobW}, {&knob("keytrack", "Key track", true), kKnobW},
                     {&choice("mode", "Mode"), 104, kChoiceH, true}, {&toggle("sync", "Sync"), 60, 22}, {&toggle("anchor", "Anchor", true), 74, 22},
                     {&polarity, 136, kChoiceH}, {&scope, 112, kChoiceH}, {&toggle("audio_rate", "Audio rate", true), 96, 22}};
        } else {
            auto& rate = knob("rate", "Rate");
            auto& division = choice("division", "Division");
            const int sync = idx(s + ".sync");
            rateOrDivision_[&rate] = {sync, false};
            rateOrDivision_[&division] = {sync, true};
            syncParams_[s] = sync;
            auto& scope = parts_.add<Segmented>(*this, ctx_, (s + ".scope").toRawUTF8(), "Scope");
            scope.names = {"Voice", "Global"};
            scope.family = Family::Modulators;
            items = {{&rate, kKnobW}, {&division, 76, kChoiceH}, {&choice("type", "Type"), 110, kChoiceH, true}, {&toggle("sync", "Sync"), 60, 22},
                     {&scope, 112, kChoiceH}};
        }
        for (auto& item : items)
            item.c->setVisible(false);
    }

    Context& ctx_;
    Parts parts_;
    std::vector<DragChip*> chips_;
    std::map<juce::String, std::vector<FlowItem>> controls_;
    std::map<const Widget*, std::pair<int, bool>> rateOrDivision_;
    std::map<juce::String, int> syncParams_;
    std::map<juce::String, bool> syncShown_;
    juce::String selected_;
    EnvelopeView env_;
    SourceScope scope_;
    CurveEditor lfo_, multi_;
    Button shapes_{"Shapes"}, steps_{"Steps"}, snap_{"Snap"}, copy_{"", Glyph::Copy}, paste_{"", Glyph::Paste};
    int envIndex_ = 0, lfoIndex_ = 0;
    double envTotalMs_ = 1000.0;
    bool multiShown_ = false;
};

// --- The dock --------------------------------------------------------------------------------------

ModDock::ModDock(Context& ctx) {
    setTitle("Modulation dock");
    macros_ = std::make_unique<MacroCard>(ctx);
    modulators_ = std::make_unique<ModulatorCard>(ctx);
    addAndMakeVisible(*macros_);
    addAndMakeVisible(*modulators_);
}

ModDock::~ModDock() = default;

void ModDock::resized() {
    auto area = getLocalBounds().reduced(8, 4);
    macros_->setBounds(area.removeFromLeft(292));
    area.removeFromLeft(8);
    modulators_->setBounds(area);
}

void ModDock::select(const juce::String& source) {
    if (source.startsWith("env") || source.startsWith("lfo") || source.startsWith("flux"))
        modulators_->select(source.upToFirstOccurrenceOf(".", false, false));
}

const juce::String& ModDock::selected() const { return modulators_->selected(); }
void ModDock::refreshNames() { macros_->refreshNames(); }
void ModDock::reloadShapes() { modulators_->reloadShapes(); }
bool ModDock::tickDock() { return modulators_->tickCard(); }

void ModDock::applySimpleView() {
    macros_->resized();
    modulators_->resized();
}

// --- The keys ----------------------------------------------------------------------------------------

namespace {

// The computer keys in two rows, each laid out like a keyboard: the letters play the white keys and
// the keys above them the black ones. The lower row starts at C of the octave, the upper row an
// octave higher, and the end of the lower row reaches into the start of the upper one.
struct ComputerKey {
    char key;
    int semitone;
};
constexpr ComputerKey kComputerKeys[] = {
    {'Z', 0},  {'S', 1},  {'X', 2},  {'D', 3},  {'C', 4},  {'V', 5},  {'G', 6},  {'B', 7},  {'H', 8},  {'N', 9},  {'J', 10}, {'M', 11},
    {',', 12}, {'L', 13}, {'.', 14}, {';', 15}, {'/', 16},
    {'Q', 12}, {'2', 13}, {'W', 14}, {'3', 15}, {'E', 16}, {'R', 17}, {'5', 18}, {'T', 19}, {'6', 20}, {'Y', 21}, {'7', 22}, {'U', 23},
    {'I', 24}, {'9', 25}, {'O', 26}, {'0', 27}, {'P', 28}, {'[', 29}, {'=', 30}, {']', 31},
};

// The character on the key itself, without Shift (US layout): a shifted key still plays its note.
int unshifted(int c) {
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    switch (c) {
    case '<': return ',';
    case '>': return '.';
    case ':': return ';';
    case '?': return '/';
    case '{': return '[';
    case '}': return ']';
    case '+': return '=';
    case '@': return '2';
    case '#': return '3';
    case '%': return '5';
    case '^': return '6';
    case '&': return '7';
    case '(': return '9';
    case ')': return '0';
    default: return c;
    }
}

// Whether a computer key is down right now. The framework looks most keys up by their character;
// the = key it knows by its shifted character.
bool computerKeyIsDown(int key) {
#if JUCE_WINDOWS
    if (key == '=')
        key = '+';
#endif
    return juce::KeyPress::isKeyCurrentlyDown(key);
}

constexpr const char* kKeysHelp = "Z S X D C V G B H N J M , L . ; / play from C of the octave, Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P [ = ] "
                                  "from C an octave higher; Left and Right move the octave, Down and Up the velocity.";

} // namespace

int PianoKeys::semitoneFor(int character) {
    const int c = unshifted(character);
    for (const auto& k : kComputerKeys)
        if (k.key == c)
            return k.semitone;
    return -1;
}

juce::StringArray PianoKeys::lettersFor(int semitone) {
    juce::StringArray out;
    for (const auto& k : kComputerKeys)
        if (k.semitone == semitone)
            out.add(juce::String::charToString(static_cast<juce::juce_wchar>(k.key)));
    return out;
}

PianoKeys::PianoKeys(Context& ctx) : ctx_(ctx) {
    setTitle("Keyboard");
    setHelp("Keyboard", juce::String("Click or drag to play. The computer keyboard plays too (Computer keys in the preferences): ") + kKeysHelp);
    setWantsKeyboardFocus(false); // clicks leave the keyboard with the editor, so the computer keys keep playing
    family = Family::Play;
    keyIsDown = computerKeyIsDown;
}

PianoKeys::~PianoKeys() { releaseAll(); }

bool PianoKeys::isBlack(int note) {
    const int k = ((note % 12) + 12) % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

int PianoKeys::whiteCount() const { return whiteIndex(highest + 1); }

int PianoKeys::whiteIndex(int note) const {
    int n = 0;
    for (int k = lowest; k < note; ++k)
        if (!isBlack(k))
            ++n;
    return n;
}

juce::Rectangle<float> PianoKeys::keyBounds(int note) const {
    const auto area = getLocalBounds().toFloat();
    const float w = area.getWidth() / static_cast<float>(juce::jmax(1, whiteCount()));
    if (!isBlack(note))
        return {area.getX() + w * static_cast<float>(whiteIndex(note)), area.getY(), w, area.getHeight()};
    const float bw = w * 0.62f;
    return {area.getX() + w * static_cast<float>(whiteIndex(note)) - bw * 0.5f, area.getY(), bw, area.getHeight() * 0.6f};
}

int PianoKeys::keyAt(juce::Point<float> p) const {
    if (!getLocalBounds().toFloat().contains(p))
        return -1;
    for (int n = lowest; n <= highest; ++n)
        if (isBlack(n) && keyBounds(n).contains(p))
            return n;
    for (int n = lowest; n <= highest; ++n)
        if (!isBlack(n) && keyBounds(n).contains(p))
            return n;
    return -1;
}

float PianoKeys::velocityAt(int note, float y) const {
    const auto b = keyBounds(note);
    return juce::jlimit(0.15f, 1.0f, 0.3f + 0.7f * (y - b.getY()) / juce::jmax(1.0f, b.getHeight()));
}

int PianoKeys::octave() const { return ctx_.prefs.keysOctave; }
int PianoKeys::velocity() const { return ctx_.prefs.keysVelocity; }

void PianoKeys::setOctave(int o) {
    o = juce::jlimit(0, 7, o); // at 7 the upper row ends on G9, the last note there is
    if (o == ctx_.prefs.keysOctave)
        return;
    ctx_.prefs.keysOctave = o;
    ctx_.prefs.save();
    repaint();
    if (onSettings)
        onSettings();
}

void PianoKeys::setVelocity(int v) {
    v = juce::jlimit(1, 127, v);
    if (v == ctx_.prefs.keysVelocity)
        return;
    ctx_.prefs.keysVelocity = v;
    ctx_.prefs.save();
    if (onSettings)
        onSettings();
}

void PianoKeys::press(int note, float velocity) {
    if (note < 0 || note > 127)
        return;
    auto& count = holds_[static_cast<std::size_t>(note)];
    if (count++ == 0)
        ctx_.processor.keyboardState().noteOn(1, note, velocity);
}

void PianoKeys::release(int note) {
    if (note < 0 || note > 127)
        return;
    auto& count = holds_[static_cast<std::size_t>(note)];
    if (count == 0)
        return;
    if (--count == 0)
        ctx_.processor.keyboardState().noteOff(1, note, 0.0f);
}

void PianoKeys::releaseAll() {
    mouseNote_ = -1;
    computerHeld_.clear();
    for (int n = 0; n < 128; ++n)
        if (holds_[static_cast<std::size_t>(n)] > 0) {
            holds_[static_cast<std::size_t>(n)] = 0;
            ctx_.processor.keyboardState().noteOff(1, n, 0.0f);
        }
}

int PianoKeys::heldCount() const {
    int n = 0;
    for (auto c : holds_)
        n += c > 0 ? 1 : 0;
    return n;
}

void PianoKeys::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu())
        return;
    if (mouseNote_ >= 0) // a press whose release never came: end it first
        release(std::exchange(mouseNote_, -1));
    const int note = keyAt(e.position);
    if (note >= 0) {
        mouseNote_ = note;
        press(note, velocityAt(note, e.position.y));
    }
}

void PianoKeys::mouseDrag(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu() || !e.mods.isLeftButtonDown())
        return;
    const int note = keyAt(e.position);
    if (note == mouseNote_)
        return;
    if (mouseNote_ >= 0)
        release(std::exchange(mouseNote_, -1));
    if (note >= 0) { // a glissando
        mouseNote_ = note;
        press(note, velocityAt(note, e.position.y));
    }
}

void PianoKeys::mouseUp(const juce::MouseEvent&) {
    if (mouseNote_ >= 0)
        release(std::exchange(mouseNote_, -1));
}

void PianoKeys::visibilityChanged() {
    if (!isVisible())
        releaseAll();
}

bool PianoKeys::computerKeyDown(const juce::KeyPress& key) {
    if (!ctx_.prefs.computerKeys)
        return false;
    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;
    // The octave and the velocity sit on keys that play no note, so playing never moves them.
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) {
        setOctave(octave() + (code == juce::KeyPress::rightKey ? 1 : -1));
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) {
        setVelocity(velocity() + (code == juce::KeyPress::upKey ? 10 : -10));
        return true;
    }
    int character = unshifted(code);
    int semitone = semitoneFor(character);
    if (semitone < 0 && code < 0x10000) { // the typed character when the key code is not one (number-pad keys are never notes)
        character = unshifted(static_cast<int>(key.getTextCharacter()));
        semitone = semitoneFor(character);
    }
    if (semitone < 0)
        return false;
    if (computerHeld_.count(character) != 0)
        return true; // the key repeats while held: one note
    const int note = (octave() + 1) * 12 + semitone;
    if (note > 127)
        return true;
    computerHeld_[character] = note;
    press(note, static_cast<float>(velocity()) / 127.0f);
    return true;
}

void PianoKeys::checkHeld() {
    for (auto it = computerHeld_.begin(); it != computerHeld_.end();) {
        if (keyIsDown == nullptr || keyIsDown(it->first)) {
            ++it;
            continue;
        }
        release(it->second);
        it = computerHeld_.erase(it);
    }
    if (mouseNote_ >= 0 && mouseIsDown != nullptr && !mouseIsDown())
        release(std::exchange(mouseNote_, -1));
}

bool PianoKeys::tick() {
    checkHeld();
    dirty_.clear();
    for (int n = lowest; n <= highest; ++n) {
        const bool down = holds_[static_cast<std::size_t>(n)] > 0 || ctx_.processor.noteDown(n);
        if (down != shown_[static_cast<std::size_t>(n)]) {
            shown_[static_cast<std::size_t>(n)] = down;
            dirty_.add(keyBounds(n).getSmallestIntegerContainer().expanded(1, 0));
        }
    }
    return !dirty_.isEmpty();
}

void PianoKeys::draw(juce::Graphics& g) {
    const Theme& t = theme();
    const auto area = getLocalBounds().toFloat();
    const juce::Colour lit = t.accent(Family::Play);
    const int base = (octave() + 1) * 12;
    // Which computer key plays each note (the preferences can hide this).
    const bool labels = ctx_.prefs.computerKeys && ctx_.prefs.keyLabels;
    const auto lettersOf = [&](int note) { return labels ? lettersFor(note - base) : juce::StringArray(); };
    // White keys: ivory with a soft fall-off towards the player, a groove between each.
    const juce::Colour ivory = t.dark ? juce::Colour(0xffe9e6df) : juce::Colour(0xfffbfaf7);
    for (int n = lowest; n <= highest; ++n) {
        if (isBlack(n))
            continue;
        const auto k = keyBounds(n).reduced(0.5f, 0.0f);
        const bool down = shown_[static_cast<std::size_t>(n)];
        const juce::Colour top = down ? ivory.interpolatedWith(lit, 0.55f) : ivory;
        g.setGradientFill(juce::ColourGradient(top.darker(0.18f), k.getX(), k.getY(), top, k.getX(), k.getY() + k.getHeight() * 0.25f, false));
        g.fillRoundedRectangle(k.withTrimmedTop(-3.0f), 2.5f);
        g.setColour(juce::Colours::black.withAlpha(down ? 0.06f : 0.14f));
        g.fillRect(k.withTop(k.getBottom() - (down ? 2.0f : 4.0f)));
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRect(k.getRight(), k.getY(), 0.8f, k.getHeight());
        // From the bottom up: the octave's name on each C, then the computer keys (two where the rows overlap).
        auto label = k.withTop(k.getBottom() - 15.0f);
        if (n % 12 == 0) {
            drawText(g, "C" + juce::String(n / 12 - 1), label, kTextSmall, juce::Colour(0xff6f737a), juce::Justification::centred);
            label = label.translated(0.0f, -13.0f);
        }
        for (const auto& l : lettersOf(n)) {
            drawText(g, l, label, kTextSmall, juce::Colour(0xff8a8f97), juce::Justification::centred);
            label = label.translated(0.0f, -13.0f);
        }
    }
    // Black keys: lacquer with a lit edge.
    for (int n = lowest; n <= highest; ++n) {
        if (!isBlack(n))
            continue;
        const auto k = keyBounds(n);
        const bool down = shown_[static_cast<std::size_t>(n)];
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.fillRoundedRectangle(k.translated(1.0f, 1.5f), 2.0f);
        const juce::Colour body = down ? lit.darker(0.35f) : juce::Colour(0xff1d1f23);
        g.setGradientFill(juce::ColourGradient(body.brighter(0.25f), k.getX(), k.getY(), body.darker(0.4f), k.getX(), k.getBottom(), false));
        g.fillRoundedRectangle(k, 2.0f);
        g.setColour(juce::Colours::white.withAlpha(down ? 0.1f : 0.16f));
        g.fillRoundedRectangle(k.reduced(2.5f, 0.0f).withTrimmedBottom(k.getHeight() * 0.2f).withTrimmedTop(1.0f).withWidth(1.5f), 0.75f);
        auto label = k.withTop(k.getBottom() - 14.0f);
        for (const auto& l : lettersOf(n)) {
            drawText(g, l, label, kTextSmall, juce::Colour(0xffaeb3ba), juce::Justification::centred);
            label = label.translated(0.0f, -12.0f);
        }
    }
    // The computer keys' range, marked along the top.
    if (labels && base + kSpan - 1 >= lowest && base <= highest) {
        const float x0 = keyBounds(juce::jlimit(lowest, highest, base)).getX();
        const float x1 = keyBounds(juce::jlimit(lowest, highest, base + kSpan - 1)).getRight();
        g.setColour(lit.withAlpha(0.8f));
        g.fillRect(x0 + 1.0f, area.getY(), x1 - x0 - 2.0f, 2.0f);
    }
    // The fallboard's shadow over the keys.
    g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(0.45f), 0.0f, area.getY(), juce::Colours::transparentBlack, 0.0f,
                                           area.getY() + 6.0f, false));
    g.fillRect(area.withHeight(6.0f));
}

// --- The wheels --------------------------------------------------------------------------------------

Wheel::Wheel(Context& ctx, bool pitch) : ctx_(ctx), pitch_(pitch), value_(ctx.processor.screenWheel(pitch)) {
    setWantsKeyboardFocus(true);
    setTitle(pitch ? "Pitch wheel" : "Modulation wheel");
    setHelp(pitch ? "Pitch" : "Mod", pitch ? "Bends the pitch; springs back when released." : "The modulation wheel (controller 1).");
}

// Nobody holds the pitch wheel once the editor closes: the pitch goes back to the centre.
Wheel::~Wheel() {
    if (pitch_)
        ctx_.processor.setScreenWheel(true, 0.0f);
}

void Wheel::draw(juce::Graphics& g) {
    const Theme& t = theme();
    auto area = getLocalBounds().toFloat();
    auto label = area.removeFromBottom(13.0f);
    drawText(g, pitch_ ? "Pitch" : "Mod", label, kTextSmall, t.textDim, juce::Justification::centred);
    const auto slot = area.reduced(2.0f, 1.0f);
    g.setColour(t.sunken);
    g.fillRoundedRectangle(slot, 4.0f);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawRoundedRectangle(slot, 4.0f, 1.0f);
    // The wheel: ridges across a lit drum, its position marked.
    const auto drum = slot.reduced(3.0f, 3.0f);
    const juce::Colour metal = t.dark ? t.raised.brighter(0.1f) : t.raised.darker(0.05f);
    juce::ColourGradient lightOnDrum(metal.darker(0.5f), drum.getX(), drum.getY(), metal.darker(0.5f), drum.getX(), drum.getBottom(), false);
    lightOnDrum.addColour(0.5, metal.brighter(0.25f));
    g.setGradientFill(lightOnDrum);
    g.fillRoundedRectangle(drum, 3.0f);
    const float norm = pitch_ ? (value_ + 1.0f) * 0.5f : value_;
    g.setColour(juce::Colours::black.withAlpha(0.25f));
    for (int i = 0; i < 9; ++i) {
        const float phase = std::fmod(norm * 3.0f + static_cast<float>(i) / 9.0f, 1.0f);
        const float y = drum.getY() + drum.getHeight() * (0.5f - 0.5f * std::cos(phase * juce::MathConstants<float>::pi));
        g.fillRect(drum.getX() + 1.0f, y, drum.getWidth() - 2.0f, 1.0f);
    }
    const float y = drum.getBottom() - drum.getHeight() * norm;
    g.setColour(t.accent(Family::Play));
    g.fillRoundedRectangle(juce::Rectangle<float>(drum.getX(), y - 2.0f, drum.getWidth(), 4.0f), 1.5f);
    drawFocus(g, getLocalBounds().toFloat());
}

void Wheel::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu())
        return;
    held_ = true;
    mouseDrag(e);
}

void Wheel::mouseDrag(const juce::MouseEvent& e) {
    if (!held_)
        return;
    const auto track = getLocalBounds().toFloat().withTrimmedBottom(13.0f).reduced(5.0f, 4.0f);
    const float norm = juce::jlimit(0.0f, 1.0f, (track.getBottom() - e.position.y) / track.getHeight());
    set(pitch_ ? norm * 2.0f - 1.0f : norm);
}

void Wheel::mouseUp(const juce::MouseEvent&) { letGo(); }

// A release the host kept from the window: the next frame sees no button down and lets go.
bool Wheel::tick() {
    if (held_ && mouseIsDown != nullptr && !mouseIsDown())
        letGo();
    return false;
}

void Wheel::letGo() {
    if (!held_)
        return;
    held_ = false;
    if (pitch_)
        set(0.0f); // the pitch wheel springs back, and the pitch with it
}

bool Wheel::keyPressed(const juce::KeyPress& key) {
    const float step = 0.1f;
    if (key == juce::KeyPress::upKey) { set(juce::jmin(1.0f, value_ + step)); return true; }
    if (key == juce::KeyPress::downKey) { set(juce::jmax(pitch_ ? -1.0f : 0.0f, value_ - step)); return true; }
    return false;
}

void Wheel::set(float v) {
    value_ = v;
    ctx_.processor.setScreenWheel(pitch_, v);
    repaint();
}

// --- The keyboard layer ------------------------------------------------------------------------------

KeyboardLayer::KeyboardLayer(Context& ctx) : ctx_(ctx), piano_(ctx) {
    setTitle("Keyboard layer");
    setOpaque(true);
    pitch_ = std::make_unique<Wheel>(ctx, true);
    mod_ = std::make_unique<Wheel>(ctx, false);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&piano_, pitch_.get(), mod_.get(), &octDown_, &octUp_, &velDown_, &velUp_,
                                                                       &octave_, &velocity_})
        addAndMakeVisible(c);
    octave_.justification = velocity_.justification = juce::Justification::centred;
    octDown_.setTitle("Octave down");
    octUp_.setTitle("Octave up");
    velDown_.setTitle("Velocity down");
    velUp_.setTitle("Velocity up");
    for (Button* b : {&octDown_, &octUp_, &velDown_, &velUp_}) {
        b->setTooltip(b->getTitle());
        b->ghost = true;
    }
    octDown_.setHelp("Octave down", "Moves the computer keys an octave down (Left arrow).");
    octUp_.setHelp("Octave up", "Moves the computer keys an octave up (Right arrow).");
    velDown_.setHelp("Velocity down", "Softer computer keys (Down arrow).");
    velUp_.setHelp("Velocity up", "Harder computer keys (Up arrow).");
    octDown_.onClick = [this] { piano_.setOctave(piano_.octave() - 1); };
    octUp_.onClick = [this] { piano_.setOctave(piano_.octave() + 1); };
    velDown_.onClick = [this] { piano_.setVelocity(piano_.velocity() - 10); };
    velUp_.onClick = [this] { piano_.setVelocity(piano_.velocity() + 10); };
    piano_.onSettings = [this] { refresh(); };
    refresh();
}

void KeyboardLayer::allNotesOff() {
    piano_.releaseAll();
    ctx_.processor.keyboardState().allNotesOff(0);
    ctx_.processor.pushUiEvent(zed::MidiEvent::controller(0, 123, 0.0f));
    ctx_.processor.pushUiEvent(zed::MidiEvent::controller(0, 120, 0.0f));
    if (ctx_.notify)
        ctx_.notify("All notes off.");
}

void KeyboardLayer::refresh() {
    octave_.setText("Oct " + juce::String(piano_.octave()));
    velocity_.setText("Vel " + juce::String(piano_.velocity()));
    octave_.setTitle("Computer keys: Z plays C" + juce::String(piano_.octave()) + ", Q plays C" + juce::String(piano_.octave() + 1));
    velocity_.setTitle("Computer keys velocity " + juce::String(piano_.velocity()));
    piano_.repaint();
}

void KeyboardLayer::resized() {
    auto area = getLocalBounds().reduced(8, 4);
    auto panel = area.reduced(8, 6);
    pitch_->setBounds(panel.removeFromLeft(26));
    panel.removeFromLeft(4);
    mod_->setBounds(panel.removeFromLeft(26));
    panel.removeFromLeft(8);
    auto column = panel.removeFromLeft(96);
    const auto row = [](juce::Rectangle<int> r, Button& down, Caption& text, Button& up) {
        down.setBounds(r.removeFromLeft(22));
        up.setBounds(r.removeFromRight(22));
        text.setBounds(r);
    };
    const int h = column.getHeight() / 2;
    row(column.removeFromTop(h).withSizeKeepingCentre(96, 22), octDown_, octave_, octUp_);
    row(column.withSizeKeepingCentre(96, 22), velDown_, velocity_, velUp_);
    panel.removeFromLeft(8);
    piano_.setBounds(panel); // the keys run to the right edge
}

void KeyboardLayer::paint(juce::Graphics& g) {
    background_.draw(g, getLocalBounds(), static_cast<std::uint64_t>(piano_.getX()), true, [this](juce::Graphics& bg) {
        const Theme& t = theme();
        drawChassis(bg, getLocalBounds().toFloat());
        const auto area = getLocalBounds().reduced(8, 4).toFloat();
        drawPanel(bg, area, t.panel);
        drawScrew(bg, {area.getX() + 7.0f, area.getY() + 7.0f}, 2.8f, 0.2f);
        drawScrew(bg, {area.getX() + 7.0f, area.getBottom() - 7.0f}, 2.8f, 0.7f);
        drawScrew(bg, {area.getRight() - 7.0f, area.getY() + 7.0f}, 2.8f, 1.1f);
        drawScrew(bg, {area.getRight() - 7.0f, area.getBottom() - 7.0f}, 2.8f, 0.4f);
        // The keys sit in a recessed bed.
        const auto bed = piano_.getBounds().toFloat().expanded(2.0f, 2.0f);
        bg.setColour(juce::Colours::black.withAlpha(t.dark ? 0.6f : 0.3f));
        bg.fillRoundedRectangle(bed, 3.0f);
    });
}

} // namespace zui
