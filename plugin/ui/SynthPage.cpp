#include "SynthPage.h"

#include "Audit.h"
#include "Displays.h"

#include "zederia/engine/Filter.h"
#include "zederia/patch/Modules.h"
#include "zederia/tuning/Tuning.h"

#include <map>
#include <nlohmann/json.hpp>

namespace zui {

namespace {

const zed::Registry& reg() { return zed::Registry::get(); }
int idx(const juce::String& id) { return reg().indexOf(id.toStdString()); }

juce::String filterGroup(const juce::String& t) {
    if (t.startsWith("svf")) return "State-variable";
    if (t.startsWith("ladder")) return "Ladder";
    if (t.startsWith("dual")) return "Dual";
    if (t.startsWith("morph")) return "Morph";
    if (t.startsWith("comb")) return "Comb";
    if (t.startsWith("flange")) return "Flange";
    if (t.startsWith("phase")) return "Phase";
    if (t.startsWith("formant")) return "Formant";
    if (t == "low_shelf" || t == "high_shelf" || t == "bell") return "Shelf and bell";
    return "Special";
}

juce::String warpGroup(const juce::String& t) {
    if (t == "off") return "Off";
    if (t.contains("sync")) return "Sync";
    if (t.startsWith("bend")) return "Bend";
    if (t == "pulse_width" || t.startsWith("skew")) return "Width and skew";
    if (t == "mirror" || t == "flip" || t == "quantize") return "Mirror, flip, quantize";
    if (t.startsWith("curve")) return "Drawn curve";
    if (t == "fold" || t == "drive") return "Fold and drive";
    return "From another source";
}

// What the filter's Character knob does for a type (plan 3.5).
juce::String characterLabel(int type) {
    using F = zed::flt::Family;
    switch (zed::flt::typeInfo(type).family) {
    case F::Svf: return "Body";
    case F::Ladder: return "Heat";
    case F::Dual: return "Offset";
    case F::Morph: return "Morph";
    case F::Comb:
    case F::Flange:
    case F::Phase: return "Damping";
    case F::Formant: return "Shift";
    case F::Shelf: return "Gain";
    case F::RingMod: return "Freq";
    case F::SampleHold: return "Rate";
    case F::AllpassCluster: return "Spread";
    case F::Diffuser: return "Size";
    case F::BandReject: return "Width";
    case F::Howl: return "Howl";
    case F::GritComb: return "Grit";
    }
    return "Character";
}

// Tabs over groups of controls: only the selected group is visible.
struct Sections {
    TabStrip* tabs = nullptr;
    std::vector<std::vector<FlowItem>> groups;
    std::vector<bool> advancedTab;
    int rowHeight = kKnobH;
    void layout(juce::Rectangle<int> area, bool simple) {
        if (tabs != nullptr) {
            for (int i = 0; i < tabs->count(); ++i)
                tabs->setTabVisible(i, !(simple && i < static_cast<int>(advancedTab.size()) && advancedTab[static_cast<std::size_t>(i)]));
            if (!tabs->tab(tabs->selected()).isVisible())
                tabs->setSelected(0, false);
            tabs->setBounds(area.removeFromTop(24));
            area.removeFromTop(6);
        }
        const int current = tabs != nullptr ? tabs->selected() : 0;
        for (int g = 0; g < static_cast<int>(groups.size()); ++g)
            for (auto& item : groups[static_cast<std::size_t>(g)]) {
                bool show = g == current;
                if (auto* w = dynamic_cast<Widget*>(item.c))
                    show = show && shownInView(*w, simple);
                item.c->setVisible(show);
            }
        if (current < static_cast<int>(groups.size()))
            flow(area, groups[static_cast<std::size_t>(current)], rowHeight, 4);
    }
    void hideAll() {
        if (tabs != nullptr)
            tabs->setVisible(false);
        for (auto& g : groups)
            for (auto& item : g)
                item.c->setVisible(false);
    }
};

// A view of a source's output over time, for sources without a shape to draw (Flux, MIDI ...).
class SourceScope final : public Widget {
public:
    explicit SourceScope(Context& ctx) : ctx_(ctx) {
        setTitle("Source output");
        setHelp("Output", "The modulator's output over the last seconds.");
        history_.assign(240, 0.0f);
    }
    juce::String source;
    bool tick() override {
        const int i = Context::sourceIndex(source);
        const float v = i >= 0 ? ctx_.processor.engine().sourceValue(i) : 0.0f;
        history_[static_cast<std::size_t>(pos_)] = v;
        pos_ = (pos_ + 1) % static_cast<int>(history_.size());
        const bool moved = std::abs(v - last_) > 1e-4f || std::abs(v) > 1e-4f;
        last_ = v;
        return moved;
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        const auto area = getLocalBounds().toFloat();
        drawWell(g, area);
        const auto plot = area.reduced(8.0f);
        g.setColour(t.lineStrong);
        g.fillRect(plot.getX(), plot.getCentreY(), plot.getWidth(), 1.0f);
        juce::Path p;
        const int n = static_cast<int>(history_.size());
        for (int i = 0; i < n; ++i) {
            const float v = history_[static_cast<std::size_t>((pos_ + i) % n)];
            const float x = plot.getX() + plot.getWidth() * static_cast<float>(i) / static_cast<float>(n - 1);
            const float y = plot.getCentreY() - juce::jlimit(-1.0f, 1.0f, v) * plot.getHeight() * 0.45f;
            if (i == 0) p.startNewSubPath(x, y); else p.lineTo(x, y);
        }
        strokeGlow(g, p, ctx_.sourceColour(source), 1.6f);
        drawText(g, Context::sourceName(source) + "  " + juce::String(last_, 2), area.reduced(10.0f, 6.0f).removeFromTop(14.0f), kTextSmall,
                 t.textDim, juce::Justification::centredRight);
    }
    bool hasVisibleLabel() const override { return false; }

private:
    Context& ctx_;
    std::vector<float> history_;
    int pos_ = 0;
    float last_ = 0.0f;
};

// Pitch and modulation wheels beside the keyboard.
class Wheel final : public Widget {
public:
    Wheel(Context& ctx, bool pitch) : ctx_(ctx), pitch_(pitch) {
        setWantsKeyboardFocus(true);
        setTitle(pitch ? "Pitch wheel" : "Modulation wheel");
        setHelp(pitch ? "Pitch" : "Mod", pitch ? "Bends the pitch; springs back when released." : "The modulation wheel (controller 1).");
    }
    void draw(juce::Graphics& g) override {
        const Theme& t = theme();
        auto area = getLocalBounds().toFloat();
        auto label = area.removeFromBottom(13.0f);
        drawText(g, pitch_ ? "Pitch" : "Mod", label, kTextSmall, t.textDim, juce::Justification::centred);
        const auto track = area.reduced(3.0f, 2.0f);
        g.setColour(t.sunken);
        g.fillRoundedRectangle(track, 4.0f);
        g.setColour(t.line);
        g.drawRoundedRectangle(track, 4.0f, 1.0f);
        const float norm = pitch_ ? (value_ + 1.0f) * 0.5f : value_;
        const float y = track.getBottom() - track.getHeight() * norm;
        g.setColour(t.modulators);
        g.fillRoundedRectangle(juce::Rectangle<float>(track.getX() + 2.0f, y - 3.0f, track.getWidth() - 4.0f, 6.0f), 2.0f);
        drawFocus(g, getLocalBounds().toFloat());
    }
    void mouseDown(const juce::MouseEvent& e) override { mouseDrag(e); }
    void mouseDrag(const juce::MouseEvent& e) override {
        const auto track = getLocalBounds().toFloat().withTrimmedBottom(13.0f).reduced(3.0f, 2.0f);
        const float norm = juce::jlimit(0.0f, 1.0f, (track.getBottom() - e.position.y) / track.getHeight());
        set(pitch_ ? norm * 2.0f - 1.0f : norm);
    }
    void mouseUp(const juce::MouseEvent&) override {
        if (pitch_)
            set(0.0f);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        const float step = 0.1f;
        if (key == juce::KeyPress::upKey) { set(juce::jmin(1.0f, value_ + step)); return true; }
        if (key == juce::KeyPress::downKey) { set(juce::jmax(pitch_ ? -1.0f : 0.0f, value_ - step)); return true; }
        return false;
    }
    void set(float v) {
        value_ = v;
        ctx_.processor.pushUiEvent(pitch_ ? zed::MidiEvent::pitchBend(0, v) : zed::MidiEvent::controller(0, 1, v));
        repaint();
    }

private:
    Context& ctx_;
    bool pitch_;
    float value_ = 0.0f;
};

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

// --- Source cards --------------------------------------------------------------------------------------

class SourceCard final : public Card {
public:
    SourceCard(Context& ctx, SynthPage& page, int source)
        : Card(source < 3 ? "Osc " + juce::String(source + 1) : source == 3 ? "Sub" : "Texture", Family::Sources), ctx_(ctx), page_(page), source_(source) {
        prefix_ = source < 3 ? "osc" + juce::String(source + 1) : source == 3 ? "sub" : "texture";
        moduleId = prefix_;
        collapsible = true;
        auto& power = parts_.add<Toggle>(*this, ctx, (prefix_ + ".on").toRawUTF8(), getTitle() + " on");
        power.power = true;
        power.family = Family::Sources;
        powerSwitch = &power;
        wave_ = &parts_.add<WaveMini>(*this, ctx, source);
        wave_->setSize(64, 20);
        headerRight.push_back(wave_);
        onHeaderClick = [this] { page_.expandSource(source_); };
        onCollapse = [this](bool c) {
            if (!c)
                page_.expandSource(source_);
            else
                page_.resized();
        };
        if (source < 3)
            buildOsc();
        else if (source == 3)
            buildSub();
        else
            buildTexture();
        refresh();
    }

