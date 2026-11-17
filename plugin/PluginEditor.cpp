#include "PluginEditor.h"

#include "ui/AgentPage.h"
#include "ui/Audit.h"
#include "ui/FxPage.h"
#include "ui/KitPage.h"
#include "ui/LibraryPage.h"
#include "ui/ModPage.h"
#include "ui/PlayPage.h"
#include "ui/SynthPage.h"
#include "ui/TablePage.h"

#include "zederia/content/PresetIndex.h"
#include "zederia/patch/Modules.h"

#include <nlohmann/json.hpp>

using namespace zui;

namespace {

int idx(const char* id) { return zed::Registry::get().indexOf(id); }

} // namespace

// The scaled content: everything is laid out at 1320 x 820 and drawn through one transform.
class ZederiaEditor::Root final : public juce::Component {
public:
    explicit Root(ZederiaEditor& owner) : owner_(owner) {
        setName("root");
        setSize(kBaseWidth, kBaseHeight);
    }
    void paint(juce::Graphics& g) override { g.fillAll(theme().bg); }
    void resized() override { owner_.applyLayout(); }

private:
    ZederiaEditor& owner_;
};

juce::StringArray ZederiaEditor::pageNames() { return {"SYNTH", "FX", "MOD", "TABLE", "PLAY", "LIBRARY", "AGENT"}; }

ZederiaEditor::ZederiaEditor(ZederiaProcessor& processor)
    : juce::AudioProcessorEditor(processor), processor_(processor), ctx_(processor), modMap_(ctx_) {
    Theme::setCurrent(ctx_.prefs.theme);
    Theme::addUserThemes(ctx_.prefs.userContentFolder().getChildFile("Themes"));
    lnf_.applyTheme();
    setLookAndFeel(&lnf_);
    setTitle("Zederia");
    root_ = std::make_unique<Root>(*this);
    addAndMakeVisible(*root_);

    topBar_ = std::make_unique<TopBar>(ctx_);
    tabs_ = std::make_unique<TabStrip>(pageNames());
    tabs_->large = true;
    tabs_->setTitle("Pages");
    tabs_->onChange = [this](int page) { showPage(page); };
    macros_ = std::make_unique<MacroStrip>(ctx_);
    help_ = std::make_unique<HelpStrip>();
    hoverList_ = std::make_unique<ModHoverList>(ctx_);
    palette_ = std::make_unique<CommandPalette>(ctx_);
    prefs_ = std::make_unique<PrefsPanel>(ctx_);
    history_ = std::make_unique<HistoryPanel>(ctx_);
    for (juce::Component* c : std::initializer_list<juce::Component*>{topBar_.get(), tabs_.get(), &simpleButton_, &mapButton_, &findButton_,
                                                                       &prefsButton_, macros_.get(), help_.get()})
        root_->addAndMakeVisible(c);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&modMap_, hoverList_.get(), history_.get(), palette_.get(), prefs_.get()})
        root_->addChildComponent(c);

    simpleButton_.toggleRole = true;
    simpleButton_.setHelp("Simple view", "Hides the advanced controls; everything stays reachable from Full view and the command palette.");
    simpleButton_.onClick = [this] {
        ctx_.prefs.simpleView = !ctx_.prefs.simpleView;
        Prefs::get().save();
        applyLayout();
    };
    mapButton_.toggleRole = true;
    mapButton_.setHelp("Mod map", "Draws every modulation connection on this page at once (Ctrl+M).");
    mapButton_.onClick = [this] { setModMap(!modMapShown()); };
    findButton_.setHelp("Find", "The command palette: any control, action or preset by name (Ctrl+K).");
    findButton_.onClick = [this] { showPalette(); };
    prefsButton_.setTitle("Preferences");
    prefsButton_.setTooltip("Preferences");
    prefsButton_.setHelp("Preferences", "Theme, size, knob and wheel behaviour, frame rate, rendering, default author and content folder.");
    prefsButton_.ghost = true;
    prefsButton_.onClick = [this] { showPrefs(); };

    topBar_->onMenu = [this] { showMainMenu(); };
    topBar_->onPalette = [this] { showPalette(); };
    topBar_->onHistory = [this] { toggleHistory(); };
    topBar_->onSavePreset = [this] { savePresetAs(); };
    topBar_->onPresetBrowse = [this] { showPage(Library); };
    topBar_->onStepPreset = [this](int d) { stepPreset(d); };
    palette_->onClose = [this] { palette_->setVisible(false); };
    prefs_->onClose = [this] { prefs_->setVisible(false); };
    prefs_->onThemeChanged = [this] { applyTheme(); };
    prefs_->onScale = [this](int percent) { setScalePercent(percent); };
    prefs_->onLayout = [this] { applyLayout(); };
    history_->onClose = [this] { history_->setVisible(false); };

    ctx_.openParamMenu = [this](ParamWidget& w) { openParamMenu(w); };
    ctx_.hoverParam = [this](ParamWidget* w) {
        if (w != nullptr) {
            if (hoverCandidate_.getComponent() != w) {
                hoverCandidate_ = w;
                hoverSince_ = juce::Time::getMillisecondCounterHiRes();
            }
        } else {
            hoverCandidate_ = nullptr;
            hoverLeft_ = juce::Time::getMillisecondCounterHiRes();
        }
    };
    ctx_.showHelp = [this](const juce::String& title, const juce::String& text) { help_->show(title, text); };
    ctx_.notify = [this](const juce::String& message) { help_->message(message); };
    ctx_.reveal = [this](const juce::String& module) { revealModule(module); };
    ctx_.layoutChanged = [this] { applyLayout(); };

    processor_.session().addChangeListener(this);
    root_->addMouseListener(this, true);

    setResizable(true, true);
    setResizeLimits(kBaseWidth * 70 / 100, kBaseHeight * 70 / 100, kBaseWidth * 3, kBaseHeight * 3);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio(static_cast<double>(kBaseWidth) / kBaseHeight);
    applyLayout();
    loadUiState(); // sets the page and the size
    if (getWidth() == 0)
        setScalePercent(ctx_.prefs.defaultScale);
    ctx_.refresh();
    startTimerHz(ctx_.prefs.frameCap);
}

