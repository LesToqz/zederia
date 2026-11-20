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

// The scaled content: everything is laid out at 1320 x 940 and drawn through one transform.
class ZederiaEditor::Root final : public juce::Component {
public:
    explicit Root(ZederiaEditor& owner) : owner_(owner) {
        setName("root");
        setSize(kBaseWidth, kBaseHeight);
    }
    void paint(juce::Graphics& g) override { owner_.paintFrame(g); }
    void resized() override { owner_.applyLayout(); }

private:
    ZederiaEditor& owner_;
};

juce::StringArray ZederiaEditor::pageNames() { return {"SYNTH", "FX", "MOD", "TABLE", "PLAY", "LIBRARY", "AGENT"}; }

ZederiaEditor::ZederiaEditor(ZederiaProcessor& processor)
    : juce::AudioProcessorEditor(processor), processor_(processor), ctx_(processor), modMap_(ctx_) {
    Theme::setCurrent(ctx_.prefs.theme);
    setTextScale(static_cast<float>(ctx_.prefs.textSize) / 100.0f);
    lnf_.applyTheme();
    setLookAndFeel(&lnf_);
    setTitle("Zederia");
    setWantsKeyboardFocus(true); // the computer keys and the shortcuts arrive here
    root_ = std::make_unique<Root>(*this);
    addAndMakeVisible(*root_);

    topBar_ = std::make_unique<TopBar>(ctx_);
    output_ = std::make_unique<OutputPanel>(ctx_);
    tabs_ = std::make_unique<TabStrip>(pageNames());
    tabs_->large = true;
    tabs_->setTitle("Pages");
    tabs_->onChange = [this](int page) { showPage(page); };
    dock_ = std::make_unique<ModDock>(ctx_);
    keys_ = std::make_unique<KeyboardLayer>(ctx_);
    help_ = std::make_unique<HelpStrip>();
    markOverlay(*help_); // while hidden in the preferences it shows its messages over the keyboard's lower edge
    hoverList_ = std::make_unique<ModHoverList>(ctx_);
    palette_ = std::make_unique<CommandPalette>(ctx_);
    prefs_ = std::make_unique<PrefsPanel>(ctx_);
    history_ = std::make_unique<HistoryPanel>(ctx_);
    for (juce::Component* c : std::initializer_list<juce::Component*>{topBar_.get(), output_.get(), tabs_.get(), &simpleButton_, &mapButton_, &findButton_,
                                                                       &prefsButton_, dock_.get(), keys_.get(), help_.get()})
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
    ctx_.showModulator = [this](const juce::String& source) { dock_->select(source); };

    processor_.session().addChangeListener(this);
    root_->addMouseListener(this, true);

    setResizable(true, true);
    setResizeLimits(kBaseWidth * 70 / 100, kBaseHeight * 70 / 100, kBaseWidth * 3, kBaseHeight * 3);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio(static_cast<double>(kBaseWidth) / kBaseHeight);
    applyLayout();
    loadUiState(); // sets the page and the size
    ctx_.refresh();
    startTimerHz(ctx_.prefs.frameCap);
}

ZederiaEditor::~ZederiaEditor() {
    stopTimer();
    saveUiState();
    processor_.session().removeChangeListener(this);
    root_->removeMouseListener(this);
    keys_->piano().releaseAll(); // keys held with the mouse or the computer keys must not hang
    processor_.keyboardState().allNotesOff(0);
    processor_.stopAudition();
    ctx_.hoverParam = nullptr;
    for (auto& p : pages_)
        p.reset();
    setLookAndFeel(nullptr);
}

void ZederiaEditor::paint(juce::Graphics& g) { g.fillAll(theme().bg); }

juce::Rectangle<int> ZederiaEditor::pageArea() const {
    auto area = juce::Rectangle<int>(kBaseWidth, kBaseHeight).withTrimmedTop(kHeaderHeight);
    if (ctx_.prefs.helpStrip)
        area.removeFromBottom(kHelpStripHeight);
    area.removeFromBottom(kKeyboardHeight + kDockHeight);
    return area;
}