    int preferredHeight() const {
        if (collapsed)
            return 30;
        return source_ < 3 ? 276 : source_ == 3 ? 198 : 232;
    }

    void refresh() {
        if (tableName_ == nullptr)
            return;
        juce::String name;
        bool modified = false;
        ctx_.processor.readDocument([&](const zed::Patch& p) {
            if (source_ < 3) {
                const auto k = static_cast<std::size_t>(source_);
                name = str(p.tables[k]);
                modified = static_cast<bool>(p.tableData[k]);
                if (modified && p.tableData[k].table != nullptr && name.isEmpty())
                    name = str(p.tableData[k].table->name);
            } else {
                name = str(p.textureSample);
            }
        });
        juce::String shown = name.fromLastOccurrenceOf("/", false, false).fromLastOccurrenceOf(":", false, false);
        if (shown.isEmpty())
            shown = source_ < 3 ? "Basic Shapes" : "White Noise";
        if (modified)
            shown += " (edited)";
        tableName_->setText(shown);
        tableName_->setTitle((source_ < 3 ? "Table of " : "Sample of ") + getTitle() + ": " + shown);
    }

    void layoutBody(juce::Rectangle<int> body) override {
        const bool simple = ctx_.simpleView();
        const bool show = !collapsed;
        for (auto* c : bodyParts_)
            c->setVisible(show);
        if (!show) {
            sections_.hideAll();
            return;
        }
        if (tableName_ != nullptr) {
            auto row = body.removeFromTop(24);
            if (engine_ != nullptr)
                engine_->setBounds(row.removeFromRight(118));
            row.removeFromRight(6);
            prevTable_->setBounds(row.removeFromLeft(22));
            nextTable_->setBounds(row.removeFromRight(22));
            tableName_->setBounds(row.reduced(2, 0));
            body.removeFromTop(6);
        }
        sections_.layout(body, simple);
        // Classic-only controls follow the engine switch.
        if (classicParts_.size() == 2 && engine_ != nullptr) {
            const bool classic = ctx_.processor.plainValue(engine_->param()) >= 0.5;
            for (auto* c : classicParts_)
                c->setVisible(c->isVisible() && classic);
        }
    }

    bool tickCard() {
        if (engine_ == nullptr)
            return false;
        const bool classic = ctx_.processor.plainValue(engine_->param()) >= 0.5;
        if (classic != lastClassic_) {
            lastClassic_ = classic;
            resized();
            return true;
        }
        return false;
    }

private:
    Knob& knob(const juce::String& suffix, const juce::String& label, bool adv = false) {
        auto& k = parts_.add<Knob>(*this, ctx_, (prefix_ + "." + suffix).toRawUTF8(), label);
        k.family = Family::Sources;
        k.advanced = adv;
        bodyParts_.push_back(&k);
        return k;
    }
    template <class T>
    T& part(const juce::String& suffix, const juce::String& label, bool adv = false) {
        auto& p = parts_.add<T>(*this, ctx_, (prefix_ + "." + suffix).toRawUTF8(), label);
        p.family = Family::Sources;
        p.advanced = adv;
        bodyParts_.push_back(&p);
        return p;
    }

    void addTableRow(bool texture) {
        prevTable_ = &parts_.add<Button>(*this, "", Glyph::Prev);
        nextTable_ = &parts_.add<Button>(*this, "", Glyph::Next);
        tableName_ = &parts_.add<Button>(*this, "Basic Shapes");
        for (Button* b : {prevTable_, nextTable_})
            b->ghost = true;
        prevTable_->setTitle(texture ? "Previous sample" : "Previous table");
        nextTable_->setTitle(texture ? "Next sample" : "Next table");
        prevTable_->setTooltip(prevTable_->getTitle());
        nextTable_->setTooltip(nextTable_->getTitle());
        tableName_->textSize = kTextSmall + 0.5f;
        tableName_->setHelp(texture ? "Sample" : "Table", texture ? "The sample the Texture player plays; click to choose another or load a file."
                                                                  : "The wavetable this oscillator plays; click to choose another, load a file or open it in the workshop.");
        tableName_->onClick = [this, texture] { chooseAsset(texture); };
        prevTable_->onClick = [this, texture] { stepAsset(texture, -1); };
        nextTable_->onClick = [this, texture] { stepAsset(texture, 1); };
        for (Button* b : {prevTable_, nextTable_, tableName_})
            bodyParts_.push_back(b);
    }

    std::vector<std::string> assets(bool texture) const {
        const auto library = ctx_.processor.library();
        std::vector<std::string> list = texture ? library.listTextures() : library.listTables();
        list.insert(list.begin(), std::string());
        return list;
    }

    void setAsset(bool texture, const std::string& ref) {
        const juce::String name = ref.empty() ? (texture ? "White Noise" : "Basic Shapes") : str(ref).fromLastOccurrenceOf("/", false, false).fromLastOccurrenceOf(":", false, false);
        ctx_.session.edit(getTitle() + (texture ? " sample: " : " table: ") + name, [&](zed::Patch& p) {
            if (texture) {
                p.textureSample = ref;
            } else {
                p.tables[static_cast<std::size_t>(source_)] = ref;
                p.tableData[static_cast<std::size_t>(source_)] = {};
            }
            return true;
        });
        refresh();
    }