ZederiaEditor::~ZederiaEditor() {
    stopTimer();
    saveUiState();
    processor_.session().removeChangeListener(this);
    root_->removeMouseListener(this);
    processor_.keyboardState().allNotesOff(0); // keys held with the mouse must not hang
    processor_.stopAudition();
    ctx_.hoverParam = nullptr;
    for (auto& p : pages_)
        p.reset();
    setLookAndFeel(nullptr);
}

void ZederiaEditor::paint(juce::Graphics& g) { g.fillAll(theme().bg); }

void ZederiaEditor::resized() {
    const float s = scale();
    root_->setTransform(juce::AffineTransform::scale(s));
    root_->setBounds(0, 0, kBaseWidth, kBaseHeight);
}

void ZederiaEditor::setScalePercent(int percent) {
    percent = juce::jlimit(70, 300, percent);
    setSize(kBaseWidth * percent / 100, kBaseHeight * percent / 100);
}

void ZederiaEditor::applyLayout() {
    if (layingOut_ || topBar_ == nullptr || help_ == nullptr || prefs_ == nullptr || history_ == nullptr)
        return;
    const juce::ScopedValueSetter<bool> guard(layingOut_, true);
    auto area = root_->getLocalBounds();
    topBar_->setBounds(area.removeFromTop(kTopBarHeight));
    auto tabRow = area.removeFromTop(kTabsHeight);
    help_->setVisible(ctx_.prefs.helpStrip);
    if (ctx_.prefs.helpStrip)
        help_->setBounds(area.removeFromBottom(kHelpStripHeight));
    macros_->setVisible(ctx_.prefs.macroStrip);
    if (ctx_.prefs.macroStrip)
        macros_->setBounds(area.removeFromBottom(kMacroStripHeight));
    tabRow.reduce(8, 0);
    prefsButton_.setBounds(tabRow.removeFromRight(28).reduced(0, 3));
    tabRow.removeFromRight(6);
    findButton_.setBounds(tabRow.removeFromRight(70).reduced(0, 3));
    tabRow.removeFromRight(6);
    mapButton_.setBounds(tabRow.removeFromRight(92).reduced(0, 3));
    tabRow.removeFromRight(6);
    simpleButton_.setBounds(tabRow.removeFromRight(70).reduced(0, 3));
    simpleButton_.setToggled(ctx_.prefs.simpleView);
    tabRow.removeFromRight(12);
    tabs_->setBounds(tabRow);
    for (auto& p : pages_) {
        if (p == nullptr)
            continue;
        p->setBounds(area);
        p->applySimpleView();
    }
    modMap_.setBounds(area);
    modMap_.scope = root_.get();
    palette_->setBounds(juce::Rectangle<int>(560, 420).withCentre(root_->getLocalBounds().getCentre()).withY(80));
    prefs_->setBounds(juce::Rectangle<int>(520, 560).withCentre(root_->getLocalBounds().getCentre()));
    history_->setBounds(juce::Rectangle<int>(360, 460).withPosition(330, kTopBarHeight + 2));
    if (isTimerRunning())
        startTimerHz(ctx_.prefs.frameCap);
    applyRenderer();
}