void ZederiaEditor::paintFrame(juce::Graphics& g) {
    drawChassis(g, juce::Rectangle<float>(static_cast<float>(kBaseWidth), static_cast<float>(kBaseHeight)));
    // The header is drawn once and kept: the meter and the CPU figure repaint over it every frame.
    const auto area = juce::Rectangle<int>(0, 0, kBaseWidth, kHeaderHeight);
    const auto window = topBar_ != nullptr ? topBar_->presetWindow() + topBar_->getPosition() : juce::Rectangle<int>();
    if (g.clipRegionIntersects(area))
        frameCache_.draw(g, area, static_cast<std::uint64_t>(window.getX()) << 32 | static_cast<std::uint64_t>(window.getRight()), true,
                         [this, window](juce::Graphics& hg) {
                             paintHeader(hg);
                             if (!window.isEmpty())
                                 drawWell(hg, window.toFloat()); // the preset name's display window
                         });
}

void ZederiaEditor::paintHeader(juce::Graphics& g) {
    const Theme& t = theme();
    drawChassis(g, juce::Rectangle<float>(static_cast<float>(kBaseWidth), static_cast<float>(kHeaderHeight)));
    // One machined plate across the top, the logo on its own plate at the left.
    const auto header = juce::Rectangle<float>(4.0f, 4.0f, kBaseWidth - 8.0f, kHeaderHeight - 6.0f);
    drawPanel(g, header, t.panel);
    for (const auto& p : {juce::Point<float>(header.getRight() - 8.0f, header.getY() + 8.0f), juce::Point<float>(header.getRight() - 8.0f, header.getBottom() - 8.0f)})
        drawScrew(g, p, 2.8f, p.y * 0.1f);
    const auto logo = juce::Rectangle<float>(10.0f, 9.0f, static_cast<float>(kLogoWidth) - 8.0f, kHeaderHeight - 16.0f);
    drawWell(g, logo); // the fasteners belong to the enclosure (the plate's corners), not to the logo
    // A groove sets the output (meter and Master) apart from the preset bar and the page tabs.
    const float groove = static_cast<float>(kBaseWidth - 14 - kOutputWidth - 5);
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.fillRect(groove, header.getY() + 8.0f, 1.0f, header.getHeight() - 16.0f);
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.fillRect(groove + 1.0f, header.getY() + 8.0f, 1.0f, header.getHeight() - 16.0f);
    // The wordmark, spaced out, over a small wave that turns from sine to saw.
    const juce::String word = "ZEDERIA";
    const juce::Font mark = font(19.0f / textScale(), true); // the wordmark is artwork: the text size leaves it alone
    float width = 0.0f;
    for (int i = 0; i < word.length(); ++i)
        width += juce::GlyphArrangement::getStringWidth(mark, word.substring(i, i + 1)) + (i + 1 < word.length() ? 3.0f : 0.0f);
    float x = logo.getCentreX() - width * 0.5f;
    const float y = logo.getY() + 9.0f;
    g.setFont(mark);
    for (int i = 0; i < word.length(); ++i) {
        const juce::String ch = word.substring(i, i + 1);
        const float w = juce::GlyphArrangement::getStringWidth(mark, ch);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawText(ch, juce::Rectangle<float>(x, y + 1.0f, w + 2.0f, 24.0f), juce::Justification::centredLeft, false);
        g.setColour(t.displayLine);
        g.drawText(ch, juce::Rectangle<float>(x, y, w + 2.0f, 24.0f), juce::Justification::centredLeft, false);
        x += w + 3.0f;
    }
    juce::Path wave;
    const float wy = logo.getBottom() - 16.0f, wx0 = logo.getX() + 22.0f, wx1 = logo.getRight() - 22.0f;
    for (int i = 0; i <= 64; ++i) {
        const float u = static_cast<float>(i) / 64.0f;
        const float shape = std::sin(u * juce::MathConstants<float>::twoPi * 3.0f) * (1.0f - u) + (std::fmod(u * 3.0f, 1.0f) * 2.0f - 1.0f) * u;
        const juce::Point<float> p(wx0 + (wx1 - wx0) * u, wy - shape * 5.0f);
        if (i == 0) wave.startNewSubPath(p); else wave.lineTo(p);
    }
    strokeGlow(g, wave, t.sources, 1.3f); // the wordmark's warm detail
}

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
    // Header: the logo, then the preset bar over the page tabs, and the output (meter and Master)
    // in a column of its own at the right, as tall as both rows.
    auto header = area.removeFromTop(kHeaderHeight).withTrimmedLeft(kLogoWidth + 6).withTrimmedRight(14);
    output_->setBounds(header.removeFromRight(kOutputWidth).withTrimmedTop(6).withTrimmedBottom(4));
    header.removeFromRight(10);
    topBar_->setBounds(header.removeFromTop(kTopBarHeight + 4).withTrimmedTop(4));
    auto tabRow = header.removeFromTop(kTabsHeight - 2);
    // A hidden help strip still shows messages ("Saved ...", errors) over the bottom edge for a few seconds.
    help_->messageOnly = !ctx_.prefs.helpStrip;
    help_->setVisible(ctx_.prefs.helpStrip || help_->showingMessage());
    if (ctx_.prefs.helpStrip)
        help_->setBounds(area.removeFromBottom(kHelpStripHeight));
    else
        help_->setBounds(area.withTop(area.getBottom() - kHelpStripHeight));
    keys_->setBounds(area.removeFromBottom(kKeyboardHeight));
    dock_->setBounds(area.removeFromBottom(kDockHeight));
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
    dock_->applySimpleView();
    keys_->refresh();
    modMap_.setBounds(area);
    modMap_.scope = root_.get();
    palette_->setBounds(juce::Rectangle<int>(560, 420).withCentre(root_->getLocalBounds().getCentre()).withY(80));
    prefs_->setBounds(juce::Rectangle<int>(520, 640).withCentre(root_->getLocalBounds().getCentre()));
    history_->setBounds(juce::Rectangle<int>(360, 460).withPosition(330 + kLogoWidth, kHeaderHeight - 30));
    if (isTimerRunning())
        startTimerHz(ctx_.prefs.frameCap);
    applyRenderer();
}