    void stepAsset(bool texture, int direction) {
        const auto list = assets(texture);
        std::string current;
        ctx_.processor.readDocument([&](const zed::Patch& p) { current = texture ? p.textureSample : p.tables[static_cast<std::size_t>(source_)]; });
        auto it = std::find(list.begin(), list.end(), current);
        const int n = static_cast<int>(list.size());
        const int at = it == list.end() ? 0 : static_cast<int>(it - list.begin());
        setAsset(texture, list[static_cast<std::size_t>(((at + direction) % n + n) % n)]);
    }

    void chooseAsset(bool texture) {
        juce::PopupMenu menu;
        const auto list = assets(texture);
        juce::PopupMenu factory, user;
        for (const auto& ref : list) {
            const juce::String r = str(ref);
            const juce::String name = r.isEmpty() ? (texture ? "White Noise (built in)" : "Basic Shapes (built in)") : r.fromFirstOccurrenceOf(":", false, false);
            auto& target = r.startsWith("user:") ? user : r.startsWith("factory:") ? factory : menu;
            target.addItem(name, [this, texture, ref] { setAsset(texture, ref); });
        }
        if (factory.getNumItems() > 0)
            menu.addSubMenu("Factory", factory);
        if (user.getNumItems() > 0)
            menu.addSubMenu("Yours", user);
        menu.addSeparator();
        menu.addItem("Load a file...", [this, texture] {
            auto chooser = std::make_shared<juce::FileChooser>(texture ? "Load a sample" : "Load a wavetable", ctx_.prefs.userContentFolder(), "*.wav");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, chooser, texture](const juce::FileChooser& fc) {
                if (fc.getResult().existsAsFile())
                    setAsset(texture, fc.getResult().getFullPathName().toStdString());
            });
        });
        if (!texture)
            menu.addItem("Edit in the workshop (TABLE page)", [this] { ctx_.reveal("table" + juce::String(source_ + 1)); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(tableName_));
    }

    void buildOsc() {
        addTableRow(false);
        engine_ = &part<Segmented>("engine", "Engine");
        engine_->names = {"Table", "Classic"};
        engine_->showLabel = false;
        engine_->labelledBy = "Engine";
        auto& tabs = parts_.add<TabStrip>(*this, juce::StringArray{"MAIN", "WARP", "UNISON", "PHASE"});
        tabs.family = Family::Sources;
        tabs.setTitle(getTitle() + " sections");
        tabs.onChange = [this](int) { resized(); };
        bodyParts_.push_back(&tabs);
        sections_.tabs = &tabs;
        sections_.advancedTab = {false, false, false, true};
        auto& octave = part<NumberBox>("octave", "Octave");
        auto& semi = part<NumberBox>("semi", "Semi");
        auto& keytrack = part<Toggle>("keytrack", "Key track", true);
        auto& shape = knob("classic.shape", "Shape");
        auto& width = knob("classic.pulse_width", "Width");
        classicParts_ = {&shape, &width};
        sections_.groups.push_back({{&knob("position", "Position"), kKnobW}, {&knob("level", "Level"), kKnobW}, {&knob("pan", "Pan"), kKnobW},
                                    {&knob("fine", "Fine"), kKnobW}, {&knob("coarse", "Coarse", true), kKnobW},
                                    {&octave, 58, 38, true}, {&semi, 58, 38}, {&keytrack, 90, 22},
                                    {&knob("sends.filter_balance", "F1 / F2", true), kKnobW, 0, true}, {&knob("sends.direct", "Direct", true), kKnobW},
                                    {&shape, kKnobW}, {&width, kKnobW}});
        const auto warp = [&](const char* slot, const juce::String& name) {
            auto& mode = part<ChoiceBox>(juce::String(slot) + ".mode", name);
            mode.groupOf = warpGroup;
            auto& source = part<ChoiceBox>(juce::String(slot) + ".source", "Mod source", true);
            auto& amount = knob(juce::String(slot) + ".amount", "Amount");
            auto& curve = parts_.add<Button>(*this, "Edit curve", Glyph::Pencil);
            curve.textSize = kTextSmall;
            curve.setHelp("Edit curve", "Draw this warp's phase curve on the Stage (the curve modes use it).");
            const int slotIndex = juce::String(slot) == "warp_a" ? 0 : 1;
            curve.onClick = [this, slotIndex] { page_.focusWarpCurve(source_, slotIndex); };
            bodyParts_.push_back(&curve);
            return std::vector<FlowItem>{{&mode, 140, kChoiceH, true}, {&amount, kKnobW}, {&source, 100, kChoiceH}};
        };
        auto warpA = warp("warp_a", "Warp A");
        auto warpB = warp("warp_b", "Warp B");
        std::vector<FlowItem> warpItems = warpA;
        warpItems.insert(warpItems.end(), warpB.begin(), warpB.end());
        // The two "Edit curve" buttons sit after each pair.
        Button* curveA = nullptr;
        Button* curveB = nullptr;
        for (auto* c : bodyParts_)
            if (auto* b = dynamic_cast<Button*>(c); b != nullptr && b->text() == "Edit curve")
                (curveA == nullptr ? curveA : curveB) = b;
        warpItems.insert(warpItems.begin() + 3, {curveA, 96, 24});
        warpItems.push_back({curveB, 96, 24});
        sections_.groups.push_back(warpItems);
        auto& voices = part<NumberBox>("unison.voices", "Voices");
        voices.costWarning = true;
        sections_.groups.push_back({{&voices, 58, 38}, {&knob("unison.detune", "Detune"), kKnobW}, {&knob("unison.range", "Range"), kKnobW},
                                    {&knob("unison.blend", "Blend"), kKnobW}, {&knob("unison.width", "Width"), kKnobW},
                                    {&knob("unison.position_spread", "Pos sprd", true), kKnobW, 0, true}, {&knob("unison.warp_spread", "Warp sprd", true), kKnobW},
                                    {&part<ChoiceBox>("unison.law", "Spread law", true), 100, kChoiceH},
                                    {&part<ChoiceBox>("unison.stack", "Stack", true), 140, kChoiceH, true}, {&part<ChoiceBox>("unison.phase", "Start phase", true), 110, kChoiceH}});
        auto& phaseMode = part<Segmented>("phase_mode", "Phase mode");
        phaseMode.names = {"Reset", "Continue"};
        sections_.groups.push_back({{&knob("phase", "Phase"), kKnobW}, {&knob("phase_random", "Random"), kKnobW}, {&phaseMode, 140, kChoiceH}});
    }

    void buildSub() {
        auto& octave = part<NumberBox>("octave", "Octave");
        auto& direct = part<Toggle>("direct", "Direct out");
        sections_.groups.push_back({{&knob("level", "Level"), kKnobW}, {&knob("pan", "Pan"), kKnobW}, {&knob("coarse", "Coarse", true), kKnobW},
                                    {&knob("fine", "Fine"), kKnobW}, {&octave, 58, 38},
                                    {&part<ChoiceBox>("shape", "Shape"), 130, kChoiceH, true}, {&direct, 96, 22}, {&knob("sends.filter_balance", "F1 / F2", true), kKnobW}});
    }

    void buildTexture() {
        addTableRow(true);
        auto& mode = part<Segmented>("mode", "Mode");
        mode.names = {"One shot", "Loop"};
        sections_.groups.push_back({{&knob("level", "Level"), kKnobW}, {&knob("pan", "Pan"), kKnobW}, {&knob("pitch", "Pitch"), kKnobW},
                                    {&knob("fine", "Fine", true), kKnobW}, {&knob("start", "Start"), kKnobW},
                                    {&knob("random_start", "Rnd start"), kKnobW, 0, true}, {&mode, 130, kChoiceH},
                                    {&part<Toggle>("keytrack", "Key track"), 90, 22}, {&part<Toggle>("direct", "Direct"), 74, 22},
                                    {&knob("sends.filter_balance", "F1 / F2", true), kKnobW}});
    }