void ZederiaEditor::applyTheme() {
    Theme::setCurrent(ctx_.prefs.theme);
    lnf_.applyTheme();
    sendLookAndFeelChange();
    repaint();
}

void ZederiaEditor::applyRenderer() {
    auto* peer = getPeer();
    if (peer == nullptr)
        return;
    const auto engines = peer->getAvailableRenderingEngines();
    int wanted = -1;
    for (int i = 0; i < engines.size(); ++i) {
        const bool software = engines[i].containsIgnoreCase("software");
        if (software != ctx_.prefs.gpuRendering)
            wanted = i;
    }
    if (wanted >= 0 && peer->getCurrentRenderingEngine() != wanted)
        peer->setCurrentRenderingEngine(wanted);
}

void ZederiaEditor::parentHierarchyChanged() { applyRenderer(); }

void ZederiaEditor::visibilityChanged() {
    applyRenderer();
    if (isVisible() && !isTimerRunning())
        startTimerHz(ctx_.prefs.frameCap);
}

// --- Pages -------------------------------------------------------------------------------------------

Page& ZederiaEditor::page(int id) {
    id = juce::jlimit(0, NumPages - 1, id);
    auto& slot = pages_[static_cast<std::size_t>(id)];
    if (slot == nullptr) {
        switch (id) {
        case Synth: slot = std::make_unique<SynthPage>(ctx_); break;
        case Fx: slot = std::make_unique<FxPage>(ctx_); break;
        case Mod: {
            auto mod = std::make_unique<ModPage>(ctx_);
            mod->onShowMap = [this] {
                showPage(Synth);
                setModMap(true);
            };
            slot = std::move(mod);
            break;
        }
        case Table: slot = std::make_unique<TablePage>(ctx_); break;
        case Play: slot = std::make_unique<PlayPage>(ctx_); break;
        case Library: slot = std::make_unique<LibraryPage>(ctx_); break;
        case Agent: slot = std::make_unique<AgentPage>(ctx_); break;
        default: slot = std::make_unique<KitPage>(ctx_); break;
        }
        root_->addChildComponent(*slot);
        slot->toBehind(&modMap_);
        auto area = root_->getLocalBounds().withTrimmedTop(kTopBarHeight + kTabsHeight);
        if (ctx_.prefs.helpStrip)
            area.removeFromBottom(kHelpStripHeight);
        if (ctx_.prefs.macroStrip)
            area.removeFromBottom(kMacroStripHeight);
        slot->setBounds(area);
        slot->applySimpleView();
    }
    return *slot;
}

void ZederiaEditor::showPage(int id) {
    id = juce::jlimit(0, NumPages - 1, id);
    Page& shown = page(id);
    for (auto& p : pages_)
        if (p != nullptr)
            p->setVisible(p.get() == &shown);
    current_ = id;
    if (id < tabs_->count())
        tabs_->setSelected(id, false);
    hoverList_->setVisible(false);
    modMap_.repaint();
    saveUiState();
}

bool ZederiaEditor::revealModule(const juce::String& module) {
    static const std::vector<std::pair<juce::String, int>> where = {
        {"osc", Synth}, {"sub", Synth}, {"texture", Synth}, {"filter", Synth}, {"voice", Synth}, {"env", Synth}, {"lfo", Synth}, {"flux", Synth},
        {"macro", Synth}, {"global", Synth}, {"fx", Fx}, {"mod.", Mod}, {"arp", Play}, {"chord", Play}, {"clip", Play}, {"table", Table}};
    for (const auto& [prefix, id] : where)
        if (module.startsWith(prefix)) {
            showPage(id);
            return page(id).reveal(module);
        }
    return false;
}

bool ZederiaEditor::revealParam(int index) {
    if (index < 0 || index >= zed::Registry::get().size())
        return false;
    const auto& def = zed::Registry::get().at(index);
    revealModule(str(def.id));
    // Focus the control once the page is laid out.
    std::function<ParamWidget*(juce::Component&)> find = [&](juce::Component& c) -> ParamWidget* {
        for (auto* child : c.getChildren()) {
            if (!child->isShowing())
                continue;
            if (auto* w = dynamic_cast<ParamWidget*>(child); w != nullptr && w->param() == index)
                return w;
            if (auto* deeper = find(*child))
                return deeper;
        }
        return nullptr;
    };
    if (auto* w = find(*root_)) {
        w->grabKeyboardFocus();
        help_->show(w->getTitle(), w->helpText, w->valueText());
        return true;
    }
    return false;
}

// --- Overlays ----------------------------------------------------------------------------------------

void ZederiaEditor::closeOverlays() {
    for (juce::Component* c : std::initializer_list<juce::Component*>{palette_.get(), prefs_.get(), history_.get(), hoverList_.get()})
        c->setVisible(false);
}