void ZederiaEditor::applyTheme() {
    Theme::setCurrent(ctx_.prefs.theme);
    setTextScale(static_cast<float>(ctx_.prefs.textSize) / 100.0f);
    lnf_.applyTheme();
    sendLookAndFeelChange();
    applyLayout(); // some parts size themselves to their text
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

void ZederiaEditor::parentHierarchyChanged() {
    applyRenderer();
    fitToScreen();
    takeKeysOnFirstShow();
}

// The standalone application plays from the computer keys as soon as it opens. Inside a host the
// keyboard stays with the host until the editor is clicked, so the host's own shortcuts keep working.
void ZederiaEditor::takeKeysOnFirstShow() {
    if (!focusPending_ || !isShowing() || processor_.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
        return;
    focusPending_ = false;
    if (juce::Component::getCurrentlyFocusedComponent() == nullptr)
        grabKeyboardFocus();
}

namespace {
// Tab and Shift-Tab go through the controls in order as usual; a click on something that takes no
// focus finds no control to give it to, and the focus goes up to the editor.
class EditorFocusTraverser final : public juce::FocusTraverser {
public:
    juce::Component* getDefaultComponent(juce::Component*) override { return nullptr; }
};
} // namespace

std::unique_ptr<juce::ComponentTraverser> ZederiaEditor::createFocusTraverser() { return std::make_unique<EditorFocusTraverser>(); }

// When the editor first appears on a screen it shrinks, if it must, so the whole window, keyboard
// included, fits on that display (a size stored for a larger screen would hide the bottom layers).
void ZederiaEditor::fitToScreen() {
    if (!fitPending_ || getPeer() == nullptr)
        return;
    fitPending_ = false;
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds());
    if (display == nullptr)
        return;
    const auto room = display->userBounds.toNearestInt().reduced(0, 45); // the host's title bar and borders
    const int fit = juce::jmin(room.getWidth() * 100 / kBaseWidth, room.getHeight() * 100 / kBaseHeight);
    const int current = juce::roundToInt(scale() * 100.0f);
    if (fit < current)
        setScalePercent(juce::jmax(70, fit));
}

void ZederiaEditor::visibilityChanged() {
    applyRenderer();
    fitToScreen();
    takeKeysOnFirstShow();
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
        slot->setBounds(pageArea());
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
    // The modulators and macros live in the dock, on every page.
    if (module.startsWith("env") || module.startsWith("lfo") || module.startsWith("flux") || module.startsWith("macro")) {
        dock_->select(module);
        return true;
    }
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
            if (!child->isVisible())
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
    prefs_->toFront(false); // the keyboard stays with the editor (Escape closes the panel): no focus outline in it
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
    action("Preferences", "size, text size, behaviour", [this] { showPrefs(); });
    action("All notes off", "stops every note and sound", [this] { keys_->allNotesOff(); });
    action("FX off", "bypass the whole rack", [this] { ctx_.session.setValue(idx("global.fx_off"), ctx_.processor.plainValue(idx("global.fx_off")) >= 0.5 ? 0.0 : 1.0); });
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
    const auto presets = zed::scanPresets(processor_.library().factoryDir(), ctx_.userFolder().getFullPathName().toStdString());
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

juce::String ZederiaEditor::defaultSaveLocation() const {
    // The folder of the current preset when it is one of yours in User Presets (saving again keeps
    // it in its pack), otherwise User Presets: a free or an agent's preset is saved as your copy.
    const juce::File presets = ctx_.userFolder().getChildFile("Presets");
    const juce::File user = presets.getChildFile(LibraryPage::userPresetsName());
    const juce::File current = ctx_.session.currentPreset();
    if (current.existsAsFile() && current.isAChildOf(user))
        return current.getParentDirectory().getRelativePathFrom(presets).replaceCharacter('\\', '/');
    return LibraryPage::userPresetsName();
}

bool ZederiaEditor::savePresetTo(const juce::String& name, const juce::String& location, const juce::String& type, const juce::String& tags,
                                 juce::String& error, juce::File* saved) {
    const juce::File presets = ctx_.userFolder().getChildFile("Presets");
    const juce::File folder = LibraryPage::resolveLocation(presets, location, error);
    if (folder == juce::File())
        return false;
    zed::PatchMeta m = processor_.meta();
    m.name = name.trim().toStdString();
    if (m.name.empty())
        m.name = "Untitled";
    m.type = type.trim().toLowerCase().toStdString();
    m.tags.clear();
    for (const auto& t : juce::StringArray::fromTokens(tags, ",", ""))
        if (t.trim().isNotEmpty())
            m.tags.push_back(t.trim().toLowerCase().toStdString());
    if (m.author.empty())
        m.author = ctx_.prefs.author.toStdString();
    const juce::File file = folder.getChildFile(juce::File::createLegalFileName(str(m.name)) + ".zpreset");
    if (!ctx_.session.savePresetFile(file, m, error))
        return false;
    if (saved != nullptr)
        *saved = file;
    if (auto& library = pages_[static_cast<std::size_t>(Library)]; library != nullptr)
        static_cast<LibraryPage&>(*library).rescan(); // the new preset shows at once
    return true;
}

void ZederiaEditor::savePresetAs() {
    const juce::File presets = ctx_.userFolder().getChildFile("Presets");
    LibraryPage::makeDefaultFolders(presets);
    const auto meta = processor_.meta();
    auto dialog = std::make_shared<juce::AlertWindow>(
        "Save preset", "Location is a folder in\n" + presets.getFullPathName() + "\nType User Presets/My Pack to make a folder for a pack.",
        juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor("name", str(meta.name), "Name");
    dialog->addComboBox("location", LibraryPage::folderPaths(presets), "Location");
    if (auto* where = dialog->getComboBoxComponent("location")) {
        where->setEditableText(true);
        where->setText(defaultSaveLocation(), juce::dontSendNotification);
        where->setTitle("Location");
    }
    // A folder button under the location opens Explorer's folder picker inside the Presets folder.
    struct Browse {
        zui::Button button{"Choose folder...", zui::Glyph::Folder};
        std::unique_ptr<juce::FileChooser> chooser;
    };
    auto browse = std::make_shared<Browse>();
    browse->button.setSize(170, 26);
    browse->button.setWantsKeyboardFocus(false); // no focus outline when Explorer's picker hands the window back
    browse->button.textSize = kTextSmall + 0.5f;
    browse->button.setHelp("Choose folder", "Pick the folder in Explorer; it must be inside the Presets folder.");
    juce::Component::SafePointer<juce::AlertWindow> dialogPointer(dialog.get());
    browse->button.onClick = [browse = std::weak_ptr<Browse>(browse), dialogPointer, presets] {
        auto parts = browse.lock();
        if (parts == nullptr || dialogPointer == nullptr)
            return;
        juce::String error;
        auto* where = dialogPointer->getComboBoxComponent("location");
        juce::File start = LibraryPage::resolveLocation(presets, where != nullptr ? where->getText() : juce::String(), error);
        if (!start.isDirectory())
            start = presets.getChildFile(LibraryPage::userPresetsName());
        parts->chooser = std::make_unique<juce::FileChooser>("Save the preset in this folder", start);
        parts->chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                    [dialogPointer, presets](const juce::FileChooser& fc) {
                                        const juce::File chosen = fc.getResult();
                                        if (chosen == juce::File() || dialogPointer == nullptr)
                                            return;
                                        if (chosen != presets && !chosen.isAChildOf(presets)) {
                                            juce::AlertWindow::showAsync(juce::MessageBoxOptions{}
                                                                             .withIconType(juce::MessageBoxIconType::InfoIcon)
                                                                             .withTitle("Choose a folder inside Presets")
                                                                             .withMessage("The library shows presets saved in " + presets.getFullPathName() +
                                                                                          ". Choose that folder or one inside it.")
                                                                             .withButton("OK", 1),
                                                                         [](int) {});
                                            return;
                                        }
                                        if (auto* where = dialogPointer->getComboBoxComponent("location"))
                                            where->setText(chosen == presets ? juce::String("Presets")
                                                                             : chosen.getRelativePathFrom(presets).replaceCharacter('\\', '/'),
                                                           juce::dontSendNotification);
                                    });
    };
    dialog->addCustomComponent(&browse->button);
    juce::StringArray types{"808", "bass", "synth", "lead", "pad", "pluck", "keys", "arp", "seq", "chord", "drum", "fx", "texture"};
    if (!meta.type.empty())
        types.addIfNotAlreadyThere(str(meta.type));
    dialog->addComboBox("type", types, "Type");
    if (auto* type = dialog->getComboBoxComponent("type")) {
        type->setEditableText(true);
        type->setText(str(meta.type), juce::dontSendNotification);
        type->setTitle("Type");
    }
    juce::StringArray tags;
    for (const auto& t : meta.tags)
        tags.add(str(t));
    dialog->addTextEditor("tags", tags.joinIntoString(", "), "Tags, separated by commas");
    dialog->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ZederiaEditor> safe(this);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safe, dialog, presets, browse](int result) {
        juce::ignoreUnused(browse); // the folder button lives as long as the dialog
        if (result != 1 || safe == nullptr)
            return;
        const juce::String name = dialog->getTextEditorContents("name");
        const juce::String location = dialog->getComboBoxComponent("location") != nullptr ? dialog->getComboBoxComponent("location")->getText() : juce::String();
        const juce::String type = dialog->getComboBoxComponent("type") != nullptr ? dialog->getComboBoxComponent("type")->getText() : juce::String();
        const juce::String tags = dialog->getTextEditorContents("tags");
        const auto save = [safe, name, location, type, tags] {
            if (safe == nullptr)
                return;
            juce::String error;
            juce::File file;
            if (safe->savePresetTo(name, location, type, tags, error, &file))
                safe->help_->message("Saved " + file.getFullPathName());
            else
                safe->help_->message(error);
        };
        // Replacing another preset of the same name asks first; saving the open preset again does not.
        juce::String error;
        const juce::File folder = LibraryPage::resolveLocation(presets, location, error);
        const juce::String legal = juce::File::createLegalFileName(name.trim().isEmpty() ? juce::String("Untitled") : name.trim());
        const juce::File target = folder == juce::File() ? juce::File() : folder.getChildFile(legal + ".zpreset");
        if (target.existsAsFile() && target != safe->ctx_.session.currentPreset()) {
            juce::AlertWindow::showAsync(juce::MessageBoxOptions{}
                                             .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                             .withTitle("Replace preset")
                                             .withMessage("\"" + legal + "\" already exists in " + folder.getFullPathName() + ". Replace it?")
                                             .withButton("Replace", 1)
                                             .withButton("Cancel", 0),
                                         [save](int replace) {
                                             if (replace == 1)
                                                 save();
                                         });
            return;
        }
        save();
    }), false);
}