    Context& ctx_;
    SynthPage& page_;
    int source_;
    juce::String prefix_;
    Parts parts_;
    WaveMini* wave_ = nullptr;
    Button *prevTable_ = nullptr, *nextTable_ = nullptr, *tableName_ = nullptr;
    Segmented* engine_ = nullptr;
    Sections sections_;
    std::vector<juce::Component*> bodyParts_;
    std::vector<juce::Component*> classicParts_;
    bool lastClassic_ = false;
};

// --- Filter cards ------------------------------------------------------------------------------------

class FilterCard final : public Card {
public:
    FilterCard(Context& ctx, SynthPage& page, int slot) : Card("Filter " + juce::String(slot + 1), Family::Filters), ctx_(ctx), page_(page), slot_(slot) {
        prefix_ = "filter" + juce::String(slot + 1);
        moduleId = prefix_;
        auto& power = parts_.add<Toggle>(*this, ctx, (prefix_ + ".on").toRawUTF8(), getTitle() + " on");
        power.power = true;
        power.family = Family::Filters;
        powerSwitch = &power;
        type_ = &parts_.add<ChoiceBox>(*this, ctx, (prefix_ + ".type").toRawUTF8(), "Type");
        type_->showLabel = false;
        type_->labelledBy = getTitle();
        type_->family = Family::Filters;
        type_->groupOf = filterGroup;
        type_->setSize(170, 20);
        headerRight.push_back(type_);
        onHeaderClick = [this] { page_.focusFilter(slot_); };
        const auto k = [&](const char* suffix, const char* label, bool adv = false) -> Knob& {
            auto& w = parts_.add<Knob>(*this, ctx, (prefix_ + "." + suffix).toRawUTF8(), label);
            w.family = Family::Filters;
            w.advanced = adv;
            return w;
        };
        character_ = &k("character", "Body");
        auto& mix = parts_.add<Fader>(*this, ctx, (prefix_ + ".mix").toRawUTF8(), "Mix");
        mix.family = Family::Filters;
        auto& mode = parts_.add<Segmented>(*this, ctx, (prefix_ + ".mix_mode").toRawUTF8(), "Mix mode");
        mode.names = {"Mix", "Level"};
        mode.showLabel = false;
        mode.labelledBy = "Mix";
        mode.family = Family::Filters;
        mode.advanced = true;
        items_ = {{&k("cutoff", "Cutoff"), 52}, {&k("resonance", "Res"), 52}, {&k("drive", "Drive"), 52}, {character_, 52},
                  {&k("stereo_offset", "Stereo", true), 52}, {&k("keytrack", "Key track"), 52},
                  {&mix, 200, 22, true}, {&mode, 104, 22}};
    }
    void layoutBody(juce::Rectangle<int> body) override {
        for (auto& item : items_)
            if (auto* w = dynamic_cast<Widget*>(item.c))
                w->setVisible(shownInView(*w, ctx_.simpleView()));
        flow(body, items_, kKnobH, 3);
    }
    bool tickCard() {
        const int type = static_cast<int>(ctx_.processor.plainValue(type_->param()));
        if (type != lastType_) {
            lastType_ = type;
            character_->setLabel(characterLabel(type));
            return true;
        }
        return false;
    }

private:
    Context& ctx_;
    SynthPage& page_;
    int slot_;
    juce::String prefix_;
    Parts parts_;
    ChoiceBox* type_ = nullptr;
    Knob* character_ = nullptr;
    std::vector<FlowItem> items_;
    int lastType_ = -1;
};

// --- Voice card: routing, voice, glide, character, tuning, MIDI, output -------------------------------------

class VoiceCard final : public Card {
public:
    VoiceCard(Context& ctx) : Card("Voice", Family::Neutral), ctx_(ctx) {
        moduleId = "voice";
        auto& tabs = parts_.add<TabStrip>(*this, juce::StringArray{"ROUTE", "VOICE", "GLIDE", "CHAR", "TUNE", "MIDI", "OUT"});
        tabs.setTitle("Voice sections");
        tabs.onChange = [this](int) { resized(); };
        sections_.tabs = &tabs;
        sections_.advancedTab = {false, false, false, false, true, true, true};
        const auto knob = [&](const char* id, const char* label, Family f = Family::Neutral) -> Knob& {
            auto& k = parts_.add<Knob>(*this, ctx, id, label);
            k.family = f;
            return k;
        };
        const auto fader = [&](const char* id, const char* label) -> Fader& {
            auto& f = parts_.add<Fader>(*this, ctx, id, label);
            f.labelWidth = 120;
            return f;
        };
        auto& routing = fader("voice.filter_routing", "Series / parallel");
        routing.family = Family::Filters;
        sections_.groups.push_back({{&routing, 320, 22, true}, {&fader("fx.filter1_balance", "F1 to lane B"), 320, 22, true},
                                    {&fader("fx.filter2_balance", "F2 to lane B"), 320, 22, true}, {&fader("fx.direct_balance", "Direct to lane B"), 320, 22, true}});
        auto& mode = parts_.add<Segmented>(*this, ctx, "global.mode", "Mode");
        mode.names = {"Poly", "Mono", "Legato"};
        voices_ = &parts_.add<Caption>(*this, "0 voices", kTextSmall, true);
        sections_.groups.push_back({{&mode, 170, kChoiceH}, {&parts_.add<NumberBox>(*this, ctx, "global.polyphony", "Polyphony"), 70, kChoiceH},
                                    {voices_, 70, 18}, {&parts_.add<ChoiceBox>(*this, ctx, "voice.steal", "Stealing"), 110, kChoiceH, true},
                                    {&parts_.add<ChoiceBox>(*this, ctx, "voice.priority", "Priority"), 90, kChoiceH},
                                    {&knob("voice.velocity_sens", "Velocity"), kKnobW}});
        auto& glideMode = parts_.add<Segmented>(*this, ctx, "global.glide_mode", "Glide mode");
        glideMode.names = {"Always", "Legato"};
        sections_.groups.push_back({{&knob("global.glide", "Glide"), kKnobW}, {&knob("global.glide_curve", "Curve"), kKnobW},
                                    {&glideMode, 130, kChoiceH}, {&parts_.add<Toggle>(*this, ctx, "global.glide_scaled", "Per octave"), 100, 22, true}});
        auto& variance = parts_.add<Caption>(*this, "Variance per note", kTextSmall, true);
        sections_.groups.push_back({{&knob("voice.drift", "Drift"), kKnobW}, {&knob("voice.warmth", "Warmth"), kKnobW},
                                    {&knob("voice.noise_floor", "Noise"), kKnobW}, {&variance, 120, 14, true},
                                    {&knob("voice.variance.cutoff", "Cutoff"), kKnobW, 0, true}, {&knob("voice.variance.env", "Env time"), kKnobW},
                                    {&knob("voice.variance.pan", "Pan"), kKnobW}, {&knob("voice.variance.level", "Level"), kKnobW}});
        tuningName_ = &parts_.add<Caption>(*this, "Equal temperament", kTextSmall, true);
        auto& load = parts_.add<Button>(*this, "Load tuning", Glyph::Folder);
        load.textSize = kTextSmall;
        load.setHelp("Load tuning", "Scala (.scl, with an optional .kbm beside it) or .tun files.");
        load.onClick = [this] { loadTuning(); };
        auto& equal = parts_.add<Button>(*this, "Equal");
        equal.textSize = kTextSmall;
        equal.setHelp("Equal temperament", "Back to twelve equal steps per octave.");
        equal.onClick = [this] { ctx_.session.edit("Equal temperament", [](zed::Patch& p) { p.tuning = {}; return true; }); };
        lockTuning_ = &parts_.add<Button>(*this, "Lock");
        lockTuning_->toggleRole = true;
        lockTuning_->textSize = kTextSmall;
        lockTuning_->setHelp("Lock tuning", "Keeps this tuning when presets load.");
        lockTuning_->onClick = [this] {
            const bool on = !ctx_.processor.engine().tuningLocked();
            ctx_.processor.engine().setTuningLocked(on);
            lockTuning_->setToggled(on);
        };
        sections_.groups.push_back({{&knob("global.master_tune", "Tune"), kKnobW}, {&knob("global.reference", "A ="), kKnobW},
                                    {&knob("global.bend_up", "Bend up"), kKnobW}, {&knob("global.bend_down", "Bend dn"), kKnobW},
                                    {&load, 104, 22, true}, {&equal, 56, 22}, {lockTuning_, 54, 22}, {tuningName_, 300, 16, true}});
        auto& follower = parts_.add<Segmented>(*this, ctx, "global.follower.source", "Follower hears");
        follower.names = {"Instrument", "Side-chain"};
        sections_.groups.push_back({{&parts_.add<Toggle>(*this, ctx, "global.mpe", "MPE"), 64, 22}, {&knob("global.mpe_bend_range", "MPE bend"), kKnobW},
                                    {&parts_.add<Toggle>(*this, ctx, "global.midi_out", "MIDI out"), 84, 22}, {&parts_.add<Toggle>(*this, ctx, "global.midi_thru", "MIDI thru"), 90, 22},
                                    {&follower, 170, kChoiceH, true}, {&knob("global.follower.attack", "Attack"), kKnobW}, {&knob("global.follower.release", "Release"), kKnobW}});
        sections_.groups.push_back({{&parts_.add<ChoiceBox>(*this, ctx, "global.limiter", "Protection"), 110, kChoiceH},
                                    {&parts_.add<Toggle>(*this, ctx, "global.dc_filter", "DC filter"), 90, 22},
                                    {&parts_.add<ChoiceBox>(*this, ctx, "global.quality", "Quality"), 100, kChoiceH, true},
                                    {&parts_.add<ChoiceBox>(*this, ctx, "global.quality_offline", "Offline"), 100, kChoiceH}});
    }
    void layoutBody(juce::Rectangle<int> body) override { sections_.layout(body, ctx_.simpleView()); }
    bool tickCard() {
        const int v = ctx_.processor.activeVoices();
        if (v != voicesShown_) {
            voicesShown_ = v;
            voices_->setText(juce::String(v) + (v == 1 ? " voice" : " voices"));
        }
        return false;
    }
    void refresh() {
        juce::String name;
        ctx_.processor.readDocument([&](const zed::Patch& p) { name = p.tuning.empty() ? juce::String("Equal temperament") : str(p.tuning.name); });
        tuningName_->setText(name.isEmpty() ? juce::String("A tuning") : name);
        lockTuning_->setToggled(ctx_.processor.engine().tuningLocked());
    }

private:
    void loadTuning() {
        auto chooser = std::make_shared<juce::FileChooser>("Load a tuning", ctx_.prefs.userContentFolder(), "*.scl;*.tun");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, chooser](const juce::FileChooser& fc) {
            const juce::File file = fc.getResult();
            if (!file.existsAsFile())
                return;
            const juce::File kbm = file.withFileExtension("kbm");
            zed::Tuning tuning;
            std::string error;
            if (!zed::loadTuningFiles(file.getFullPathName().toStdString(), kbm.existsAsFile() ? kbm.getFullPathName().toStdString() : std::string(), tuning, error)) {
                if (ctx_.notify)
                    ctx_.notify(str(error));
                return;
            }
            ctx_.session.edit("Tuning " + file.getFileNameWithoutExtension(), [&](zed::Patch& p) {
                p.tuning.name = tuning.name.empty() ? file.getFileNameWithoutExtension().toStdString() : tuning.name;
                p.tuning.keys.assign(tuning.note.begin(), tuning.note.end());
                return true;
            });
            refresh();
        });
    }
    Context& ctx_;
    Parts parts_;
    Sections sections_;
    Caption* voices_ = nullptr;
    Caption* tuningName_ = nullptr;
    Button* lockTuning_ = nullptr;
    int voicesShown_ = -1;
};