void ZederiaEditor::showPalette() {
    closeOverlays();
    palette_->setEntries(paletteEntries());
    palette_->setVisible(true);
    palette_->toFront(true);
    palette_->open();
}

void ZederiaEditor::showPrefs() {
    closeOverlays();
    prefs_->refresh();
    prefs_->setVisible(true);
    prefs_->toFront(true);
}

void ZederiaEditor::toggleHistory() {
    const bool show = !history_->isVisible();
    closeOverlays();
    history_->setVisible(show);
    if (show) {
        history_->refresh();
        history_->toFront(true);
    }
}

void ZederiaEditor::setModMap(bool shown) {
    modMap_.setVisible(shown);
    mapButton_.setToggled(shown);
    if (shown)
        modMap_.toFront(false);
    modMap_.repaint();
}

std::vector<CommandPalette::Entry> ZederiaEditor::paletteEntries() {
    std::vector<CommandPalette::Entry> out;
    const auto& reg = zed::Registry::get();
    const auto names = pageNames();
    for (int p = 0; p < names.size(); ++p)
        out.push_back({"Page", names[p], "Ctrl+" + juce::String(p + 1), [this, p] { showPage(p); }});
    const auto action = [&](const juce::String& name, const juce::String& detail, std::function<void()> run) {
        out.push_back({"Action", name, detail, std::move(run)});
    };
    action("Undo", "Ctrl+Z", [this] { ctx_.session.undo(); });
    action("Redo", "Ctrl+Shift+Z", [this] { ctx_.session.redo(); });
    action("Show history", "every change by name", [this] { toggleHistory(); });
    action("Init", "start from the init sound", [this] { ctx_.session.initPatch(); });
    action("Save preset", "Ctrl+S", [this] { savePresetAs(); });
    action("Compare: switch A and B", "Ctrl+B", [this] { ctx_.session.switchAB(); });
    action("Compare: copy to the other side", "A/B", [this] { ctx_.session.copyToOtherSide(); });
    action("Randomize", "half way, sources, filters, modulators, effects", [this] { ctx_.session.randomize({}, 0.5); });
    action("Mutate", "small changes", [this] { ctx_.session.mutate(0.5); });
    action("Mod map", "Ctrl+M", [this] { setModMap(!modMapShown()); });
    action("Simple view", "hide advanced controls", [this] { ctx_.prefs.simpleView = !ctx_.prefs.simpleView; Prefs::get().save(); applyLayout(); });
    action("Preferences", "theme, size, behaviour", [this] { showPrefs(); });
    action("FX off", "bypass the whole rack", [this] { ctx_.session.setValue(idx("global.fx_off"), ctx_.processor.plainValue(idx("global.fx_off")) >= 0.5 ? 0.0 : 1.0); });
    for (const auto& themeName : Theme::names())
        action("Theme " + themeName, "colours", [this, themeName] { ctx_.prefs.theme = themeName; Prefs::get().save(); applyTheme(); });
    for (const int s : {70, 100, 125, 150, 200, 300})
        action("Size " + juce::String(s) + " %", "window size", [this, s] { setScalePercent(s); });
    const zed::Patch patch = processor_.snapshot();
    for (const auto& def : reg.params()) {
        if (!patch.inUse(def.index))
            continue; // an empty effect slot, a route that does not exist
        const int index = def.index;
        juce::String detail = str(def.module);
        out.push_back({"Control", str(def.name), detail, [this, index] { revealParam(index); }});
    }
    const auto presets = zed::scanPresets(processor_.library().factoryDir(), ctx_.prefs.userContentFolder().getFullPathName().toStdString());
    for (const auto& p : presets) {
        const juce::File file(str(p.path));
        out.push_back({"Preset", str(p.meta.name), str(p.meta.type.empty() ? p.folder : p.meta.type), [this, file] {
                           juce::String error;
                           if (!ctx_.session.loadPresetFile(file, error))
                               help_->message(error);
                       }});
    }
    return out;
}