void ZederiaEditor::stepPreset(int direction) {
    const auto presets = zed::scanPresets(processor_.library().factoryDir(), ctx_.userFolder().getFullPathName().toStdString());
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
    // Letters nobody else took play the keyboard (a text field keeps its letters).
    return keys_->piano().computerKeyDown(key);
}

bool ZederiaEditor::keyStateChanged(bool isKeyDown) {
    if (!isKeyDown)
        keys_->piano().checkHeld(); // a computer key went up: its note ends now, not at the next frame
    return false;
}

// Every click inside the editor, after the clicked part's own mouseDown. A part that keeps the focus
// to edit with keys keeps it for a click on itself (without an outline); a click anywhere else
// gives the keyboard back to the editor.
void ZederiaEditor::mouseDown(const juce::MouseEvent& e) {
    auto* focused = dynamic_cast<Widget*>(juce::Component::getCurrentlyFocusedComponent());
    if (focused == nullptr)
        return;
    if (e.eventComponent == focused || focused->isParentOf(e.eventComponent))
        focused->clickedWhileFocused();
    else
        grabKeyboardFocus();
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
        keys_->piano().checkHeld(); // a key let go while the window was hidden still ends its note
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
        for (const auto& r : w->dirtyArea()) {
            const auto area = r.getIntersection(w->getLocalBounds());
            if (!area.isEmpty()) {
                w->repaint(area);
                dirty.push_back(getLocalArea(w, area));
            }
        }
    }
    for (auto* child : c.getChildren())
        tickTree(*child, dirty);
}