// --- Stage -------------------------------------------------------------------------------------------

class Stage final : public juce::Component {
public:
    enum class View { Source, WarpCurve, Filter, Envelope, EnvMulti, Lfo, Scope, Map };

    Stage(Context& ctx) : ctx_(ctx), table_(ctx, 0), filter_(ctx, 0), env_(ctx, 0), map_(ctx), scope_(ctx), warp_(ctx, "Warp curve"),
                          lfo_(ctx, "LFO shape"), multi_(ctx, "Envelope shape") {
        setTitle("Stage");
        title_.upper = true;
        title_.bold = true;
        addAndMakeVisible(title_);
        for (juce::Component* c : std::initializer_list<juce::Component*>{&table_, &filter_, &env_, &map_, &scope_, &warp_, &lfo_, &multi_})
            addChildComponent(c);
        for (Button* b : {&viewToggle_, &copy_, &paste_, &shapes_, &steps_, &snap_, &mapButton_}) {
            b->textSize = kTextSmall;
            addAndMakeVisible(*b);
        }
        steps_.toggleRole = snap_.toggleRole = true;
        mapButton_.setHelp("Signal map", "The routing of the whole sound; click a part to go to it.");
        mapButton_.onClick = [this] { show(view_ == View::Map ? previous_ : View::Map); };
        viewToggle_.onClick = [this] {
            if (view_ == View::Source) { table_.show3d = !table_.show3d; table_.repaint(); }
            else if (view_ == View::Filter) { filter_.showPhase = !filter_.showPhase; filter_.repaint(); }
            updateButtons();
        };
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
        copy_.setHelp("Copy shape", "Copy this shape; paste it into any LFO, envelope, warp curve or drive curve.");
        paste_.setHelp("Paste shape", "Paste a copied shape here.");
        shapes_.setHelp("Shapes", "Start from a basic shape.");
        steps_.setHelp("Step drawing", "Drag across the grid to draw steps.");
        snap_.setHelp("Snap", "Points snap to the grid (hold Alt while dragging to move freely).");

        CurveEditor::Config warpConfig;
        warpConfig.bipolar = false;
        warpConfig.fixedEnds = true;
        warp_.setConfig(warpConfig);
        warp_.family = Family::Sources;
        warp_.onChange = [this](const zed::Shape& s) { ctx_.processor.setWarpCurveLive(osc_, warpSlot_, s); };
        warp_.onCommit = [this](const zed::Shape&, const juce::String& label) { ctx_.session.commit(label); };
        CurveEditor::Config lfoConfig;
        lfoConfig.loopPoint = true;
        lfo_.setConfig(lfoConfig);
        lfo_.onChange = [this](const zed::Shape& s) { ctx_.processor.setLfoShapeLive(lfoIndex_, s); };
        lfo_.onCommit = [this](const zed::Shape&, const juce::String& label) { ctx_.session.commit(label); };
        CurveEditor::Config envConfig;
        envConfig.bipolar = false;
        envConfig.anchoredStart = true;
        envConfig.envelopeMarkers = true;
        envConfig.maxPoints = zed::kMaxEnvPoints + 1;
        envConfig.xUnit = "ms";
        multi_.setConfig(envConfig);
        multi_.onChange = [this](const zed::Shape& s) { pushEnv(s); };
        multi_.onCommit = [this](const zed::Shape&, const juce::String& label) { ctx_.session.commit(label); };
        multi_.onMarkers = [this](int, int, int) {
            pushEnv(multi_.shape());
            ctx_.session.commit("Envelope " + juce::String(envIndex_ + 1) + " sustain and loop");
        };
        for (CurveEditor* e : {&warp_, &lfo_, &multi_}) {
            e->onCopy = [this] { copyShape(); };
            e->onPaste = [this] { pasteShape(); };
        }
        show(View::Source);
    }