void ZederiaEditor::openParamMenu(ParamWidget& w) {
    const auto& reg = zed::Registry::get();
    const int index = w.param();
    const auto& own = reg.at(index);
    juce::PopupMenu menu;
    menu.addSectionHeader(w.getTitle() + "  " + w.valueText());
    juce::Component::SafePointer<ParamWidget> safe(&w);
    menu.addItem("Type a value...", [safe] { if (safe != nullptr) safe->showTypeIn(); });
    menu.addItem("Reset to " + str(zed::formatValueText(w.def(), w.def().def)), [safe] { if (safe != nullptr) safe->resetToDefault(); });
    menu.addSeparator();
    if (processor_.hostParameter(index) != nullptr) {
        if (processor_.midiLearnTarget() == index)
            menu.addItem("Cancel MIDI learn", [this] { processor_.cancelMidiLearn(); });
        else
            menu.addItem("MIDI learn (move a controller next)", [this, index] {
                processor_.startMidiLearn(index);
                help_->message("Move a MIDI controller to bind it to this control.");
            });
        const auto map = processor_.ccMap();
        for (const auto& b : map.bindings)
            if (b.param == own.id)
                menu.addItem("Forget controller CC " + juce::String(b.controller), [this, id = own.id] {
                    auto m = processor_.ccMap();
                    m.unbindParam(id);
                    processor_.setCcMap(m);
                });
        if (auto* host = getHostContext())
            if (auto hostMenu = host->getContextMenuForParameter(processor_.hostParameter(index)))
                menu.addSubMenu("Host: automation and more", hostMenu->getEquivalentPopupMenu());
    } else {
        menu.addItem("MIDI learn (not available: this setting is not automatable)", false, false, [] {});
    }
    menu.addSeparator();
    const bool locked = processor_.paramLocked(index);
    menu.addItem(locked ? "Unlock (preset loads may change it)" : "Lock (keeps its value when presets load)", [this, index, locked] {
        ctx_.session.setLocked(index, !locked);
    });
    const juce::String module = str(own.module);
    juce::String moduleName = module;
    for (const auto& m : reg.modules())
        if (m.id == own.module)
            moduleName = str(m.name);
    const bool moduleLocked = ctx_.session.moduleLocked(module);
    menu.addItem((moduleLocked ? "Unlock " : "Lock ") + moduleName, [this, module, moduleLocked] { ctx_.session.setModuleLocked(module, !moduleLocked); });
    menu.addItem("Copy value", [this, safe] { if (safe != nullptr) ctx_.session.copyText(safe->valueText()); });
    menu.addItem("Paste value", [this, safe] {
        if (safe != nullptr)
            safe->typeValue(ctx_.session.clipboardText());
    });
    juce::String moduleId = module;
    if (module.startsWith("mod."))
        moduleId.clear();
    if (!zed::moduleClass(moduleId.toStdString()).empty()) {
        menu.addItem("Copy " + moduleName, [this, moduleId] { ctx_.session.copyModule(moduleId); help_->message("Copied; paste into any module of the same kind."); });
        const bool fits = ctx_.session.clipboardClass() == str(zed::moduleClass(moduleId.toStdString()));
        menu.addItem("Paste into " + moduleName, fits, false, [this, moduleId] {
            juce::String error;
            if (!ctx_.session.pasteModule(moduleId, error))
                help_->message(error);
        });
    }
    if (own.modulatable) {
        menu.addSeparator();
        juce::PopupMenu add;
        buildSourceMenu(add, [this, index](const juce::String& source) {
            const int route = ctx_.session.addRoute(source, index, 0.25);
            if (route < 0)
                help_->message("All 64 routes are in use; remove one on the MOD page first.");
            else {
                ctx_.selectedSource = source;
                ctx_.focusRoute = route;
            }
        });
        menu.addSubMenu("Add modulation", add);
        for (const auto* r : ctx_.routesTo(index)) {
            juce::PopupMenu sub;
            const int route = r->index;
            sub.addItem(r->route.mute ? "Unmute" : "Mute (bypass)", [this, route, m = r->route.mute] { ctx_.session.setRouteMute(route, !m); });
            sub.addItem(r->route.solo ? "Unsolo" : "Solo", [this, route, s = r->route.solo] { ctx_.session.setRouteSolo(route, !s); });
            sub.addItem("Flip polarity", [this, route] { ctx_.session.flipPolarity(route); });
            sub.addItem("Show on the MOD page", [this, route] { ctx_.focusRoute = route; showPage(Mod); page(Mod).reveal("mod." + juce::String(route + 1)); });
            sub.addItem("Remove", [this, route] { ctx_.session.removeRoute(route); });
            menu.addSubMenu("Modulation: " + Context::sourceName(str(r->route.source)) + " " +
                                str(zed::formatValueText(reg.at(r->amountParam), ctx_.routeAmount(route))),
                            sub);
        }
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&w));
}

