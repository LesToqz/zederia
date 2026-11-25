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

bool shownIn(const FlowItem& item, bool simple) {
    if (auto* w = dynamic_cast<const Widget*>(item.c))
        return shownInView(*w, simple);
    return true;
}

// Tabs over groups of controls: only the selected group is visible. A group can place its items
// itself (custom) instead of flowing them; it then says how tall it is.
struct Sections {
    struct Custom {
        std::function<void(juce::Rectangle<int>, bool simple)> place;
        int height = 0;
    };
    TabStrip* tabs = nullptr;
    std::vector<std::vector<FlowItem>> groups;
    std::vector<Custom> custom; // per group; empty place: flowed
    std::vector<bool> advancedTab;
    int rowHeight = kKnobH;
    int gap = 4, tabHeight = 24, tabGap = 6;
    bool tabShown(int i, bool simple) const { return !(simple && i < static_cast<int>(advancedTab.size()) && advancedTab[static_cast<std::size_t>(i)]); }
    int groupHeight(int g, int width, bool simple) const {
        if (g < static_cast<int>(custom.size()) && custom[static_cast<std::size_t>(g)].place)
            return custom[static_cast<std::size_t>(g)].height;
        return flowHeight(width, groups[static_cast<std::size_t>(g)], rowHeight, gap, [simple](const FlowItem& i) { return shownIn(i, simple); });
    }
    // The tallest group a tab can show, so the area above can be given everything else and stays
    // put when another tab is chosen.
    int tallest(int width, bool simple) const {
        int h = 0;
        for (int g = 0; g < static_cast<int>(groups.size()); ++g)
            if (tabShown(g, simple))
                h = std::max(h, groupHeight(g, width, simple));
        return h;
    }
    void layout(juce::Rectangle<int> area, bool simple) {
        if (tabs != nullptr) {
            for (int i = 0; i < tabs->count(); ++i)
                tabs->setTabVisible(i, tabShown(i, simple));
            if (!tabs->tab(tabs->selected()).isVisible())
                tabs->setSelected(0, false);
            tabs->setBounds(area.removeFromTop(tabHeight));
            area.removeFromTop(tabGap);
        }
        const int current = tabs != nullptr ? tabs->selected() : 0;
        for (int g = 0; g < static_cast<int>(groups.size()); ++g)
            for (auto& item : groups[static_cast<std::size_t>(g)])
                item.c->setVisible(g == current && shownIn(item, simple));
        if (current >= static_cast<int>(groups.size()))
            return;
        if (current < static_cast<int>(custom.size()) && custom[static_cast<std::size_t>(current)].place)
            custom[static_cast<std::size_t>(current)].place(area, simple);
        else
            flow(area, groups[static_cast<std::size_t>(current)], rowHeight, gap);
    }
    void hideAll() {
        if (tabs != nullptr)
            tabs->setVisible(false);
        for (auto& g : groups)
            for (auto& item : g)
                item.c->setVisible(false);
    }
};

} // namespace

// --- Source cards --------------------------------------------------------------------------------------