void ZederiaEditor::updateHover() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    auto* candidate = hoverCandidate_.getComponent();
    if (candidate != nullptr && hoverList_->isParentOf(candidate))
        candidate = nullptr; // the list's own amount controls leave it as it is
    if (candidate != nullptr && candidate->modulated() && zui::visibleWithin(*candidate, this) && now - hoverSince_ > 350.0) {
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
        }
    } else if (candidate == nullptr && hoverList_->isVisible() && !hoverList_->isMouseOver(true) && now - hoverLeft_ > 300.0) {
        hoverList_->setVisible(false);
    }
    // While open (the pointer on its target or in the list itself), the list follows the routes:
    // a mute, solo, flip or removal made in it shows at once, and it closes with its last route.
    if (hoverList_->isVisible()) {
        hoverList_->rebuild();
        if (hoverList_->target() == nullptr || hoverList_->rowCount() == 0)
            hoverList_->setVisible(false);
        else if (hoverList_->getHeight() != hoverList_->preferredHeight())
            hoverList_->setSize(hoverList_->getWidth(), hoverList_->preferredHeight());
    }
}

std::vector<juce::Rectangle<int>> ZederiaEditor::tickFrame() {
    std::vector<juce::Rectangle<int>> dirty;
    if (processor_.uiStateRestores() != seenRestores_)
        loadUiState(); // a project was opened while the editor was open
    const bool routesChanged = ctx_.refresh();
    if (processor_.stateGeneration() != seenGeneration_) {
        seenGeneration_ = processor_.stateGeneration();
        for (auto& p : pages_)
            if (p != nullptr)
                p->structureChanged();
        dock_->refreshNames();
        dock_->reloadShapes();
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
    tickTree(*output_, dirty);
    if (topBar_->tick())
        dirty.push_back(getLocalArea(&topBar_->cpuText, topBar_->cpuText.getLocalBounds()));
    // The fixed layers under the page.
    tickTree(*dock_, dirty);
    if (dock_->tickDock())
        dirty.push_back(getLocalArea(dock_.get(), dock_->getLocalBounds()));
    tickTree(*keys_, dirty);
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
    if (processor_.uiStateRestores() != seenRestores_)
        return; // the host restored a project since this editor opened: keep what it restored
    nlohmann::json state = nlohmann::json::object();
    state["page"] = current_;
    state["scale"] = juce::roundToInt(scale() * 100.0f);
    state["selected_source"] = ctx_.selectedSource.toStdString();
    state["modulator"] = dock_->selected().toStdString();
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
    seenRestores_ = processor_.uiStateRestores();
    int pageId = Synth, percent = 0;
    nlohmann::json pages;
    try {
        const std::string text = processor_.uiState();
        if (!text.empty()) {
            const auto state = nlohmann::json::parse(text);
            pageId = juce::jlimit(0, NumPages - 1, state.value("page", 0));
            percent = state.value("scale", 0);
            ctx_.selectedSource = juce::String(state.value("selected_source", std::string()));
            if (const juce::String modulator(state.value("modulator", std::string())); modulator.isNotEmpty())
                dock_->select(modulator);
            if (state.contains("pages"))
                pages = state["pages"];
        }
    } catch (const std::exception&) {
    }
    setScalePercent(percent > 0 ? percent : ctx_.prefs.defaultScale); // the resize limits may already have set a size
    fitPending_ = true; // once on a screen: never larger than the screen
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