    View view() const { return view_; }
    CurveEditor* editor() {
        return view_ == View::Lfo ? &lfo_ : view_ == View::EnvMulti ? &multi_ : view_ == View::WarpCurve ? &warp_ : nullptr;
    }

    void showSource(int source) {
        source_ = source;
        if (source < 3) {
            osc_ = source;
            table_.setOsc(source);
            show(View::Source);
        } else {
            scope_.source = source == 3 ? "sub_audio" : "texture_audio";
            show(View::Scope);
        }
    }
    void showWarp(int osc, int slot) {
        osc_ = osc;
        warpSlot_ = slot;
        reloadShapes();
        show(View::WarpCurve);
    }
    void showFilter(int slot) {
        filter_.setSlot(slot);
        show(View::Filter);
    }
    void showModulator(const juce::String& source) {
        if (source.startsWith("env")) {
            envIndex_ = source.getTrailingIntValue() - 1;
            env_.setEnv(envIndex_);
            const bool multi = envIndex_ > 0 && ctx_.processor.plainValue(idx("env" + juce::String(envIndex_ + 1) + ".mode")) >= 0.5;
            reloadShapes();
            show(multi ? View::EnvMulti : View::Envelope);
        } else if (source.startsWith("lfo")) {
            lfoIndex_ = source.getTrailingIntValue() - 1;
            reloadShapes();
            show(View::Lfo);
        } else {
            scope_.source = source;
            show(View::Scope);
        }
    }
    void show(View v) {
        if (view_ != View::Map)
            previous_ = view_;
        view_ = v;
        table_.setVisible(v == View::Source);
        warp_.setVisible(v == View::WarpCurve);
        filter_.setVisible(v == View::Filter);
        env_.setVisible(v == View::Envelope);
        multi_.setVisible(v == View::EnvMulti);
        lfo_.setVisible(v == View::Lfo);
        scope_.setVisible(v == View::Scope);
        map_.setVisible(v == View::Map);
        updateButtons();
        resized();
    }