void ZederiaEditor::showMainMenu() {
    juce::PopupMenu menu;
    menu.addItem("Init", [this] { ctx_.session.initPatch(); });
    menu.addItem("Open preset file...", [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Open a preset", ctx_.prefs.userContentFolder().getChildFile("Presets"), "*.zpreset");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, chooser](const juce::FileChooser& fc) {
            juce::String error;
            if (fc.getResult().existsAsFile() && !ctx_.session.loadPresetFile(fc.getResult(), error))
                help_->message(error);
        });
    });
    menu.addItem("Save preset as...", [this] { savePresetAs(); });
    menu.addSeparator();
    juce::PopupMenu variation;
    for (const int amount : {10, 25, 50, 100})
        variation.addItem("Randomize " + juce::String(amount) + " %", [this, amount] { ctx_.session.randomize({}, amount / 100.0); });
    variation.addItem("Randomize sources only", [this] {
        zed::VariationScope s;
        s.filters = s.modulators = s.effects = false;
        ctx_.session.randomize(s, 0.6);
    });
    variation.addItem("Randomize effects only", [this] {
        zed::VariationScope s;
        s.sources = s.filters = s.modulators = false;
        ctx_.session.randomize(s, 0.6);
    });
    variation.addItem("Randomize everything, character and play too", [this] {
        zed::VariationScope s;
        s.character = s.play = true;
        ctx_.session.randomize(s, 0.5);
    });
    variation.addItem("Mutate", [this] { ctx_.session.mutate(0.5); });
    variation.addItem("Blend presets... (in the library)", [this] { showPage(Library); });
    menu.addSubMenu("Variation", variation);
    juce::PopupMenu locks;
    locks.addItem("Lock the effect rack", true, processor_.rackLocked(), [this] { processor_.setRackLocked(!processor_.rackLocked()); ctx_.session.sendChangeMessage(); });
    locks.addItem("Lock the tuning", true, processor_.engine().tuningLocked(), [this] {
        const bool on = !processor_.engine().tuningLocked();
        processor_.engine().setTuningLocked(on);
        for (const char* id : {"global.master_tune", "global.reference"})
            ctx_.session.setLocked(idx(id), on);
    });
    locks.addItem("Lock polyphony and voice mode", true, processor_.paramLocked(idx("global.polyphony")), [this] {
        const bool on = !processor_.paramLocked(idx("global.polyphony"));
        for (const char* id : {"global.polyphony", "global.mode"})
            ctx_.session.setLocked(idx(id), on);
    });
    locks.addItem("Unlock every parameter", [this] {
        for (int i = 0; i < zed::Registry::get().size(); ++i)
            if (processor_.paramLocked(i))
                processor_.setParamLocked(i, false);
        ctx_.session.sendChangeMessage();
    });
    menu.addSubMenu("Locks", locks);
    juce::PopupMenu midi;
    midi.addItem("Save controller map...", [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Save the MIDI controller map", ctx_.prefs.userContentFolder().getChildFile("Controller maps.zccmap"), "*.zccmap");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, chooser](const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                fc.getResult().replaceWithText(str(zed::saveCcMap(processor_.ccMap())));
        });
    });
    midi.addItem("Load controller map...", [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Load a MIDI controller map", ctx_.prefs.userContentFolder(), "*.zccmap");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, chooser](const juce::FileChooser& fc) {
            zed::CcMap map;
            std::vector<zed::Issue> issues;
            if (fc.getResult().existsAsFile() && zed::loadCcMap(fc.getResult().loadFileAsString().toStdString(), map, issues))
                processor_.setCcMap(map);
            else if (!issues.empty())
                help_->message(str(issues.front().message));
        });
    });
    midi.addItem("Clear controller map", [this] { processor_.setCcMap({}); });
    midi.addItem("Program list from a folder...", [this] {
        auto chooser = std::make_shared<juce::FileChooser>("Presets for program changes", ctx_.prefs.userContentFolder().getChildFile("Presets"));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, chooser](const juce::FileChooser& fc) {
            std::vector<std::string> files;
            for (const auto& f : fc.getResult().findChildFiles(juce::File::findFiles, false, "*.zpreset"))
                files.push_back(f.getFullPathName().toStdString());
            std::sort(files.begin(), files.end());
            processor_.setProgramList(files);
            help_->message(juce::String(static_cast<int>(files.size())) + " presets in the program list");
        });
    });
    menu.addSubMenu("MIDI", midi);
    menu.addSeparator();
    menu.addItem("Preferences...", [this] { showPrefs(); });
    menu.addItem("Command palette (Ctrl+K)", [this] { showPalette(); });
    menu.addItem("History", [this] { toggleHistory(); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&topBar_->menu));
}