// An oscillator: its table (prev, name, next), a display of the table or its cycle, the controls
// used most on one row, and the rest in four tabs (unison, spread, warp, pitch). Sub and Texture:
// two rows each. Nothing is collapsed: the three oscillators sit side by side.
class SourceCard final : public Card {
public:
    SourceCard(Context& ctx, SynthPage& page, int source)
        : Card(source < 3 ? "Osc " + juce::String(source + 1) : source == 3 ? "Sub" : "Texture", Family::Sources), ctx_(ctx), page_(page), source_(source) {
        prefix_ = source < 3 ? "osc" + juce::String(source + 1) : source == 3 ? "sub" : "texture";
        moduleId = prefix_;
        auto& power = parts_.add<Toggle>(*this, ctx, (prefix_ + ".on").toRawUTF8(), getTitle() + " on");
        power.power = true;
        power.family = Family::Sources;
        powerSwitch = &power;
        onHeaderClick = [this] { page_.focusSource(source_); };
        if (source < 3)
            buildOsc();
        else if (source == 3)
            buildSub();
        else
            buildTexture();
        refresh();
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
        if (source_ == 4) {
            auto row = body.removeFromTop(22);
            prevTable_->setBounds(row.removeFromLeft(22));
            nextTable_->setBounds(row.removeFromRight(22));
            tableName_->setBounds(row.reduced(2, 0));
            body.removeFromTop(4);
        }
        if (source_ >= 3) { // Sub and Texture: their controls in rows
            for (auto& item : main_)
                if (auto* w = dynamic_cast<Widget*>(item.c))
                    w->setVisible(shownInView(*w, simple));
            flow(body, main_, kKnobH, 4);
            return;
        }
        // An oscillator. The same control sits in the same place in Table and Classic, so the three
        // oscillators line up whatever their engines: Position (Table) or Shape (Classic) first,
        // then Level, Pan and Fine, then Coarse (Table) or Width (Classic); in Classic, Coarse joins
        // the PITCH group. The display takes every pixel the tallest group of the tabs leaves.
        const bool classic = engine_ != nullptr && ctx_.processor.plainValue(engine_->param()) >= 0.5;
        sections_.groups[3] = classic ? pitchClassic_ : pitchTable_;
        for (auto* c : swappable_)
            c->setVisible(false);
        const auto& main = classic ? mainClassic_ : mainTable_;
        const int tallest = sections_.tallest(body.getWidth(), simple);
        constexpr int performanceRowHeight = 76;
        const int viewH = juce::jmax(60, body.getHeight() - 4 - (performanceRowHeight + 2) - (sections_.tabHeight + sections_.tabGap) - tallest);
        view_->setBounds(body.removeFromTop(viewH));
        // The asset selector is part of the display bezel. Classic generates its own cycle.
        auto picker = view_->getLocalBounds().withTrimmedLeft(45).withTrimmedRight(48).removeFromTop(23).withTrimmedTop(3);
        prevTable_->setBounds(picker.removeFromLeft(20));
        nextTable_->setBounds(picker.removeFromRight(20));
        tableName_->setBounds(picker);
        for (auto* b : {prevTable_, tableName_, nextTable_})
            b->setVisible(!classic);
        body.removeFromTop(4);
        auto mainRow = body.removeFromTop(performanceRowHeight);
        body.removeFromTop(2);
        sections_.layout(body, simple);
        for (auto& item : main)
            if (auto* w = dynamic_cast<Widget*>(item.c))
                w->setVisible(shownInView(*w, simple));
        // The row keeps its columns: a hidden control (Coarse in Simple view) leaves its place empty.
        for (std::size_t i = 0; i < main.size(); ++i) {
            const int w = i == 0 ? mainRow.getWidth() / 4 : mainRow.getWidth() / static_cast<int>(main.size() - i);
            auto cell = mainRow.removeFromLeft(w);
            if (i != 0)
                cell = cell.removeFromBottom(kKnobH);
            main[i].c->setBounds(cell);
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
        return k;
    }
    template <class T>
    T& part(const juce::String& suffix, const juce::String& label, bool adv = false) {
        auto& p = parts_.add<T>(*this, ctx_, (prefix_ + "." + suffix).toRawUTF8(), label);
        p.family = Family::Sources;
        p.advanced = adv;
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

    void addTabs(const juce::StringArray& names, std::vector<bool> advancedTabs) {
        auto& tabs = parts_.add<TabStrip>(*this, names);
        tabs.family = Family::Sources;
        tabs.setTitle(getTitle() + " sections");
        tabs.onChange = [this](int) { resized(); };
        sections_.tabs = &tabs;
        sections_.advancedTab = std::move(advancedTabs);
    }

    void buildOsc() {
        engine_ = &parts_.add<Segmented>(*this, ctx_, (prefix_ + ".engine").toRawUTF8(), "Engine");
        engine_->names = {"Table", "Classic"};
        engine_->showLabel = false;
        engine_->labelledBy = getTitle();
        engine_->family = Family::Sources;
        engine_->setSize(104, 20);
        headerRight.push_back(engine_);
        addTableRow(false);
        view_ = &parts_.add<WavetableView>(*this, ctx_, source_);
        for (auto* b : {prevTable_, tableName_, nextTable_})
            view_->addAndMakeVisible(*b);
        tableName_->ghost = true;
        tableName_->boldText = true;
        view_->setHelp("Table", "The table in depth with the playing position lit, or the cycle as it sounds now. The 3D / 2D button switches.");
        auto& position = knob("position", "Position");
        auto& coarse = knob("coarse", "Coarse", true);
        auto& shape = knob("classic.shape", "Shape");
        position.valueSize = shape.valueSize = kPrimaryValueText;
        auto& width = knob("classic.pulse_width", "Width");
        auto& level = knob("level", "Level");
        auto& pan = knob("pan", "Pan");
        auto& fine = knob("fine", "Fine");
        swappable_ = {&position, &coarse, &shape, &width};
        mainTable_ = {{&position, kKnobW}, {&level, kKnobW}, {&pan, kKnobW}, {&fine, kKnobW}, {&coarse, kKnobW}};
        mainClassic_ = {{&shape, kKnobW}, {&level, kKnobW}, {&pan, kKnobW}, {&fine, kKnobW}, {&width, kKnobW}};
        addTabs({"UNISON", "SPREAD", "WARP", "PITCH"}, {false, true, false, false});
        sections_.gap = 2;
        sections_.rowHeight = kSmallKnobH;
        sections_.tabHeight = 22;
        sections_.tabGap = 4;
        // Unison: how many voices and how they spread.
        auto& voices = part<NumberBox>("unison.voices", "Voices");
        voices.costWarning = true;
        sections_.groups.push_back({{&voices, 50, kChoiceH}, {&knob("unison.detune", "Detune"), kKnobW}, {&knob("unison.blend", "Blend"), kKnobW},
                                    {&knob("unison.width", "Width"), kKnobW}, {&knob("unison.range", "Range", true), kKnobW}});
        // Spread: the laws on one row, the two spreads below.
        sections_.groups.push_back({{&part<ChoiceBox>("unison.law", "Spread law"), 86, kChoiceH}, {&part<ChoiceBox>("unison.stack", "Stack"), 112, kChoiceH},
                                    {&part<ChoiceBox>("unison.phase", "Start"), 80, kChoiceH},
                                    {&knob("unison.position_spread", "Pos sprd"), kKnobW, 0, true}, {&knob("unison.warp_spread", "Warp sprd"), kKnobW}});
        // Warp: the two slots side by side, each a column: its mode over its amount, the source of
        // the cross-modulation modes and the button that draws its curve.
        std::vector<FlowItem> warpItems;
        struct Slot {
            ChoiceBox* mode;
            Knob* amount;
            ChoiceBox* source;
            Button* curve;
        };
        auto slots = std::make_shared<std::array<Slot, 2>>();
        for (int slot = 0; slot < 2; ++slot) {
            const juce::String id = slot == 0 ? "warp_a" : "warp_b";
            auto& mode = part<ChoiceBox>(id + ".mode", slot == 0 ? "Warp A" : "Warp B");
            mode.groupOf = warpGroup;
            auto& amount = knob(id + ".amount", "Amount");
            auto& source = part<ChoiceBox>(id + ".source", "Source", true);
            auto& curve = parts_.add<Button>(*this, "", Glyph::Pencil);
            curve.setTitle(juce::String("Edit warp ") + (slot == 0 ? "A" : "B") + " curve");
            curve.setTooltip(curve.getTitle());
            curve.setHelp("Edit curve", "Draw this warp's phase curve in the display below (the curve modes use it).");
            curve.onClick = [this, slot] { page_.focusWarpCurve(source_, slot); };
            (*slots)[static_cast<std::size_t>(slot)] = {&mode, &amount, &source, &curve};
            for (juce::Component* c : std::initializer_list<juce::Component*>{&mode, &amount, &source, &curve})
                warpItems.push_back({c, 0});
        }
        sections_.groups.push_back(warpItems);
        sections_.custom.resize(4);
        sections_.custom[0].height = kKnobH;
        sections_.custom[0].place = [this](juce::Rectangle<int> area, bool) {
            const auto& items = sections_.groups.front();
            int count = static_cast<int>(std::count_if(items.begin(), items.end(), [](const FlowItem& i) { return i.c->isVisible(); }));
            // Centre this single-row bank in the space shared with the two-row Warp and Pitch banks.
            auto row = area.withSizeKeepingCentre(area.getWidth(), juce::jmin(kKnobH, area.getHeight()));
            for (const auto& item : items) {
                if (!item.c->isVisible())
                    continue;
                auto cell = row.removeFromLeft(row.getWidth() / count--);
                if (item.height > 0)
                    cell = cell.withSizeKeepingCentre(std::min(cell.getWidth(), item.width), item.height);
                item.c->setBounds(cell);
            }
        };
        sections_.custom[2].height = kChoiceH + 2 + kSmallKnobH;
        sections_.custom[2].place = [slots](juce::Rectangle<int> area, bool) {
            constexpr int kColumnGap = 8;
            const int colW = (area.getWidth() - kColumnGap) / 2;
            for (int s = 0; s < 2; ++s) {
                const auto& slot = (*slots)[static_cast<std::size_t>(s)];
                auto col = juce::Rectangle<int>(area.getX() + s * (colW + kColumnGap), area.getY(), colW, kChoiceH + 2 + kSmallKnobH);
                slot.mode->setBounds(col.removeFromTop(kChoiceH));
                col.removeFromTop(2);
                slot.amount->setBounds(col.removeFromLeft(52));
                col.removeFromLeft(2);
                slot.curve->setBounds(col.removeFromRight(24).withSizeKeepingCentre(24, 24));
                col.removeFromRight(2);
                slot.source->setBounds(col.withSizeKeepingCentre(col.getWidth(), kChoiceH));
            }
        };
        // Pitch and phase, and where the oscillator goes: the switches on one row, the dials below.
        auto& phaseMode = part<Segmented>("phase_mode", "Phase");
        phaseMode.names = {"Reset", "Continue"};
        pitchTable_ = {{&part<NumberBox>("octave", "Octave"), 50, kChoiceH}, {&part<NumberBox>("semi", "Semi"), 50, kChoiceH},
                       {&part<Toggle>("keytrack", "Key", true), 58, 22}, {&phaseMode, 120, kChoiceH},
                       {&knob("phase", "Phase"), kKnobW, 0, true}, {&knob("phase_random", "Random"), kKnobW},
                       {&knob("sends.filter_balance", "F1 / F2", true), kKnobW}, {&knob("sends.direct", "Direct", true), kKnobW}};
        pitchClassic_ = pitchTable_;
        pitchClassic_.push_back({&coarse, kKnobW});
        sections_.groups.push_back(pitchTable_);
    }

    void buildSub() {
        auto& wave = parts_.add<WaveMini>(*this, ctx_, source_);
        wave.setSize(64, 20);
        auto& direct = part<Toggle>("direct", "Direct");
        direct.setSize(70, 20);
        headerRight = {&direct, &wave};
        main_ = {{&knob("level", "Level"), kKnobW}, {&knob("pan", "Pan"), kKnobW}, {&knob("fine", "Fine"), kKnobW}, {&knob("coarse", "Coarse", true), kKnobW},
                 {&part<NumberBox>("octave", "Octave"), 50, kChoiceH, true}, {&part<ChoiceBox>("shape", "Shape"), 110, kChoiceH},
                 {&knob("sends.filter_balance", "F1 / F2", true), kKnobW}};
    }

    void buildTexture() {
        auto& wave = parts_.add<WaveMini>(*this, ctx_, source_);
        wave.setSize(64, 20);
        auto& direct = part<Toggle>("direct", "Direct");
        direct.setSize(70, 20);
        headerRight = {&direct, &wave};
        addTableRow(true);
        auto& mode = part<Segmented>("mode", "Mode");
        mode.names = {"One shot", "Loop"};
        main_ = {{&knob("level", "Level"), kKnobW}, {&knob("pan", "Pan"), kKnobW}, {&knob("pitch", "Pitch"), kKnobW}, {&knob("fine", "Fine", true), kKnobW},
                 {&knob("start", "Start"), kKnobW},
                 {&knob("random_start", "Rnd start", true), kKnobW, 0, true}, {&knob("sends.filter_balance", "F1 / F2", true), kKnobW},
                 {&mode, 100, kChoiceH}, {&part<Toggle>("keytrack", "Key track"), 75, 22}};
    }

    Context& ctx_;
    SynthPage& page_;
    int source_;
    juce::String prefix_;
    Parts parts_;
    WavetableView* view_ = nullptr;
    Button *prevTable_ = nullptr, *nextTable_ = nullptr, *tableName_ = nullptr;
    Segmented* engine_ = nullptr;
    std::vector<FlowItem> main_;                    // Sub and Texture
    std::vector<FlowItem> mainTable_, mainClassic_; // an oscillator's first row, by engine
    std::vector<FlowItem> pitchTable_, pitchClassic_;
    std::vector<juce::Component*> swappable_;       // the parts that change places with the engine
    Sections sections_;
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
        // Cutoff is the dial that defines the sound: the largest, with its value and unit large on
        // it; the precision controls keep their row beside it.
        cutoff_ = &k("cutoff", "Cutoff");
        cutoff_->valueSize = kPrimaryValueText;
        cutoff_->trackInset = 9.0f;
        character_ = &k("character", "Body");
        auto& mix = parts_.add<Fader>(*this, ctx, (prefix_ + ".mix").toRawUTF8(), "Mix");
        mix.family = Family::Filters;
        auto& mode = parts_.add<Segmented>(*this, ctx, (prefix_ + ".mix_mode").toRawUTF8(), "Mix mode");
        mode.names = {"Mix", "Level"};
        mode.showLabel = false;
        mode.labelledBy = "Mix";
        mode.family = Family::Filters;
        mode.advanced = true;
        items_ = {{&k("resonance", "Res"), 50}, {&k("drive", "Drive"), 50}, {character_, 50}, {&k("stereo_offset", "Stereo", true), 50},
                  {&k("keytrack", "Key track"), 50}, {&mix, 158, 22, true}, {&mode, 100, 22}};
        for (auto* child : getChildren())
            child->addMouseListener(this, false);
    }
    void mouseDown(const juce::MouseEvent& event) override {
        if (event.eventComponent == this)
            Card::mouseDown(event);
        else if (!event.mods.isPopupMenu())
            page_.focusFilter(slot_);
    }
    void layoutBody(juce::Rectangle<int> body) override {
        for (auto& item : items_)
            if (auto* w = dynamic_cast<Widget*>(item.c))
                w->setVisible(shownInView(*w, ctx_.simpleView()));
        cutoff_->setBounds(body.removeFromLeft(76).withSizeKeepingCentre(76, juce::jmin(body.getHeight(), 100)));
        body.removeFromLeft(4);
        body = body.withSizeKeepingCentre(body.getWidth(), juce::jmin(body.getHeight(), kKnobH + 3 + 22));
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
    Knob *cutoff_ = nullptr, *character_ = nullptr;
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
        sections_.rowHeight = kSmallKnobH;
        sections_.gap = 3;
        sections_.advancedTab = {false, false, false, false, true, true, true};
        const auto knob = [&](const char* id, const char* label, Family f = Family::Neutral) -> Knob& {
            auto& k = parts_.add<Knob>(*this, ctx, id, label);
            k.family = f;
            return k;
        };
        const auto fader = [&](const char* id, const char* label) -> Fader& {
            auto& f = parts_.add<Fader>(*this, ctx, id, label);
            f.labelWidth = 105;
            return f;
        };
        auto& routing = fader("voice.filter_routing", "Series / parallel");
        routing.family = Family::Filters;
        sections_.groups.push_back({{&routing, 296, 28, true}, {&fader("fx.filter1_balance", "F1 to lane B"), 296, 28, true},
                                    {&fader("fx.filter2_balance", "F2 to lane B"), 296, 28, true}, {&fader("fx.direct_balance", "Direct to lane B"), 296, 28, true}});
        auto& mode = parts_.add<Segmented>(*this, ctx, "global.mode", "Mode");
        mode.names = {"Poly", "Mono", "Legato"};
        voiceMode_ = &mode;
        modeCaption_ = &parts_.add<Caption>(*this, mode.valueText(), kTextSmall, true);
        modeCaption_->justification = juce::Justification::centredRight;
        modeCaption_->setSize(90, 18);
        headerRight = {modeCaption_};
        sections_.groups.push_back({{&mode, 170, kChoiceH}, {&parts_.add<NumberBox>(*this, ctx, "global.polyphony", "Polyphony"), 70, kChoiceH},
                                    {&parts_.add<ChoiceBox>(*this, ctx, "voice.steal", "Stealing"), 110, kChoiceH, true},
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
        sections_.groups.push_back({{&parts_.add<Toggle>(*this, ctx, "global.mpe", "MPE"), 54, 22}, {&knob("global.mpe_bend_range", "MPE bend"), kKnobW},
                                    {&parts_.add<Toggle>(*this, ctx, "global.midi_out", "MIDI out"), 83, 22}, {&parts_.add<Toggle>(*this, ctx, "global.midi_thru", "MIDI thru"), 88, 22},
                                    {&follower, 170, kChoiceH, true}, {&knob("global.follower.attack", "Attack"), kKnobW}, {&knob("global.follower.release", "Release"), kKnobW}});
        sections_.groups.push_back({{&parts_.add<ChoiceBox>(*this, ctx, "global.limiter", "Protection"), 110, kChoiceH},
                                    {&parts_.add<Toggle>(*this, ctx, "global.dc_filter", "DC filter"), 90, 22},
                                    {&parts_.add<ChoiceBox>(*this, ctx, "global.quality", "Quality"), 100, kChoiceH, true},
                                    {&parts_.add<ChoiceBox>(*this, ctx, "global.quality_offline", "Offline"), 100, kChoiceH}});
    }
    void layoutBody(juce::Rectangle<int> body) override { sections_.layout(body, ctx_.simpleView()); }
    bool tickCard() {
        const int mode = juce::roundToInt(voiceMode_->value());
        if (mode != modeShown_) {
            modeShown_ = mode;
            modeCaption_->setText(voiceMode_->valueText());
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
    Segmented* voiceMode_ = nullptr;
    Caption* modeCaption_ = nullptr;
    Caption* tuningName_ = nullptr;
    Button* lockTuning_ = nullptr;
    int modeShown_ = -1;
};

// --- Stage -------------------------------------------------------------------------------------------

// The display beside Sub and Texture: the focused filter's response, a warp curve to draw, Sub's or
// Texture's output, or the signal map. The oscillators have displays of their own, and the
// modulators theirs in the dock.
class Stage final : public Card {
public:
    enum class View { Filter, WarpCurve, Scope };
    std::function<void(bool)> onMap; // the signal map, shown large over the oscillators

    explicit Stage(Context& ctx) : Card("Filter 1 response", Family::Filters), ctx_(ctx), filter_(ctx, 0), scope_(ctx), warp_(ctx, "Warp curve") {
        setTitle("Stage");
        screws = false;
        for (juce::Component* c : std::initializer_list<juce::Component*>{&filter_, &scope_, &warp_})
            addChildComponent(c);
        for (Button* b : {&filter1_, &filter2_, &viewToggle_, &shapes_, &steps_, &snap_, &copy_, &paste_, &mapButton_}) {
            b->textSize = kTextSmall;
            addAndMakeVisible(*b);
        }
        viewToggle_.setSize(74, 18);
        shapes_.setSize(56, 18);
        steps_.setSize(48, 18);
        snap_.setSize(44, 18);
        copy_.setSize(22, 18);
        paste_.setSize(22, 18);
        mapButton_.setSize(22, 18);
        filter1_.setSize(30, 18);
        filter2_.setSize(30, 18);
        filter1_.setTitle("Show Filter 1 response");
        filter2_.setTitle("Show Filter 2 response");
        filter1_.setTooltip(filter1_.getTitle());
        filter2_.setTooltip(filter2_.getTitle());
        filter1_.toggleRole = filter2_.toggleRole = true;
        filter1_.onClick = [this] { showFilter(0); };
        filter2_.onClick = [this] { showFilter(1); };
        headerRight = {&mapButton_, &paste_, &copy_, &snap_, &steps_, &shapes_, &viewToggle_, &filter2_, &filter1_};
        steps_.toggleRole = snap_.toggleRole = mapButton_.toggleRole = true;
        mapButton_.setTitle("Signal map");
        mapButton_.setTooltip("Signal map");
        mapButton_.setHelp("Signal map", "The routing of the whole sound; click a part to go to it.");
        mapButton_.onClick = [this] { setMapShown(!mapShown_); };
        viewToggle_.onClick = [this] { filter_.togglePhase(); };
        filter_.onPhaseChanged = [this] { updateButtons(); };
        steps_.onClick = [this] { warp_.stepMode = !warp_.stepMode; warp_.repaint(); updateButtons(); };
        snap_.onClick = [this] { warp_.snap = !warp_.snap; updateButtons(); };
        copy_.setTitle("Copy curve");
        paste_.setTitle("Paste curve");
        copy_.setTooltip("Copy curve");
        paste_.setTooltip("Paste curve");
        copy_.onClick = [this] { copyShape(); };
        paste_.onClick = [this] { pasteShape(); };
        shapes_.onClick = [this] { warp_.showShapesMenu(&shapes_); };
        copy_.setHelp("Copy curve", "Copy this curve; paste it into any warp, LFO, envelope or drive curve.");
        paste_.setHelp("Paste curve", "Paste a copied curve here.");
        shapes_.setHelp("Shapes", "Start from a basic shape, or a shape from the factory or your Shapes folder.");
        steps_.setHelp("Step drawing", "Drag across the grid to draw steps.");
        snap_.setHelp("Snap", "Points snap to the grid (hold Alt while dragging to move freely).");
        CurveEditor::Config warpConfig;
        warpConfig.bipolar = false;
        warpConfig.fixedEnds = true;
        warp_.setConfig(warpConfig);
        warp_.family = Family::Sources;
        warp_.onChange = [this](const zed::Shape& s) { ctx_.processor.setWarpCurveLive(osc_, warpSlot_, s); };
        warp_.onCommit = [this](const zed::Shape&, const juce::String& label) { ctx_.session.commit(label); };
        warp_.onCopy = [this] { copyShape(); };
        warp_.onPaste = [this] { pasteShape(); };
        show(View::Filter);
    }

    View view() const { return view_; }
    void setMapShown(bool shown) {
        mapShown_ = shown;
        if (onMap)
            onMap(shown);
        updateButtons();
    }
    void showSource(int source) {
        scope_.source = source == 3 ? "sub_audio" : "texture_audio";
        show(View::Scope);
    }
    void showWarp(int osc, int slot) {
        osc_ = osc;
        warpSlot_ = slot;
        reloadShapes();
        show(View::WarpCurve);
    }
    void showFilter(int slot) {
        if (view_ == View::Filter && slot_ == slot)
            return;
        slot_ = slot;
        filter_.setSlot(slot);
        show(View::Filter);
    }
    void show(View v) {
        view_ = v;
        filter_.setVisible(v == View::Filter);
        warp_.setVisible(v == View::WarpCurve);
        scope_.setVisible(v == View::Scope);
        updateButtons();
    }
    void reloadShapes() {
        ctx_.processor.readDocument([&](const zed::Patch& p) { warp_.setShape(p.warpCurves[static_cast<std::size_t>(osc_)][static_cast<std::size_t>(warpSlot_)]); });
    }
    void layoutBody(juce::Rectangle<int> body) override {
        for (juce::Component* c : std::initializer_list<juce::Component*>{&filter_, &scope_, &warp_})
            c->setBounds(body);
    }

private:
    void updateButtons() {
        const bool curve = view_ == View::WarpCurve;
        filter1_.setVisible(view_ == View::Filter);
        filter2_.setVisible(view_ == View::Filter);
        filter1_.setToggled(slot_ == 0);
        filter2_.setToggled(slot_ == 1);
        viewToggle_.setVisible(view_ == View::Filter);
        viewToggle_.setText(filter_.isShowingPhase() ? "Magnitude" : "Phase");
        for (Button* b : {&shapes_, &steps_, &snap_, &copy_, &paste_})
            b->setVisible(curve);
        steps_.setToggled(warp_.stepMode);
        snap_.setToggled(warp_.snap);
        mapButton_.setToggled(mapShown_);
        juce::String t;
        switch (view_) {
        case View::Filter: t = "Filter response"; break;
        case View::WarpCurve: t = "Warp " + juce::String(warpSlot_ == 0 ? "A" : "B"); break;
        case View::Scope: t = Context::sourceName(scope_.source); break;
        }
        setTitleText(t);
        resized();
        repaint();
    }
    void copyShape() {
        ctx_.session.copyText(str(zed::copyShape(warp_.shape(), zed::ShapeKind::Curve)));
        if (ctx_.notify)
            ctx_.notify("Curve copied: paste it into any warp, LFO, envelope or curve.");
    }
    void pasteShape() {
        zed::Shape shape;
        std::vector<zed::Issue> issues;
        if (zed::pasteShape(ctx_.session.clipboardText().toStdString(), zed::ShapeKind::Curve, shape, issues)
            && ctx_.session.edit("Paste warp curve", [&](zed::Patch& p) {
                   p.warpCurves[static_cast<std::size_t>(osc_)][static_cast<std::size_t>(warpSlot_)] = shape;
                   return true;
               }))
            reloadShapes();
        else if (ctx_.notify)
            ctx_.notify(issues.empty() ? juce::String("Nothing to paste.") : str(issues.front().message));
    }

    Context& ctx_;
    FilterResponseView filter_;
    SourceScope scope_;
    CurveEditor warp_;
    Button filter1_{"F1"}, filter2_{"F2"}, viewToggle_{"Phase"}, shapes_{"Shapes"}, steps_{"Steps"}, snap_{"Snap"}, copy_{"", Glyph::Copy}, paste_{"", Glyph::Paste}, mapButton_{"", Glyph::Map};
    View view_ = View::Filter;
    bool mapShown_ = false;
    int slot_ = 0, osc_ = 0, warpSlot_ = 0;
};

// --- The signal map, large -----------------------------------------------------------------------------

// The routing of the whole sound over the oscillator row, where its parts have room to be read.
class MapCard final : public Card {
public:
    MapCard(Context& ctx, std::function<void()> close) : Card("Signal map", Family::Neutral), map_(ctx) {
        screws = false;
        addAndMakeVisible(map_);
        close_.setTitle("Close the signal map");
        close_.setTooltip("Close");
        close_.ghost = true;
        close_.setSize(22, 20);
        close_.onClick = std::move(close);
        addAndMakeVisible(close_);
        headerRight = {&close_};
    }
    void layoutBody(juce::Rectangle<int> body) override { map_.setBounds(body); }

private:
    SignalMap map_;
    Button close_{"", Glyph::Close};
};

// --- The page ----------------------------------------------------------------------------------------

namespace {
// Ticks the page's own parts that are not plain widgets (the cards).
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
    map_ = std::make_unique<MapCard>(ctx, [this] { stage_->setMapShown(false); });
    addChildComponent(*map_);
    stage_->onMap = [this](bool shown) {
        map_->setVisible(shown);
        if (shown)
            map_->toFront(false);
    };
    auto ticker = std::make_unique<PageTicker>([this] {
        bool any = false;
        for (auto& s : sources_)
            any = s->tickCard() || any;
        for (auto& f : filters_)
            any = f->tickCard() || any;
        any = voice_->tickCard() || any;
        return any;
    });
    ticker->setBounds(0, 0, 1, 1);
    addAndMakeVisible(*ticker);
    ticker_ = std::move(ticker);
    focusFilter(0);
    structureChanged();
}

SynthPage::~SynthPage() = default;

void SynthPage::paint(juce::Graphics&) {} // the chassis shows through

void SynthPage::resized() {
    auto area = getLocalBounds();
    auto right = area.removeFromRight(360);
    // Reserve the two lower control rows even with the help strip open.
    auto top = area.removeFromTop(juce::jmin(390, juce::jmax(0, area.getHeight() - 208)));
    map_->setBounds(top);
    // Lower bays share the oscillator row's vertical seams.
    const int oscW = top.getWidth() / 3;
    for (int i = 0; i < 3; ++i) {
        sources_[static_cast<std::size_t>(i)]->setBounds(i < 2 ? top.removeFromLeft(oscW) : top);
    }
    sources_[3]->setBounds(area.removeFromLeft(oscW));
    sources_[4]->setBounds(area.removeFromLeft(oscW));
    voice_->setBounds(area);
    // Both cutoff controls and their shared transfer display occupy one instrument strip.
    for (auto& f : filters_)
        f->setBounds(right.removeFromTop(146));
    stage_->setBounds(right);
}

void SynthPage::expandSource(int source) { focusSource(source); }

void SynthPage::focusSource(int source) {
    expanded_ = juce::jlimit(0, 4, source);
    if (source >= 3)
        stage_->showSource(source);
}

void SynthPage::focusFilter(int slot) {
    stage_->showFilter(slot);
}

void SynthPage::focusWarpCurve(int osc, int slot) {
    focusSource(osc);
    stage_->showWarp(osc, slot);
}

void SynthPage::showSignalMap() { stage_->setMapShown(true); }

bool SynthPage::reveal(const juce::String& module) {
    const juce::String m = module.upToFirstOccurrenceOf(".", false, false);
    if (m.startsWith("osc")) { focusSource(m.getTrailingIntValue() - 1); return true; }
    if (m == "sub") { focusSource(3); return true; }
    if (m == "texture") { focusSource(4); return true; }
    if (m.startsWith("filter")) { focusFilter(m.getTrailingIntValue() - 1); return true; }
    if (m.startsWith("env") || m.startsWith("lfo") || m.startsWith("flux") || m.startsWith("macro")) {
        if (ctx_.showModulator)
            ctx_.showModulator(m); // the dock, under every page
        return true;
    }
    return m == "voice" || m == "global";
}

void SynthPage::applySimpleView() {
    for (auto& s : sources_)
        s->resized();
    for (auto& f : filters_)
        f->resized();
    voice_->resized();
    stage_->resized();
}

void SynthPage::structureChanged() {
    for (auto& s : sources_)
        s->refresh();
    voice_->refresh();
    stage_->reloadShapes();
}

void SynthPage::saveState(nlohmann::json& state) const { state["focus"] = expanded_; }

void SynthPage::loadState(const nlohmann::json& state) {
    if (state.contains("focus") && state["focus"].is_number_integer())
        focusSource(juce::jlimit(0, 4, state["focus"].get<int>()));
}

} // namespace zui