    // The shapes on show follow the document (undo, presets, pasting).
    void reloadShapes() {
        ctx_.processor.readDocument([&](const zed::Patch& p) {
            lfo_.setShape(p.lfoShapes[static_cast<std::size_t>(lfoIndex_)]);
            warp_.setShape(p.warpCurves[static_cast<std::size_t>(osc_)][static_cast<std::size_t>(warpSlot_)]);
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
        updateTitle();
    }

    void resized() override {
        auto area = getLocalBounds();
        auto header = area.removeFromTop(26);
        mapButton_.setBounds(header.removeFromRight(86).reduced(0, 2));
        header.removeFromRight(4);
        for (Button* b : {&paste_, &copy_, &shapes_, &snap_, &steps_, &viewToggle_}) {
            if (!b->isVisible())
                continue;
            b->setBounds(header.removeFromRight(b == &viewToggle_ ? 90 : 62).reduced(0, 2));
            header.removeFromRight(4);
        }
        title_.setBounds(header.withTrimmedLeft(4));
        area.removeFromTop(4);
        for (juce::Component* c : std::initializer_list<juce::Component*>{&table_, &filter_, &env_, &map_, &scope_, &warp_, &lfo_, &multi_})
            c->setBounds(area);
    }

    bool tickStage() {
        // An envelope switched to multi-segment mode (or back) changes the view.
        if (view_ == View::Envelope || view_ == View::EnvMulti) {
            const bool multi = envIndex_ > 0 && ctx_.processor.plainValue(idx("env" + juce::String(envIndex_ + 1) + ".mode")) >= 0.5;
            if (multi != (view_ == View::EnvMulti)) {
                reloadShapes();
                show(multi ? View::EnvMulti : View::Envelope);
                return true;
            }
        }
        return false;
    }

    void updateTitle() {
        juce::String t;
        switch (view_) {
        case View::Source: t = "Osc " + juce::String(osc_ + 1) + (table_.show3d ? "  table" : "  cycle"); break;
        case View::WarpCurve: t = "Osc " + juce::String(osc_ + 1) + "  warp " + (warpSlot_ == 0 ? "A" : "B") + " curve"; break;
        case View::Filter: t = "Filter response"; break;
        case View::Envelope: t = "Env " + juce::String(envIndex_ + 1); break;
        case View::EnvMulti: t = "Env " + juce::String(envIndex_ + 1) + "  multi-segment"; break;
        case View::Lfo: t = "LFO " + juce::String(lfoIndex_ + 1) + "  shape"; break;
        case View::Scope: t = Context::sourceName(scope_.source); break;
        case View::Map: t = "Signal map"; break;
        }
        title_.setText(t);
    }

private:
    void updateButtons() {
        const bool curve = editor() != nullptr;
        viewToggle_.setVisible(view_ == View::Source || view_ == View::Filter);
        viewToggle_.setText(view_ == View::Source ? (table_.show3d ? "Show cycle" : "Show table") : (filter_.showPhase ? "Magnitude" : "Phase"));
        for (Button* b : {&copy_, &paste_, &shapes_, &steps_, &snap_})
            b->setVisible(curve);
        if (auto* e = editor()) {
            steps_.setToggled(e->stepMode);
            snap_.setToggled(e->snap);
        }
        mapButton_.setToggled(view_ == View::Map);
        updateTitle();
        resized();
    }
    void pushEnv(const zed::Shape& s) {
        ctx_.processor.setEnvShapeLive(envIndex_, shapeToEnv(s, envTotalMs_, multi_.sustainPoint, multi_.loopStart, multi_.loopEnd));
    }
    void copyShape() {
        if (view_ == View::EnvMulti) {
            zed::EnvShape env;
            ctx_.processor.readDocument([&](const zed::Patch& p) { env = p.envShapes[static_cast<std::size_t>(envIndex_)]; });
            ctx_.session.copyText(str(zed::copyEnvShape(env)));
        } else if (auto* e = editor()) {
            ctx_.session.copyText(str(zed::copyShape(e->shape(), view_ == View::Lfo ? zed::ShapeKind::Lfo : zed::ShapeKind::Curve)));
        }
        if (ctx_.notify)
            ctx_.notify("Shape copied: paste it into any LFO, envelope or curve.");
    }
    void pasteShape() {
        const std::string text = ctx_.session.clipboardText().toStdString();
        std::vector<zed::Issue> issues;
        bool ok = false;
        if (view_ == View::EnvMulti) {
            zed::EnvShape env;
            if (zed::pasteEnvShape(text, env, issues))
                ok = ctx_.session.edit("Paste shape into Env " + juce::String(envIndex_ + 1), [&](zed::Patch& p) {
                    p.envShapes[static_cast<std::size_t>(envIndex_)] = env;
                    return true;
                });
        } else if (view_ == View::Lfo || view_ == View::WarpCurve) {
            zed::Shape shape;
            if (zed::pasteShape(text, view_ == View::Lfo ? zed::ShapeKind::Lfo : zed::ShapeKind::Curve, shape, issues))
                ok = ctx_.session.edit(view_ == View::Lfo ? "Paste shape into LFO " + juce::String(lfoIndex_ + 1) : juce::String("Paste warp curve"), [&](zed::Patch& p) {
                    if (view_ == View::Lfo)
                        p.lfoShapes[static_cast<std::size_t>(lfoIndex_)] = shape;
                    else
                        p.warpCurves[static_cast<std::size_t>(osc_)][static_cast<std::size_t>(warpSlot_)] = shape;
                    return true;
                });
        }
        if (ok)
            reloadShapes();
        else if (ctx_.notify)
            ctx_.notify(issues.empty() ? juce::String("Nothing to paste.") : str(issues.front().message));
    }

    Context& ctx_;
    Caption title_{"", kTextSmall + 0.5f, false};
    WavetableView table_;
    FilterResponseView filter_;
    EnvelopeView env_;
    SignalMap map_;
    SourceScope scope_;
    CurveEditor warp_, lfo_, multi_;
    Button viewToggle_{"Phase"}, copy_{"Copy", Glyph::Copy}, paste_{"Paste", Glyph::Paste}, shapes_{"Shapes"}, steps_{"Steps"}, snap_{"Snap"},
        mapButton_{"Signal map", Glyph::Map};
    View view_ = View::Source, previous_ = View::Source;
    int source_ = 0, osc_ = 0, warpSlot_ = 0, envIndex_ = 0, lfoIndex_ = 0;
    double envTotalMs_ = 1000.0;
};

// --- Modulator dock ----------------------------------------------------------------------------------

class ModDock final : public juce::Component {
public:
    ModDock(Context& ctx, SynthPage& page) : ctx_(ctx), page_(page), keyboard_(ctx.processor.keyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard),
                                             pitch_(ctx, true), mod_(ctx, false) {
        setTitle("Modulators");
        for (Button* b : {&modsButton_, &keysButton_}) {
            b->toggleRole = true;
            b->textSize = kTextSmall;
            addAndMakeVisible(*b);
        }
        modsButton_.setHelp("Modulators", "Envelopes, LFOs and Flux: drag a chip onto any control.");
        keysButton_.setHelp("Keyboard", "Play notes with the mouse or the computer keyboard; wheels for pitch and modulation.");
        modsButton_.onClick = [this] { setKeys(false); };
        keysButton_.onClick = [this] { setKeys(true); };
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
        keyboard_.setTitle("Keyboard");
        keyboard_.setAvailableRange(21, 108);
        keyboard_.setOctaveForMiddleC(4);
        const Theme& t = theme();
        keyboard_.setColour(juce::MidiKeyboardComponent::whiteNoteColourId, t.dark ? juce::Colour(0xffd9dde2) : juce::Colours::white);
        keyboard_.setColour(juce::MidiKeyboardComponent::blackNoteColourId, t.dark ? t.raised : juce::Colour(0xff2a2d31));
        keyboard_.setColour(juce::MidiKeyboardComponent::keyDownOverlayColourId, t.modulators.withAlpha(0.7f));
        keyboard_.setColour(juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, t.modulators.withAlpha(0.25f));
        addChildComponent(keyboard_);
        addChildComponent(pitch_);
        addChildComponent(mod_);
        select("env1");
        setKeys(false);
    }

    void select(const juce::String& source) {
        selected_ = source;
        for (auto* c : chips_)
            c->repaint();
        page_.focusModulator(source);
        resized();
    }
    const juce::String& selected() const { return selected_; }
    bool keys() const { return keys_; }
    void setKeys(bool keys) {
        keys_ = keys;
        modsButton_.setToggled(!keys);
        keysButton_.setToggled(keys);
        resized();
    }

    void resized() override {
        auto area = getLocalBounds().reduced(8, 6);
        auto modes = area.removeFromLeft(60);
        modsButton_.setBounds(modes.removeFromTop(26));
        modes.removeFromTop(4);
        keysButton_.setBounds(modes.removeFromTop(26));
        area.removeFromLeft(10);
        for (auto* c : chips_)
            c->setVisible(!keys_);
        keyboard_.setVisible(keys_);
        pitch_.setVisible(keys_);
        mod_.setVisible(keys_);
        const bool simple = ctx_.simpleView();
        for (auto& [source, items] : controls_)
            for (auto& item : items)
                if (auto* w = dynamic_cast<Widget*>(item.c))
                    w->setVisible(!keys_ && source == selected_ && shownInView(*w, simple) && visibleNow(*w));
        if (keys_) {
            pitch_.setBounds(area.removeFromLeft(30));
            area.removeFromLeft(4);
            mod_.setBounds(area.removeFromLeft(30));
            area.removeFromLeft(8);
            keyboard_.setBounds(area);
            keyboard_.setKeyWidth(juce::jmax(10.0f, static_cast<float>(area.getWidth()) / 52.0f));
            return;
        }
        auto chipRow = area.removeFromTop(22);
        const int w = juce::jmin(88, (chipRow.getWidth() - 13 * 4) / 14);
        for (auto* c : chips_) {
            c->setBounds(chipRow.removeFromLeft(w));
            chipRow.removeFromLeft(4);
        }
        area.removeFromTop(4);
        if (const auto it = controls_.find(selected_); it != controls_.end())
            flow(area, it->second, kKnobH, 6);
    }

    void paint(juce::Graphics& g) override {
        const Theme& t = theme();
        drawPanel(g, getLocalBounds().toFloat(), t.panel);
        if (!keys_) { // the selected chip, underlined
            for (auto* c : chips_)
                if (c->source() == selected_) {
                    g.setColour(ctx_.sourceColour(selected_));
                    g.fillRect(c->getBounds().getX() + 8, c->getBottom() + 1, c->getWidth() - 16, 2);
                }
        }
    }

    bool tickDock() {
        bool changed = false;
        for (auto& [id, sync] : syncParams_) {
            const bool on = ctx_.processor.plainValue(sync) >= 0.5;
            if (on != syncShown_[id]) {
                syncShown_[id] = on;
                changed = true;
            }
        }
        if (changed)
            resized();
        return changed;
    }

private:
    bool visibleNow(const Widget& w) const {
        // Rate or division, whichever the sync switch selects.
        const auto it = rateOrDivision_.find(&w);
        if (it == rateOrDivision_.end())
            return true;
        const bool synced = ctx_.processor.plainValue(it->second.first) >= 0.5;
        return it->second.second ? synced : !synced;
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
                items.push_back({&mode, 150, kChoiceH});
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
            auto& scope = parts_.add<Segmented>(*this, ctx_, (s + ".scope").toRawUTF8(), "Scope");
            scope.names = {"Voice", "Global"};
            scope.advanced = true;
            items = {{&choice("mode", "Mode"), 104, kChoiceH}, {&toggle("sync", "Sync"), 64, 22}, {&rate, kKnobW}, {&division, 76, kChoiceH},
                     {&toggle("anchor", "Anchor", true), 74, 22}, {&knob("phase", "Phase"), kKnobW}, {&knob("delay", "Delay", true), kKnobW},
                     {&knob("rise", "Rise"), kKnobW}, {&knob("smooth", "Smooth", true), kKnobW}, {&polarity, 140, kChoiceH}, {&scope, 112, kChoiceH},
                     {&knob("keytrack", "Key track", true), kKnobW}, {&toggle("audio_rate", "Audio rate", true), 96, 22}};
        } else {
            auto& rate = knob("rate", "Rate");
            auto& division = choice("division", "Division");
            const int sync = idx(s + ".sync");
            rateOrDivision_[&rate] = {sync, false};
            rateOrDivision_[&division] = {sync, true};
            syncParams_[s] = sync;
            auto& scope = parts_.add<Segmented>(*this, ctx_, (s + ".scope").toRawUTF8(), "Scope");
            scope.names = {"Voice", "Global"};
            items = {{&choice("type", "Type"), 110, kChoiceH}, {&toggle("sync", "Sync"), 64, 22}, {&rate, kKnobW}, {&division, 76, kChoiceH}, {&scope, 112, kChoiceH}};
        }
        for (auto& item : items)
            item.c->setVisible(false);
    }

    Context& ctx_;
    SynthPage& page_;
    Parts parts_;
    Button modsButton_{"Mods"}, keysButton_{"Keys", Glyph::Keyboard};
    std::vector<DragChip*> chips_;
    std::map<juce::String, std::vector<FlowItem>> controls_;
    std::map<const Widget*, std::pair<int, bool>> rateOrDivision_;
    std::map<juce::String, int> syncParams_;
    std::map<juce::String, bool> syncShown_;
    juce::String selected_;
    bool keys_ = false;
    juce::MidiKeyboardComponent keyboard_;
    Wheel pitch_, mod_;
};

// --- The page ----------------------------------------------------------------------------------------

namespace {
// Ticks the page's own parts that are not plain widgets (cards, stage, dock).
class PageTicker final : public Widget {
public:
    explicit PageTicker(std::function<bool()> f) : f_(std::move(f)) {
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

SynthPage::SynthPage(Context& ctx) : Page(ctx) {
    setTitle("SYNTH");
    for (int i = 0; i < 5; ++i) {
        sources_.push_back(std::make_unique<SourceCard>(ctx, *this, i));
        addAndMakeVisible(*sources_.back());
    }
    for (int i = 0; i < 2; ++i) {
        filters_.push_back(std::make_unique<FilterCard>(ctx, *this, i));
        addAndMakeVisible(*filters_.back());
    }
    voice_ = std::make_unique<VoiceCard>(ctx);
    addAndMakeVisible(*voice_);
    stage_ = std::make_unique<Stage>(ctx);
    addAndMakeVisible(*stage_);
    dock_ = std::make_unique<ModDock>(ctx, *this);
    addAndMakeVisible(*dock_);
    auto ticker = std::make_unique<PageTicker>([this] {
        bool any = false;
        for (auto& s : sources_)
            any = s->tickCard() || any;
        for (auto& f : filters_)
            any = f->tickCard() || any;
        any = voice_->tickCard() || any;
        any = stage_->tickStage() || any;
        any = dock_->tickDock() || any;
        return any;
    });
    ticker->setBounds(0, 0, 1, 1);
    addAndMakeVisible(*ticker);
    ticker_ = std::move(ticker);
    expandSource(0);
    focusSource(0);
    structureChanged();
}

SynthPage::~SynthPage() = default;

void SynthPage::paint(juce::Graphics& g) { g.fillAll(theme().bg); }

void SynthPage::resized() {
    auto area = getLocalBounds().reduced(8);
    dock_->setBounds(area.removeFromBottom(108));
    area.removeFromBottom(8);
    auto left = area.removeFromLeft(348);
    area.removeFromLeft(8);
    auto right = area.removeFromRight(348);
    area.removeFromRight(8);
    // Collapsed cards take their header; the open one takes the rest of the column.
    int collapsedHeight = 0;
    for (auto& s : sources_)
        if (s->collapsed)
            collapsedHeight += s->preferredHeight() + 6;
    for (auto& s : sources_) {
        const int h = s->collapsed ? s->preferredHeight() : std::max(s->preferredHeight(), left.getHeight() - collapsedHeight);
        s->setBounds(left.removeFromTop(h));
        left.removeFromTop(6);
    }
    for (auto& f : filters_) {
        f->setBounds(right.removeFromTop(132));
        right.removeFromTop(6);
    }
    voice_->setBounds(right);
    stage_->setBounds(area);
}

void SynthPage::expandSource(int source) {
    expanded_ = source;
    for (int i = 0; i < static_cast<int>(sources_.size()); ++i)
        sources_[static_cast<std::size_t>(i)]->collapsed = i != source;
    resized();
    for (auto& s : sources_)
        s->resized();
    focusSource(source);
}

void SynthPage::focusSource(int source) {
    for (int i = 0; i < static_cast<int>(sources_.size()); ++i)
        sources_[static_cast<std::size_t>(i)]->selectedLook = i == source;
    for (auto& f : filters_)
        f->selectedLook = false;
    stage_->showSource(source);
    repaint();
}

void SynthPage::focusFilter(int slot) {
    for (int i = 0; i < 2; ++i)
        filters_[static_cast<std::size_t>(i)]->selectedLook = i == slot;
    for (auto& s : sources_)
        s->selectedLook = false;
    stage_->showFilter(slot);
    repaint();
}

void SynthPage::focusModulator(const juce::String& source) {
    if (stage_ != nullptr)
        stage_->showModulator(source);
}

void SynthPage::focusWarpCurve(int osc, int slot) { stage_->showWarp(osc, slot); }
void SynthPage::showSignalMap() { stage_->show(Stage::View::Map); }

bool SynthPage::keyboardShown() const { return dock_->keys(); }
void SynthPage::setKeyboardShown(bool shown) { dock_->setKeys(shown); }

bool SynthPage::reveal(const juce::String& module) {
    const juce::String m = module.upToFirstOccurrenceOf(".", false, false);
    if (m.startsWith("osc")) { expandSource(m.getTrailingIntValue() - 1); return true; }
    if (m == "sub") { expandSource(3); return true; }
    if (m == "texture") { expandSource(4); return true; }
    if (m.startsWith("filter")) { focusFilter(m.getTrailingIntValue() - 1); return true; }
    if (m.startsWith("env") || m.startsWith("lfo") || m.startsWith("flux")) {
        dock_->setKeys(false);
        dock_->select(m);
        return true;
    }
    if (m == "voice" || m == "global") {
        return true;
    }
    if (m == "env1" || m.startsWith("macro"))
        return true;
    return false;
}

void SynthPage::applySimpleView() {
    for (auto& s : sources_)
        s->resized();
    for (auto& f : filters_)
        f->resized();
    voice_->resized();
    dock_->resized();
}

void SynthPage::structureChanged() {
    for (auto& s : sources_)
        s->refresh();
    voice_->refresh();
    stage_->reloadShapes();
}

void SynthPage::saveState(nlohmann::json& state) const {
    state["expanded"] = expanded_;
    state["modulator"] = dock_->selected().toStdString();
    state["keys"] = dock_->keys();
}

void SynthPage::loadState(const nlohmann::json& state) {
    if (state.contains("expanded") && state["expanded"].is_number_integer())
        expandSource(juce::jlimit(0, 4, state["expanded"].get<int>()));
    if (state.contains("modulator") && state["modulator"].is_string())
        dock_->select(juce::String(state["modulator"].get<std::string>()));
    if (state.contains("keys") && state["keys"].is_boolean())
        dock_->setKeys(state["keys"].get<bool>());
}

} // namespace zui