void ZederiaEditor::savePresetAs() {
    auto dialog = std::make_shared<juce::AlertWindow>("Save preset", "Saved in your Presets folder.", juce::MessageBoxIconType::NoIcon);
    const auto meta = processor_.meta();
    dialog->addTextEditor("name", str(meta.name), "Name");
    dialog->addTextEditor("type", str(meta.type), "Type (bass, lead, pad ...)");
    juce::StringArray tags;
    for (const auto& t : meta.tags)
        tags.add(str(t));
    dialog->addTextEditor("tags", tags.joinIntoString(", "), "Tags, separated by commas");
    dialog->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, dialog](int result) {
        if (result != 1)
            return;
        zed::PatchMeta m = processor_.meta();
        m.name = dialog->getTextEditorContents("name").trim().toStdString();
        if (m.name.empty())
            m.name = "Untitled";
        m.type = dialog->getTextEditorContents("type").trim().toLowerCase().toStdString();
        m.tags.clear();
        for (const auto& t : juce::StringArray::fromTokens(dialog->getTextEditorContents("tags"), ",", ""))
            if (t.trim().isNotEmpty())
                m.tags.push_back(t.trim().toLowerCase().toStdString());
        if (m.author.empty())
            m.author = ctx_.prefs.author.toStdString();
        const juce::File file = ctx_.prefs.userContentFolder().getChildFile("Presets").getChildFile(juce::File::createLegalFileName(str(m.name)) + ".zpreset");
        juce::String error;
        if (ctx_.session.savePresetFile(file, m, error))
            help_->message("Saved " + file.getFullPathName());
        else
            help_->message(error);
    }), false);
}

void ZederiaEditor::stepPreset(int direction) {
    const auto presets = zed::scanPresets(processor_.library().factoryDir(), ctx_.prefs.userContentFolder().getFullPathName().toStdString());
    if (presets.empty()) {
        help_->message("No presets yet: save one, or import a pack in the library.");
        return;
    }
    std::vector<int> order(presets.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = static_cast<int>(i);
    zed::sortPresets(presets, order, zed::PresetSort::Name, false);
    int at = -1;
    const juce::String current = ctx_.session.currentPreset().getFullPathName();
    for (int i = 0; i < static_cast<int>(order.size()); ++i)
        if (str(presets[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])].path) == current)
            at = i;
    const int n = static_cast<int>(order.size());
    const int next = at < 0 ? (direction > 0 ? 0 : n - 1) : ((at + direction) % n + n) % n;
    juce::String error;
    if (!ctx_.session.loadPresetFile(juce::File(str(presets[static_cast<std::size_t>(order[static_cast<std::size_t>(next)])].path)), error))
        help_->message(error);
}

// --- Keys, mouse, clock --------------------------------------------------------------------------------

bool ZederiaEditor::keyPressed(const juce::KeyPress& key) {
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    if (mods.isCommandDown() && (code == 'Z' || code == 'z')) {
        if (mods.isShiftDown()) ctx_.session.redo(); else ctx_.session.undo();
        return true;
    }
    if (mods.isCommandDown() && (code == 'Y' || code == 'y')) { ctx_.session.redo(); return true; }
    if (mods.isCommandDown() && (code == 'K' || code == 'k')) { showPalette(); return true; }
    if (mods.isCommandDown() && (code == 'S' || code == 's')) { savePresetAs(); return true; }
    if (mods.isCommandDown() && (code == 'B' || code == 'b')) { ctx_.session.switchAB(); return true; }
    if (mods.isCommandDown() && (code == 'M' || code == 'm')) { setModMap(!modMapShown()); return true; }
    if (mods.isCommandDown() && code >= '1' && code <= '7') { showPage(code - '1'); return true; }
    if (code == juce::KeyPress::F1Key) {
        help_->pinned = !help_->pinned;
        help_->repaint();
        return true;
    }
    if (code == juce::KeyPress::escapeKey) {
        closeOverlays();
        setModMap(false);
        return true;
    }
    return false;
}

void ZederiaEditor::mouseMove(const juce::MouseEvent& e) {
    for (juce::Component* c = e.eventComponent; c != nullptr && c != root_.get(); c = c->getParentComponent()) {
        if (auto* w = dynamic_cast<Widget*>(c); w != nullptr && (w->helpTitle.isNotEmpty() || w->helpText.isNotEmpty())) {
            const auto* p = dynamic_cast<ParamWidget*>(w);
            help_->show(w->helpTitle, w->helpText, p != nullptr ? p->valueText() : juce::String());
            return;
        }
    }
}

void ZederiaEditor::changeListenerCallback(juce::ChangeBroadcaster*) {
    topBar_->refresh();
    if (history_->isVisible())
        history_->refresh();
}

void ZederiaEditor::timerCallback() {
    if (!isShowing()) { // hidden or minimised: nothing animates; look again twice a second
        if (getTimerInterval() < 400)
            startTimer(500);
        return;
    }
    const int interval = 1000 / juce::jmax(15, ctx_.prefs.frameCap);
    if (getTimerInterval() != interval)
        startTimer(interval);
    tickFrame();
}

void ZederiaEditor::tickTree(juce::Component& c, std::vector<juce::Rectangle<int>>& dirty) {
    if (!c.isVisible())
        return;
    if (auto* w = dynamic_cast<Widget*>(&c); w != nullptr && w->tick()) {
        w->repaint();
        dirty.push_back(getLocalArea(w, w->getLocalBounds()));
    }
    for (auto* child : c.getChildren())
        tickTree(*child, dirty);
}

void ZederiaEditor::updateHover() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    auto* candidate = hoverCandidate_.getComponent();
    if (candidate != nullptr && candidate->modulated() && candidate->isShowing() && now - hoverSince_ > 350.0) {
        if (!hoverList_->isVisible() || hoverList_->target() != candidate) {
            if (hoverList_->showFor(*candidate)) {
                const auto at = root_->getLocalArea(candidate, candidate->getLocalBounds());
                const int h = hoverList_->preferredHeight();
                auto box = juce::Rectangle<int>(300, h).withPosition(at.getRight() + 4, at.getY());
                if (box.getRight() > kBaseWidth - 4)
                    box.setX(at.getX() - 304);
                box = box.constrainedWithin(root_->getLocalBounds().reduced(4));
                hoverList_->setBounds(box);
                hoverList_->setVisible(true);
                hoverList_->toFront(false);
            }
        } else {
            hoverList_->rebuild();
        }
    } else if (candidate == nullptr && hoverList_->isVisible() && !hoverList_->isMouseOver(true) && now - hoverLeft_ > 300.0) {
        hoverList_->setVisible(false);
    }
}

std::vector<juce::Rectangle<int>> ZederiaEditor::tickFrame() {
    std::vector<juce::Rectangle<int>> dirty;
    const bool routesChanged = ctx_.refresh();
    if (processor_.stateGeneration() != seenGeneration_) {
        seenGeneration_ = processor_.stateGeneration();
        for (auto& p : pages_)
            if (p != nullptr)
                p->structureChanged();
        macros_->refreshNames();
        topBar_->refresh();
        processor_.engine().collectGarbage();
    }
    if (ctx_.session.generation() != seenSession_) {
        seenSession_ = ctx_.session.generation();
        topBar_->refresh();
    }
    if (current_ >= 0 && pages_[static_cast<std::size_t>(current_)] != nullptr)
        tickTree(*pages_[static_cast<std::size_t>(current_)], dirty);
    tickTree(*topBar_, dirty);
    if (topBar_->tick())
        dirty.push_back(getLocalArea(&topBar_->cpuText, topBar_->cpuText.getLocalBounds()));
    if (macros_->isVisible())
        tickTree(*macros_, dirty);
    if (hoverList_->isVisible())
        tickTree(*hoverList_, dirty);
    if (help_->tick())
        dirty.push_back(getLocalArea(help_.get(), help_->getLocalBounds()));
    updateHover();
    if (modMap_.isVisible() && (routesChanged || !dirty.empty())) {
        modMap_.repaint();
        dirty.push_back(getLocalArea(&modMap_, modMap_.getLocalBounds()));
    }
    return dirty;
}

// --- Interface state -----------------------------------------------------------------------------------

void ZederiaEditor::saveUiState() {
    nlohmann::json state = nlohmann::json::object();
    state["page"] = current_;
    state["scale"] = juce::roundToInt(scale() * 100.0f);
    state["selected_source"] = ctx_.selectedSource.toStdString();
    nlohmann::json pages = nlohmann::json::object();
    for (int i = 0; i < NumPages; ++i)
        if (pages_[static_cast<std::size_t>(i)] != nullptr) {
            nlohmann::json s = nlohmann::json::object();
            pages_[static_cast<std::size_t>(i)]->saveState(s);
            pages[std::to_string(i)] = s;
        }
    state["pages"] = pages;
    processor_.setUiState(state.dump());
}

void ZederiaEditor::loadUiState() {
    int pageId = Synth, percent = 0;
    nlohmann::json pages;
    try {
        const std::string text = processor_.uiState();
        if (!text.empty()) {
            const auto state = nlohmann::json::parse(text);
            pageId = juce::jlimit(0, NumPages - 1, state.value("page", 0));
            percent = state.value("scale", 0);
            ctx_.selectedSource = juce::String(state.value("selected_source", std::string()));
            if (state.contains("pages"))
                pages = state["pages"];
        }
    } catch (const std::exception&) {
    }
    if (percent > 0)
        setScalePercent(percent);
    showPage(pageId);
    if (pages.is_object())
        for (const auto& [key, value] : pages.items()) {
            const int i = std::atoi(key.c_str());
            if (i >= 0 && i < NumPages && value.is_object())
                page(i).loadState(value);
        }
    showPage(pageId);
}

juce::Component& ZederiaEditor::content() { return *root_; }
