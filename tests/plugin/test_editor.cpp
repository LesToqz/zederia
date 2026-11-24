// The interface (milestone 5): snapshots with the automatic checks at three scales, theme contrast,
// every parameter reachable, undo and redo round trips, modulation gestures, state across editor
// and host sessions, keyboard and screen-reader access, typed values, the command palette, the
// frame clock, the interface's cost, workshop latency, Simple view, rack and library actions, and
// the keyboard (mouse, computer keys, releases that never arrive) and the wheels.
#include "../support/TestUtil.h"
#include "AgentLink.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Prefs.h"
#include "Session.h"
#include "ui/AgentPage.h"
#include "ui/Audit.h"
#include "ui/FxPage.h"
#include "ui/LibraryPage.h"
#include "ui/ModPage.h"
#include "ui/PlayPage.h"
#include "ui/SynthPage.h"
#include "ui/TablePage.h"
#include "ui/Brand.h"
#include "ui/ContentMenu.h"
#include "ui/CurveEditor.h"
#include "ui/Theme.h"
#include "zederia/midi/MidiFile.h"
#include "zederia/patch/Modules.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/table/Formula.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <set>
#include <thread>

using namespace zed;
using zedtest::idx;
using zui::str;

namespace {

// Preferences of this test process: defaults, in a file of its own (never the person's settings).
const bool kPrefsRedirected = [] {
    Prefs::useFile(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_settings_" + juce::Uuid().toString() + ".json"));
    return true;
}();

juce::File snapshotFolder() { return juce::File(juce::String(ZED_SNAPSHOT_DIR)); }

std::string readPatch(const char* name) { return zedtest::readText(zedtest::dataPath(std::string("patches/") + name)); }

struct Rig {
    ZederiaProcessor processor;
    juce::AudioBuffer<float> buffer{2, 512};
    juce::MidiBuffer midi;
    std::unique_ptr<juce::AudioProcessorEditor> base;
    ZederiaEditor* editor = nullptr;
    juce::File userFolder;

    explicit Rig(const char* patch = nullptr, bool withLibrary = false) {
        (void)kPrefsRedirected;
        processor.setPlayConfigDetails(0, 2, 48000.0, 512);
        processor.prepareToPlay(48000.0, 512);
        midi.ensureSize(4096);
        // Every rig has a content folder of its own: the library makes its default folders in the
        // user folder, and tests never touch the person's Documents.
        userFolder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_library_" + juce::Uuid().toString());
        userFolder.createDirectory();
        if (withLibrary) {
            const auto presets = userFolder.getChildFile("Presets").getChildFile("Tests");
            presets.createDirectory();
            for (const char* name : {"bass", "pad", "pluck", "lead", "arp_fx", "glass_pluck"})
                presets.getChildFile(juce::String(name) + ".zpreset").replaceWithText(juce::String::fromUTF8(readPatch((std::string(name) + ".zpreset").c_str()).c_str()));
            processor.setContentFolders("", userFolder.getFullPathName().toStdString());
        } else {
            processor.setContentFolders(processor.library().factoryDir(), userFolder.getFullPathName().toStdString());
        }
        if (patch != nullptr) {
            REQUIRE(processor.loadPatchJson(readPatch(patch)).ok());
            processor.session().resetHistory("Opened");
        }
        open();
    }
    ~Rig() {
        base.reset();
        if (userFolder.isDirectory())
            userFolder.deleteRecursively();
    }
    void open() {
        base.reset(); // a host closes the editor before it opens another
        base.reset(processor.createEditor());
        editor = dynamic_cast<ZederiaEditor*>(base.get());
        REQUIRE(editor != nullptr);
    }
    void run(int blocks = 1) {
        for (int b = 0; b < blocks; ++b) {
            buffer.clear();
            processor.processBlock(buffer, midi);
            midi.clear();
        }
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            run(1);
            editor->tickFrame();
        }
    }
    zui::Context& ctx() { return editor->context(); }
};

template <class T>
void collect(juce::Component& root, std::vector<T*>& out) {
    for (auto* child : root.getChildren()) {
        if (auto* t = dynamic_cast<T*>(child))
            out.push_back(t);
        collect(*child, out);
    }
}

template <class T>
std::vector<T*> all(juce::Component& root) {
    std::vector<T*> out;
    collect(root, out);
    return out;
}

template <class T>
T* visibleWithParam(juce::Component& root, const char* id) {
    for (auto* w : all<T>(root))
        if (w->param() == idx(id) && zui::visibleWithin(*w, root.getTopLevelComponent()))
            return w;
    return nullptr;
}

juce::MouseEvent mouse(juce::Component& c, juce::Point<float> at, juce::ModifierKeys mods = {}, juce::Point<float> down = {-1, -1}, int clicks = 1) {
    if (down.x < 0)
        down = at;
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, down, now, clicks, at != down);
}

void showForCheck(Rig& rig) { rig.frames(3); }

} // namespace

TEST_CASE("editor: every page renders at 100, 150 and 200 % without clipped, overlapping, unlabeled or truncated parts", "[editor][snapshot]") {
    Rig rig("arp_fx.zpreset", true);
    rig.midi.addEvent(juce::MidiMessage::noteOn(1, 48, 0.8f), 0);
    rig.midi.addEvent(juce::MidiMessage::noteOn(1, 55, 0.8f), 0);
    rig.frames(10);
    const auto folder = snapshotFolder();
    folder.createDirectory();
    static const char* const names[] = {"synth", "fx", "mod", "table", "play", "library", "agent"};
    int total = 0;
    for (const auto& themeName : zui::Theme::names()) {
        rig.ctx().prefs.theme = themeName; // the editor takes its theme from the preferences
        rig.editor->applyTheme();
        REQUIRE(zui::Theme::current().name == themeName);
        const std::vector<int> scales = themeName == "Graphite" ? std::vector<int>{100, 150, 200} : std::vector<int>{100};
        for (int page = 0; page < 7; ++page) {
            for (const int scale : scales) {
                rig.editor->showPage(page);
                rig.editor->setScalePercent(scale);
                showForCheck(rig);
                std::vector<zui::Finding> findings;
                const auto image = zui::renderSnapshot(*rig.editor, &findings);
                for (auto& f : zui::checkLayout(*rig.editor))
                    findings.push_back(f);
                juce::String file = juce::String(names[page]) + "_" + juce::String(scale);
                if (themeName != "Graphite")
                    file << "_" << themeName.toLowerCase();
                REQUIRE(zui::writePng(image, folder.getChildFile(file + ".png")));
                CHECK(image.getWidth() == 1320 * scale / 100);
                for (const auto& f : findings)
                    UNSCOPED_INFO(file << ": " << f.kind << ": " << f.where << ": " << f.detail);
                CHECK(findings.empty());
                total += static_cast<int>(findings.size());
            }
        }
    }
    rig.ctx().prefs.theme = "Graphite";
    rig.editor->applyTheme();
    // Open overlays are checked too: the palette, preferences, history, the hover list and the mod map.
    rig.editor->setScalePercent(100);
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.editor->showPalette();
    rig.editor->palette().setQuery("cutoff");
    std::vector<zui::Finding> overlayFindings;
    zui::writePng(zui::renderSnapshot(*rig.editor, &overlayFindings), folder.getChildFile("overlay_palette_100.png"));
    rig.editor->showPrefs();
    zui::writePng(zui::renderSnapshot(*rig.editor, &overlayFindings), folder.getChildFile("overlay_prefs_100.png"));
    rig.editor->toggleHistory();
    zui::writePng(zui::renderSnapshot(*rig.editor, &overlayFindings), folder.getChildFile("overlay_history_100.png"));
    rig.editor->closeOverlays();
    rig.editor->setModMap(true);
    rig.frames(2);
    zui::writePng(zui::renderSnapshot(*rig.editor, &overlayFindings), folder.getChildFile("overlay_modmap_100.png"));
    for (auto& f : zui::checkLayout(*rig.editor))
        overlayFindings.push_back(f);
    rig.editor->setModMap(false);
    // The signal map, shown large over the oscillators.
    dynamic_cast<zui::SynthPage&>(rig.editor->page(ZederiaEditor::Synth)).showSignalMap();
    rig.frames(2);
    zui::writePng(zui::renderSnapshot(*rig.editor, &overlayFindings), folder.getChildFile("overlay_signalmap_100.png"));
    for (const auto& f : overlayFindings)
        UNSCOPED_INFO("overlay: " << f.kind << ": " << f.where << ": " << f.detail);
    CHECK(overlayFindings.empty());
    INFO("snapshots in " << folder.getFullPathName());
    CHECK(total == 0);
}

TEST_CASE("editor: theme colours have the contrast they need", "[editor][theme]") {
    for (const auto& name : zui::Theme::names()) {
        zui::Theme::setCurrent(name);
        const auto findings = zui::checkTheme(zui::Theme::current());
        for (const auto& f : findings)
            UNSCOPED_INFO(f.where << ": " << f.detail);
        CHECK(findings.empty());
    }
    zui::Theme::setCurrent("Graphite");
    CHECK(zui::Theme::names() == juce::StringArray{"Graphite"}); // the only theme
}

TEST_CASE("editor: every parameter of the registry can be reached from the interface", "[editor][reach]") {
    const auto& reg = Registry::get();
    // A patch that uses everything that can be used: every slot holds an effect with the most
    // parameters, and all 64 routes exist.
    Patch patch;
    int widest = 0;
    for (int t = 0; t < static_cast<int>(reg.effectTypes().size()); ++t)
        if (reg.effectTypes()[static_cast<std::size_t>(t)].params.size() > reg.effectTypes()[static_cast<std::size_t>(widest)].params.size())
            widest = t;
    for (int lane = 0; lane < kNumFxLanes; ++lane)
        for (int s = 0; s < kNumFxSlots; ++s)
            patch.addFxSlot(lane, widest);
    for (int r = 0; r < kNumModRoutes; ++r)
        patch.routes.push_back({"lfo1", "", "filter1.cutoff"});
    Rig rig;
    REQUIRE(rig.processor.loadPatchJson(savePatch(patch)).ok());
    rig.frames(2);
    std::set<int> reached;
    const auto gather = [&] {
        for (auto* w : all<zui::ParamWidget>(*rig.editor))
            reached.insert(w->param());
    };
    for (int page = 0; page < 7; ++page) {
        rig.editor->showPage(page);
        rig.frames(1);
        gather();
    }
    auto& fx = dynamic_cast<zui::FxPage&>(rig.editor->page(ZederiaEditor::Fx));
    for (int lane = 0; lane < kNumFxLanes; ++lane)
        for (int s = 0; s < kNumFxSlots; ++s) {
            fx.select(lane, s);
            gather();
        }
    rig.editor->showPrefs();
    gather();
    // No effect type uses a sixteenth slot parameter: those are reserved and have nothing to show.
    int widestCount = 0;
    for (const auto& t : reg.effectTypes())
        widestCount = std::max(widestCount, static_cast<int>(t.params.size()));
    CHECK(widestCount <= kNumFxSlotParams);
    int missing = 0;
    for (int i = 0; i < reg.size(); ++i) {
        int lane = 0, slot = 0, k = 0;
        if (reg.fxSlotParamAt(i, lane, slot, k) && k >= widestCount)
            continue;
        if (!reached.count(i)) {
            UNSCOPED_INFO("not reachable: " << reg.at(i).id);
            ++missing;
        }
    }
    CHECK(missing == 0);
    CHECK(reached.size() >= static_cast<std::size_t>(reg.size() - kNumFxLanes * kNumFxSlots * (kNumFxSlotParams - widestCount)));
}

TEST_CASE("editor: undo and redo round trips over parameters, routes, shapes, tables, rack edits and presets", "[editor][undo]") {
    Rig rig(nullptr, true);
    auto& session = rig.processor.session();
    const Patch start = rig.processor.snapshot();
    std::vector<Patch> states{start};
    const auto step = [&] { states.push_back(rig.processor.snapshot()); };
    // A knob gesture.
    auto& synth = dynamic_cast<zui::SynthPage&>(rig.editor->page(ZederiaEditor::Synth));
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(1);
    auto* cutoff = visibleWithParam<zui::Knob>(synth, "filter1.cutoff");
    REQUIRE(cutoff != nullptr);
    const auto c = cutoff->getLocalBounds().getCentre().toFloat();
    cutoff->mouseDown(mouse(*cutoff, c));
    cutoff->mouseDrag(mouse(*cutoff, c.translated(0, -40), {}, c));
    cutoff->mouseUp(mouse(*cutoff, c.translated(0, -40), {}, c));
    CHECK(rig.processor.plainValue(idx("filter1.cutoff")) > 2000.0);
    step();
    // A route by dropping a chip, its depth by alt-drag.
    juce::DragAndDropTarget::SourceDetails drop("mod:lfo2", nullptr, {});
    cutoff->itemDropped(drop);
    REQUIRE(rig.processor.routes().size() == 1);
    step();
    rig.frames(1);
    cutoff->mouseDown(mouse(*cutoff, c, juce::ModifierKeys::altModifier));
    cutoff->mouseDrag(mouse(*cutoff, c.translated(0, -30), juce::ModifierKeys::altModifier, c));
    cutoff->mouseUp(mouse(*cutoff, c.translated(0, -30), juce::ModifierKeys::altModifier, c));
    CHECK(rig.processor.plainValue(Registry::get().modAmount(0)) > 0.25);
    step();
    // A shape.
    Shape lfo;
    lfo.points = {{0.0, -1.0, 0.0}, {0.5, 1.0, 0.4}, {1.0, -1.0, 0.0}};
    rig.processor.setLfoShapeLive(1, lfo);
    session.commit("LFO 2 shape");
    step();
    // A table drawn in the workshop.
    auto& table = dynamic_cast<zui::TablePage&>(rig.editor->page(ZederiaEditor::Table));
    table.setOsc(0);
    table.drawStroke(0.1, 0.8, 0.4, -0.8, 6);
    step();
    // Rack edits on the FX page (click-free moves).
    auto& fx = dynamic_cast<zui::FxPage&>(rig.editor->page(ZederiaEditor::Fx));
    REQUIRE(fx.insertEffect(0, 0, Registry::get().effectIndex("chorus")));
    step();
    REQUIRE(fx.insertEffect(0, 1, Registry::get().effectIndex("delay")));
    step();
    REQUIRE(fx.moveEffect(0, 1, 2, 0));
    step();
    // A preset load and the A/B switch.
    juce::String error;
    REQUIRE(session.loadPresetFile(rig.userFolder.getChildFile("Presets/Tests/pad.zpreset"), error));
    step();
    session.copyToOtherSide(); // B holds the pad as loaded
    session.setValue(idx("osc1.position"), 0.3);
    step();
    session.switchAB();
    step();
    session.randomize({}, 0.5);
    step();
    const int steps = static_cast<int>(states.size()) - 1;
    CHECK(session.history().size() == steps + 1);
    for (int i = steps; i > 0; --i) {
        INFO("undo to step " << i - 1 << ": " << session.history().label(i));
        REQUIRE(session.undo());
        rig.frames(1);
        CHECK(rig.processor.snapshot() == states[static_cast<std::size_t>(i - 1)]);
    }
    CHECK_FALSE(session.undo());
    for (int i = 1; i <= steps; ++i) {
        REQUIRE(session.redo());
        rig.frames(1);
        CHECK(rig.processor.snapshot() == states[static_cast<std::size_t>(i)]);
    }
    // The history names its steps.
    CHECK(str(session.history().label(1)).startsWith("Filter 1 Cutoff"));
    CHECK(str(session.history().label(2)).contains("LFO 2"));
    CHECK(str(session.history().label(9)).startsWith("Load preset"));
}

TEST_CASE("editor: modulation as in plan 3.7: drop, depth arcs, live marker, hover list, polarity, mute, solo, remove, mod map", "[editor][modulation]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(2);
    auto* position = visibleWithParam<zui::Knob>(*rig.editor, "osc1.position");
    REQUIRE(position != nullptr);
    // Drop a chip: a route with a visible amount, and the chip's source selected.
    position->itemDropped(juce::DragAndDropTarget::SourceDetails("mod:lfo1", nullptr, {}));
    REQUIRE(rig.processor.routes().size() == 1);
    CHECK(rig.ctx().selectedSource == "lfo1");
    rig.frames(2);
    REQUIRE(position->arcs().size() == 1);
    CHECK(position->arcs()[0].high > 0.0);
    // A note: the live value moves.
    rig.midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
    rig.processor.session().setValue(idx("lfo1.rate"), 8.0);
    rig.frames(20);
    CHECK(position->liveNorm() >= 0.0);
    // Alt-drag with another selected source creates and sets its route; Shift+Alt-click flips polarity.
    rig.ctx().selectedSource = "env2";
    const auto c = position->getLocalBounds().getCentre().toFloat();
    position->mouseDown(mouse(*position, c, juce::ModifierKeys::altModifier));
    position->mouseDrag(mouse(*position, c.translated(0, -45), juce::ModifierKeys::altModifier, c));
    position->mouseUp(mouse(*position, c.translated(0, -45), juce::ModifierKeys::altModifier, c));
    REQUIRE(rig.processor.routes().size() == 2);
    CHECK(rig.processor.routes()[1].source == "env2");
    CHECK(rig.processor.plainValue(Registry::get().modAmount(1)) > 0.2);
    const auto before = rig.processor.routes()[1].polarity;
    position->mouseDown(mouse(*position, c, juce::ModifierKeys(juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier)));
    CHECK(rig.processor.routes()[1].polarity != before);
    rig.frames(2);
    CHECK(position->arcs().size() == 2);
    // Hovering a source lights up its destinations.
    rig.ctx().hoveredSource = "lfo1";
    rig.frames(1);
    rig.ctx().hoveredSource.clear();
    // Hover list: appears after a moment, with editable amounts, mute, solo and remove.
    rig.ctx().hoverParam(position);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    rig.frames(1);
    auto& list = rig.editor->hoverList();
    REQUIRE(list.isVisible());
    CHECK(list.target() == position);
    auto buttons = all<zui::Button>(list);
    const auto find = [&](const juce::String& title) -> zui::Button* {
        for (auto* b : buttons)
            if (b->getTitle() == title)
                return b;
        return nullptr;
    };
    // The pointer moves from the control into the list: the list stays and follows each change.
    rig.ctx().hoverParam(nullptr);
    REQUIRE(find("Mute LFO 1") != nullptr);
    find("Mute LFO 1")->onClick();
    CHECK(rig.processor.routes()[0].mute);
    rig.frames(1);
    buttons = all<zui::Button>(list);
    REQUIRE(find("Solo Env 2") != nullptr);
    find("Solo Env 2")->onClick();
    CHECK(rig.processor.routes()[1].solo);
    rig.frames(1);
    buttons = all<zui::Button>(list);
    REQUIRE(find("Remove Env 2 route") != nullptr);
    find("Remove Env 2 route")->onClick();
    CHECK(rig.processor.routes().size() == 1);
    // The amount in the list is a control of its own.
    rig.frames(1);
    CHECK(list.isVisible());
    buttons = all<zui::Button>(list);
    CHECK(find("Remove Env 2 route") == nullptr);
    CHECK(find("Mute LFO 1") != nullptr);
    const auto faders = all<zui::Fader>(list);
    REQUIRE(faders.size() == 1);
    CHECK(faders[0]->param() == Registry::get().modAmount(0));
    // The mod map draws the connection on the page.
    rig.ctx().hoverParam(nullptr);
    rig.editor->setModMap(true);
    juce::Image image(juce::Image::ARGB, 1320, 820, true, juce::SoftwareImageType());
    {
        juce::Graphics g(image);
        rig.editor->paintEntireComponent(g, true);
    }
    CHECK(rig.editor->modMap().connectionsDrawn() == 1);
    // The MOD page lists, searches, sorts and replaces sources in bulk.
    rig.processor.session().addRoute("macro1", idx("filter1.resonance"), 0.5);
    rig.processor.session().addRoute("lfo3", idx("osc2.position"), 0.3);
    rig.editor->showPage(ZederiaEditor::Mod);
    rig.frames(1);
    auto& mod = dynamic_cast<zui::ModPage&>(rig.editor->page(ZederiaEditor::Mod));
    CHECK(mod.shownRoutes().size() == 3);
    mod.setSearch("filter 1");
    CHECK(mod.shownRoutes().size() == 1);
    mod.setSearch("");
    mod.setSort(zui::ModPage::Sort::Amount);
    CHECK(mod.shownRoutes().front() == 1); // macro 1, 50 %
    mod.setSelected(0, true);
    mod.setSelected(2, true);
    mod.replaceSelectedSource("flux1");
    CHECK(rig.processor.routes()[0].source == "flux1");
    CHECK(rig.processor.routes()[2].source == "flux1");
    CHECK(rig.processor.routes()[1].source == "macro1");
}

TEST_CASE("editor: state survives closing and reopening the editor and the host project", "[editor][state]") {
    juce::MemoryBlock state;
    {
        Rig rig("arp_fx.zpreset");
        rig.editor->showPage(ZederiaEditor::Fx);
        rig.editor->setScalePercent(150);
        dynamic_cast<zui::FxPage&>(rig.editor->page(ZederiaEditor::Fx)).select(0, 1);
        rig.processor.session().setValue(idx("osc1.position"), 0.4);
        rig.processor.session().setLocked(idx("filter1.cutoff"), true);
        rig.processor.session().switchAB(); // B is a copy of A: the other side is kept
        rig.processor.session().setValue(idx("osc1.position"), 0.6);
        rig.open(); // closes and reopens the editor
        CHECK(rig.editor->currentPage() == ZederiaEditor::Fx);
        CHECK(rig.editor->getWidth() == 1980);
        CHECK(dynamic_cast<zui::FxPage&>(rig.editor->page(ZederiaEditor::Fx)).selectedSlot() == 1);
        rig.processor.getStateInformation(state);
    }
    Rig other;
    other.processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    other.open();
    CHECK(other.editor->currentPage() == ZederiaEditor::Fx);
    CHECK(other.editor->getWidth() == 1980);
    CHECK(other.processor.plainValue(idx("osc1.position")) == Catch::Approx(0.6));
    CHECK(other.processor.paramLocked(idx("filter1.cutoff")));
    CHECK(other.processor.session().abSide() == 1);
    other.processor.session().switchAB();
    CHECK(other.processor.plainValue(idx("osc1.position")) == Catch::Approx(0.4));
    // A locked parameter keeps its value when a preset loads.
    other.processor.session().setValue(idx("filter1.cutoff"), 345.0);
    Patch preset;
    preset.set(idx("filter1.cutoff"), 5000.0);
    preset.set(idx("filter1.resonance"), 0.7);
    other.processor.loadPresetPatch(preset);
    CHECK(other.processor.plainValue(idx("filter1.cutoff")) == Catch::Approx(345.0));
    CHECK(other.processor.plainValue(idx("filter1.resonance")) == Catch::Approx(0.7));
}

TEST_CASE("editor: every control has a name and a value for screen readers and works from the keyboard", "[editor][access]") {
    Rig rig("arp_fx.zpreset");
    int controls = 0;
    for (int page = 0; page < 7; ++page) {
        rig.editor->showPage(page);
        rig.frames(1);
        for (auto* c : all<juce::Component>(*rig.editor)) {
            if (!zui::visibleWithin(*c, rig.editor) || !c->getWantsKeyboardFocus())
                continue;
            ++controls;
            INFO(zui::componentPath(*c));
            CHECK(c->getTitle().trim().isNotEmpty());
            // Without a window the framework makes no handler on demand: make the one a screen reader gets.
            std::unique_ptr<juce::AccessibilityHandler> handler;
            if (auto* w = dynamic_cast<zui::ParamWidget*>(c))
                handler = w->createAccessibilityHandler();
            else if (auto* b = dynamic_cast<zui::Button*>(c))
                handler = b->createAccessibilityHandler();
            else if (auto* chip = dynamic_cast<zui::DragChip*>(c))
                handler = chip->createAccessibilityHandler();
            else if (auto* e = dynamic_cast<zui::CurveEditor*>(c))
                handler = e->createAccessibilityHandler();
            else
                continue; // the framework's own parts (text fields, lists) bring theirs
            REQUIRE(handler != nullptr);
            CHECK(handler->getRole() != juce::AccessibilityRole::ignored);
            if (auto* w = dynamic_cast<zui::ParamWidget*>(c)) {
                REQUIRE(handler->getValueInterface() != nullptr);
                CHECK(handler->getValueInterface()->getCurrentValueAsString() == w->valueText());
            }
        }
    }
    CHECK(controls > 300);
    // Keys move values, Enter opens typed entry, Space switches.
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(1);
    auto* level = visibleWithParam<zui::Knob>(*rig.editor, "osc1.level");
    REQUIRE(level != nullptr);
    const double before = rig.processor.plainValue(idx("osc1.level"));
    CHECK(level->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    CHECK(rig.processor.plainValue(idx("osc1.level")) > before);
    CHECK(level->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(rig.processor.plainValue(idx("osc1.level")) == Catch::Approx(Registry::get().find("osc1.level")->def));
    auto* power = visibleWithParam<zui::Toggle>(*rig.editor, "filter1.on");
    REQUIRE(power != nullptr);
    const bool on = rig.processor.plainValue(idx("filter1.on")) >= 0.5;
    CHECK(power->keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    CHECK((rig.processor.plainValue(idx("filter1.on")) >= 0.5) != on);
    // Screen readers can set a value with units.
    level->createAccessibilityHandler()->getValueInterface()->setValueAsString("-12 dB");
    CHECK(rig.processor.plainValue(idx("osc1.level")) == Catch::Approx(-12.0));
    // The help strip describes what is under the pointer.
    rig.editor->mouseMove(mouse(*level, {5.0f, 5.0f}));
    CHECK(rig.editor->helpStrip().title() == level->getTitle());
}

TEST_CASE("editor: typed values with units, note lengths that stay linked, and note names", "[editor][values]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(1);
    auto* cutoff = visibleWithParam<zui::Knob>(*rig.editor, "filter1.cutoff");
    REQUIRE(cutoff != nullptr);
    CHECK(cutoff->typeValue("2.5k"));
    CHECK(rig.processor.plainValue(idx("filter1.cutoff")) == Catch::Approx(2500.0));
    CHECK(cutoff->typeValue("A4"));
    CHECK(rig.processor.plainValue(idx("filter1.cutoff")) == Catch::Approx(440.0));
    juce::String error;
    CHECK_FALSE(cutoff->typeValue("very bright", &error));
    CHECK(error.isNotEmpty());
    // An envelope time typed as a note length follows the tempo and shows the length.
    auto* attack = visibleWithParam<zui::Knob>(*rig.editor, "env1.attack");
    REQUIRE(attack != nullptr);
    CHECK(attack->typeValue("1/8"));
    std::string token;
    rig.processor.readDocument([&](const Patch& p) { token = p.envSync[0][1]; });
    CHECK(token == "1/8");
    CHECK(attack->valueText() == "1/8");
    CHECK(attack->typeValue("20 ms"));
    rig.processor.readDocument([&](const Patch& p) { token = p.envSync[0][1]; });
    CHECK(token.empty());
    // An LFO rate typed as a note length switches the LFO to its synced rate.
    dynamic_cast<zui::SynthPage&>(rig.editor->page(ZederiaEditor::Synth)).reveal("lfo3");
    rig.frames(1);
    auto* rate = visibleWithParam<zui::Knob>(*rig.editor, "lfo3.rate");
    REQUIRE(rate != nullptr);
    CHECK(rate->typeValue("1/16T"));
    CHECK(rig.processor.plainValue(idx("lfo3.sync")) == 1.0);
    CHECK(rig.processor.plainValue(idx("lfo3.division")) == Registry::get().find("lfo3.division")->choiceIndex("1/16T"));
}

TEST_CASE("editor: the command palette finds controls, actions, pages and presets", "[editor][palette]") {
    Rig rig(nullptr, true);
    rig.editor->showPalette();
    auto& palette = rig.editor->palette();
    palette.setQuery("filter 1 cutoff");
    REQUIRE(palette.matchCount() > 0);
    CHECK(palette.match(0).name == "Filter 1 Cutoff");
    CHECK(palette.runMatch(0));
    CHECK(rig.editor->currentPage() == ZederiaEditor::Synth);
    rig.editor->showPalette();
    palette.setQuery("library");
    REQUIRE(palette.matchCount() > 0);
    CHECK(palette.match(0).kind == "Page");
    palette.setQuery("test pad");
    bool presetFound = false;
    for (int i = 0; i < palette.matchCount(); ++i)
        presetFound = presetFound || (palette.match(i).kind == "Preset" && palette.match(i).name == "Test Pad");
    CHECK(presetFound);
    palette.setQuery("randomize");
    REQUIRE(palette.matchCount() > 0);
    CHECK(palette.match(0).kind == "Action");
}

TEST_CASE("editor: the frame clock repaints only what changes, nothing when idle, and stops when hidden", "[editor][frames]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(30); // settle
    // The CPU readout is the one exception: it shows measured time, which moves on its own (it
    // changes at most twice a second, and only when the figure it shows changes).
    const auto cpuArea = rig.editor->getLocalArea(&rig.editor->topBar().cpuText, rig.editor->topBar().cpuText.getLocalBounds());
    for (int i = 0; i < 10; ++i) {
        rig.run(1);
        auto dirty = rig.editor->tickFrame();
        std::erase(dirty, cpuArea);
        for (const auto& r : dirty)
            UNSCOPED_INFO("repainted " << r.toString());
        CHECK(dirty.empty());
    }
    // A value change repaints its control, not the page.
    rig.processor.session().setValue(idx("osc1.position"), 0.5);
    const auto dirty = rig.editor->tickFrame();
    REQUIRE_FALSE(dirty.empty());
    int area = 0;
    for (const auto& r : dirty)
        area += r.getWidth() * r.getHeight();
    CHECK(area < 1320 * 820 / 2);
    // Not on screen (no window): the clock runs only a slow check, so nothing animates.
    CHECK_FALSE(rig.editor->isShowing());
}

// Plan 5.8's budgets are for optimized builds; a Debug build runs the measurements and prints them
// but says nothing about the budgets.
#ifdef NDEBUG
constexpr bool kBudgetsApply = true;
#else
constexpr bool kBudgetsApply = false;
#endif

TEST_CASE("editor: interface cost while animating stays within plan 5.8", "[editor][perf]") {
    Rig rig;
    rig.processor.session().setValue(idx("osc1.unison.voices"), 5);
    rig.processor.session().addRoute("lfo1", idx("osc1.position"), 0.6);
    rig.processor.session().addRoute("lfo2", idx("filter1.cutoff"), 0.5);
    rig.processor.session().setValue(idx("filter1.on"), 1.0);
    rig.processor.session().setValue(idx("lfo1.rate"), 0.7);
    rig.processor.session().setValue(idx("lfo2.rate"), 3.0);
    rig.midi.addEvent(juce::MidiMessage::noteOn(1, 48, 0.8f), 0);
    rig.midi.addEvent(juce::MidiMessage::noteOn(1, 55, 0.8f), 0);
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(10);
    juce::Image screen(juce::Image::ARGB, rig.editor->getWidth(), rig.editor->getHeight(), true, juce::SoftwareImageType());
    {
        juce::Graphics g(screen);
        rig.editor->paintEntireComponent(g, true);
    }
    {
        rig.run(3);
        for (const auto& r : rig.editor->tickFrame()) { // what one frame repaints, for the record
            // The deepest showing component under the area's centre (hit testing aside).
            juce::Component* at = rig.editor;
            for (bool deeper = true; deeper;) {
                deeper = false;
                for (int i = at->getNumChildComponents(); --i >= 0;) {
                    auto* child = at->getChildComponent(i);
                    if (child->isVisible() && child->getWidth() > 2 && rig.editor->getLocalArea(child, child->getLocalBounds()).contains(r.getCentre())) {
                        at = child;
                        deeper = true;
                        break;
                    }
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 20; ++i) {
                juce::Graphics g(screen);
                g.reduceClipRegion(r);
                rig.editor->paintEntireComponent(g, true);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20.0;
            std::printf("  repaints %s (%.3f ms)  %s\n", r.toString().toRawUTF8(), ms, zui::componentPath(*at).toRawUTF8());
        }
    }
    constexpr int kFrames = 120; // four seconds at 30 frames per second
    double interfaceSeconds = 0.0, tickSeconds = 0.0;
    std::size_t repainted = 0;
    for (int f = 0; f < kFrames; ++f) {
        rig.run(3); // 32 ms of audio between frames, as at 30 frames per second
        const auto t0 = std::chrono::steady_clock::now();
        const auto dirty = rig.editor->tickFrame();
        tickSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!dirty.empty()) {
            juce::RectangleList<int> region; // merged as a window merges the areas it is asked to repaint
            for (const auto& r : dirty)
                region.add(r);
            for (const auto& r : region)
                repainted += static_cast<std::size_t>(r.getWidth() * r.getHeight());
            juce::Graphics g(screen);
            g.reduceClipRegion(region);
            rig.editor->paintEntireComponent(g, true);
        }
        interfaceSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    const double percent = 100.0 * interfaceSeconds / (kFrames / 30.0);
    std::printf("interface while animating (SYNTH page, two notes, two LFOs, 30 fps, 100 %%): %.2f %% of one core (frame clock %.2f %%), %.0f px repainted per frame\n",
                percent, 100.0 * tickSeconds / (kFrames / 30.0), static_cast<double>(repainted) / kFrames);
    if (kBudgetsApply)
        CHECK(percent <= 2.0);
}

TEST_CASE("editor: workshop drawing gives feedback within 16 ms and new audio within 50 ms", "[editor][perf][table]") {
    Rig rig;
    auto& page = dynamic_cast<zui::TablePage&>(rig.editor->page(ZederiaEditor::Table));
    rig.editor->showPage(ZederiaEditor::Table);
    rig.frames(1);
    struct Case {
        const char* name;
        int frames;
        MorphMode morph;
    };
    for (const Case& k : {Case{"64 key frames, crossfade", 64, MorphMode::Crossfade}, Case{"256 key frames, none", 256, MorphMode::None},
                          Case{"16 key frames, spectral (256 positions)", 16, MorphMode::Spectral}}) {
        auto table = std::make_shared<Wavetable>(Wavetable::silent(k.frames));
        FormulaRequest request;
        request.expression = "sin(tau*t) * (1 - y) + saw(t) * y";
        std::string error;
        REQUIRE(applyFormula(*table, request, error));
        table->morph = k.morph;
        table->name = k.name;
        page.replaceTable(table, "Test table");
        for (int i = 0; i < 400 && rig.processor.tableBuildPending(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        juce::Image image(juce::Image::ARGB, 640, 420, true, juce::SoftwareImageType());
        double worstFeedback = 0.0, worstAudio = 0.0;
        for (int stroke = 0; stroke < 8; ++stroke) {
            page.setFrame(stroke % k.frames);
            const auto t0 = std::chrono::steady_clock::now();
            page.apply("", [stroke](Wavetable& t) {
                drawSegment(t.frames[static_cast<std::size_t>(stroke % t.size())], 0.1 + 0.05 * stroke, 0.3 + 0.05 * stroke, DrawShape::Line, 0.9f, -0.9f);
                return true;
            }, true);
            {
                juce::Graphics g(image);
                page.waveEditor().paintEntireComponent(g, true);
            }
            worstFeedback = std::max(worstFeedback, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            for (int i = 0; i < 400 && rig.processor.tableBuildPending(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            rig.run(1); // the engine takes the table at the start of its next block
            worstAudio = std::max(worstAudio, rig.processor.lastTableLatencyMs() + 512.0 / 48.0);
            page.finish("Draw");
        }
        std::printf("workshop, %s: feedback %.2f ms, audio %.2f ms (build plus one 512-sample block), worst of 8 strokes\n", k.name, worstFeedback, worstAudio);
        if (kBudgetsApply) {
            CHECK(worstFeedback <= 16.0);
            CHECK(worstAudio <= 50.0);
        }
    }
}

TEST_CASE("editor: Simple view hides advanced controls and keeps the layout clean", "[editor][simple]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(1);
    auto* coarse = visibleWithParam<zui::Knob>(*rig.editor, "osc1.coarse");
    REQUIRE(coarse != nullptr);
    rig.ctx().prefs.simpleView = true;
    rig.editor->applyLayout();
    rig.frames(1);
    CHECK_FALSE(coarse->isVisible());
    CHECK(visibleWithParam<zui::Knob>(*rig.editor, "osc1.position") != nullptr);
    std::vector<zui::Finding> findings;
    zui::renderSnapshot(*rig.editor, &findings);
    for (auto& f : zui::checkLayout(*rig.editor))
        findings.push_back(f);
    zui::writePng(zui::renderSnapshot(*rig.editor), snapshotFolder().getChildFile("synth_100_simple.png"));
    for (const auto& f : findings)
        UNSCOPED_INFO(f.kind << ": " << f.where << ": " << f.detail);
    CHECK(findings.empty());
    rig.ctx().prefs.simpleView = false;
    rig.editor->applyLayout();
}

TEST_CASE("editor: the library searches, sorts, edits in bulk, exports and imports packs without duplicates", "[editor][library]") {
    Rig rig(nullptr, true);
    rig.editor->showPage(ZederiaEditor::Library);
    auto& lib = dynamic_cast<zui::LibraryPage&>(rig.editor->page(ZederiaEditor::Library));
    lib.rescan();
    CHECK(lib.resultCount() == 6);
    lib.setQuery("type:pluck");
    CHECK(lib.resultCount() == 2);
    lib.setQuery("");
    lib.setSort(zed::PresetSort::Name, true);
    CHECK(str(lib.result(0).meta.name) == "Test Pluck");
    lib.setQuery("tag:bright");
    const int bright = lib.resultCount();
    lib.setQuery("");
    lib.selectRow(0);
    lib.selectRow(1, true);
    CHECK(lib.selectedRows().size() == 2);
    CHECK(lib.addTag("Shiny") == 2);
    lib.setQuery("tag:shiny");
    CHECK(lib.resultCount() == 2);
    lib.selectRow(0);
    CHECK(lib.setRating(4) == 1);
    CHECK(lib.setFavourite(true) == 1);
    lib.setQuery("fav rating:4");
    CHECK(lib.resultCount() == 1);
    lib.setQuery("");
    for (int i = 0; i < lib.resultCount(); ++i)
        lib.selectRow(i, i > 0);
    const juce::File pack = rig.userFolder.getChildFile("Test Pack.zpack");
    CHECK(lib.exportPack(pack, "Test Pack"));
    int skipped = 0;
    CHECK(lib.importPack(pack, skipped) == 0); // every sound is already there
    CHECK(skipped == 6);
    // A click on one preset loads it; Ctrl-click builds a selection for bulk edits and loads nothing.
    lib.setQuery("type:pad");
    REQUIRE(lib.resultCount() == 1);
    lib.table().deselectAllRows();
    lib.table().selectRowsBasedOnModifierKeys(0, {}, false);
    CHECK(rig.processor.session().currentPreset().getFullPathName() == str(lib.result(0).path));
    CHECK(str(rig.processor.meta().type) == "pad");
    lib.setQuery("");
    lib.table().deselectAllRows();
    lib.table().selectRowsBasedOnModifierKeys(0, {}, false);
    const juce::String first = str(lib.result(0).path);
    CHECK(rig.processor.session().currentPreset().getFullPathName() == first);
    lib.table().selectRowsBasedOnModifierKeys(1, juce::ModifierKeys(juce::ModifierKeys::commandModifier), false);
    CHECK(lib.selectedRows().size() == 2);
    CHECK(rig.processor.session().currentPreset().getFullPathName() == first);
    // Selections the page makes itself (bulk edits, finding duplicates) load nothing.
    lib.selectRow(2);
    CHECK(rig.processor.session().currentPreset().getFullPathName() == first);
    rig.run(4);
    juce::ignoreUnused(bright);
}

TEST_CASE("editor: the library opens with folded sections and uses one sidebar filter at a time", "[editor][library]") {
    Rig rig(nullptr, true);
    rig.editor->showPage(ZederiaEditor::Library);
    auto& lib = dynamic_cast<zui::LibraryPage&>(rig.editor->page(ZederiaEditor::Library));
    const auto checkFolded = [](zui::LibraryPage& page) -> juce::Component* {
        const juce::StringArray headings{"PRESETS", "USER PRESETS", "SHOW", "TYPE", "CHARACTER", "GENRE"};
        juce::Component* sidebar = nullptr;
        for (auto* button : all<zui::Button>(page))
            if (button->getComponentID() == "section/presets")
                sidebar = button->getParentComponent();
        REQUIRE(sidebar != nullptr);
        REQUIRE(sidebar->getNumChildComponents() == headings.size());
        int previousBottom = 0;
        for (int i = 0; i < headings.size(); ++i) {
            auto* found = dynamic_cast<zui::Button*>(sidebar->getChildComponent(i));
            REQUIRE(found != nullptr);
            CHECK(found->text() == headings[i]);
            CHECK(zui::visibleWithin(*found, &page));
            CHECK(found->getY() >= previousBottom);
            previousBottom = found->getBottom();
        }
        for (const char* key : {"presets", "user_presets", "show", "type", "tag", "genre"})
            CHECK(page.sectionCollapsed(key));
        CHECK(page.visibleFolders().isEmpty());
        return sidebar;
    };
    auto* sidebar = checkFolded(lib);
    const auto row = [sidebar](const char* id) -> zui::Button& {
        CAPTURE(id);
        auto* button = dynamic_cast<zui::Button*>(sidebar->findChildWithID(id));
        REQUIRE(button != nullptr);
        return *button;
    };
    const auto press = [&row](const char* id) {
        REQUIRE(row(id).keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    };
    press("section/presets");
    CHECK(sidebar->findChildWithID("folder/Tests") != nullptr);
    CHECK(sidebar->findChildWithID("folder/User Presets") == nullptr);
    press("section/user_presets");
    press("folder/User Presets");
    CHECK(lib.folder() == "User Presets");
    CHECK(lib.resultCount() == 0);

    press("section/type");
    press("filter/type:bass");
    CHECK(lib.folder().isEmpty());
    REQUIRE(lib.resultCount() == 1);
    CHECK(lib.result(0).meta.type == "bass");
    lib.selectRow(0);
    REQUIRE(lib.selectionToolsShown());
    press("filter/type:pluck");
    REQUIRE(lib.resultCount() == 2);
    CHECK(lib.selectedRows().empty());
    CHECK_FALSE(lib.selectionToolsShown());
    CHECK_FALSE(row("filter/type:bass").toggled());
    CHECK(row("filter/type:pluck").toggled());
    press("section/tag");
    press("filter/tag:mono");
    CHECK_FALSE(row("filter/type:pluck").toggled());
    CHECK(row("filter/tag:mono").toggled());
    REQUIRE(lib.resultCount() == 2);
    CHECK(lib.result(0).meta.type == "bass");
    CHECK(lib.result(1).meta.type == "lead");
    press("folder/Tests");
    CHECK_FALSE(row("filter/tag:mono").toggled());
    CHECK(lib.resultCount() == 6);
    lib.setQuery("tag:warm");

    rig.open();
    rig.editor->showPage(ZederiaEditor::Library);
    auto& reopened = dynamic_cast<zui::LibraryPage&>(rig.editor->page(ZederiaEditor::Library));
    checkFolded(reopened);
    CHECK(reopened.folder() == "Tests");
    REQUIRE(reopened.resultCount() == 1);
    zui::Button* clearScope = nullptr;
    for (auto* button : all<zui::Button>(reopened))
        if (button->getTitle() == "Clear sidebar selection: Tests" && zui::visibleWithin(*button, &reopened))
            clearScope = button;
    REQUIRE(clearScope != nullptr);
    REQUIRE(clearScope->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    CHECK(reopened.folder().isEmpty());
    REQUIRE(reopened.resultCount() == 1);
    CHECK(reopened.result(0).meta.type == "bass");
}

TEST_CASE("editor: the library shows the Presets folder as a tree, folds its sections and saves presets into folders", "[editor][library]") {
    Rig rig(nullptr, true);
    rig.editor->showPage(ZederiaEditor::Library);
    auto& lib = dynamic_cast<zui::LibraryPage&>(rig.editor->page(ZederiaEditor::Library));
    const juce::File presets = rig.userFolder.getChildFile("Presets");
    const juce::File user = presets.getChildFile("User Presets");
    // The folders of the free presets and User Presets are made, empty, when the library opens.
    for (const auto& name : zui::LibraryPage::defaultFolders())
        CHECK(presets.getChildFile(name).isDirectory());
    CHECK(lib.visibleFolders().isEmpty());
    lib.setSectionCollapsed("presets", false);
    const auto top = lib.visibleFolders();
    REQUIRE(top.size() >= 2);
    CHECK(top[0] == "808");
    CHECK(top[1] == "Bass");
    CHECK(top.contains("Tests"));
    CHECK_FALSE(top.contains("User Presets"));

    // Locations as typed in the save dialog.
    juce::String error;
    CHECK(zui::LibraryPage::resolveLocation(presets, "", error) == user);
    CHECK(zui::LibraryPage::resolveLocation(presets, "User Presets/My Phonk Pack", error) == user.getChildFile("My Phonk Pack"));
    CHECK(zui::LibraryPage::resolveLocation(presets, "User Presets\\phonk1\\", error) == user.getChildFile("phonk1"));
    CHECK(zui::LibraryPage::resolveLocation(presets, "../../Windows", error) == presets.getChildFile("Windows")); // never above Presets
    CHECK(zui::LibraryPage::resolveLocation(presets, presets.getChildFile("Bass").getFullPathName(), error) == presets.getChildFile("Bass"));
    const juce::File named = rig.userFolder.getChildFile("Zederia").getChildFile("Presets");
    CHECK(zui::LibraryPage::resolveLocation(named, "\\Zederia\\Presets\\user presets\\", error) == named.getChildFile("user presets"));
    CHECK(zui::LibraryPage::resolveLocation(named, "Presets/Bass", error) == named.getChildFile("Bass"));
    CHECK(error.isEmpty());
    CHECK(zui::LibraryPage::resolveLocation(presets, juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getFullPathName(), error) == juce::File());
    CHECK(error.isNotEmpty());

    // Saving into a new folder of User Presets: name, location, type and tags.
    CHECK(rig.editor->defaultSaveLocation() == "User Presets");
    juce::File saved;
    error.clear();
    REQUIRE(rig.editor->savePresetTo("Kick", "User Presets/My Phonk Pack", "808", "Hard, phonk", error, &saved));
    CHECK(saved == user.getChildFile("My Phonk Pack").getChildFile("Kick.zpreset"));
    CHECK(saved.existsAsFile());
    CHECK(rig.editor->defaultSaveLocation() == "User Presets/My Phonk Pack"); // saving again stays in its pack
    // A preset that is not one of yours is saved as your copy, in User Presets.
    juce::String loadError;
    REQUIRE(rig.processor.session().loadPresetFile(presets.getChildFile("Tests").getChildFile("pad.zpreset"), loadError));
    CHECK(rig.editor->defaultSaveLocation() == "User Presets");
    REQUIRE(rig.editor->savePresetTo("Snare", "User Presets/phonk1", "drum", "", error));
    // The saved folders have their own section, independent of the factory tree.
    lib.setSectionCollapsed("presets", true);
    lib.setSectionCollapsed("user_presets", false);
    CHECK_FALSE(lib.visibleFolders().contains("Tests"));
    CHECK(lib.visibleFolders().contains("User Presets/My Phonk Pack"));
    CHECK(lib.visibleFolders().contains("User Presets/phonk1"));
    lib.setFolder("User Presets/My Phonk Pack");
    REQUIRE(lib.resultCount() == 1);
    CHECK(str(lib.result(0).meta.name) == "Kick");
    CHECK(str(lib.result(0).meta.type) == "808");
    CHECK(lib.result(0).meta.tags == std::vector<std::string>{"hard", "phonk"});
    lib.setFolder("User Presets"); // a folder shows the presets in the folders inside it too
    CHECK(lib.resultCount() == 2);
    lib.setFolder("Tests");
    CHECK(lib.resultCount() == 6);
    lib.setFolder("Bass");
    CHECK(lib.resultCount() == 0);
    lib.setFolder("");
    CHECK(lib.resultCount() == 8);
    // Closing the user section hides its children. Choosing a folder reveals it again.
    lib.setSectionCollapsed("user_presets", true);
    CHECK_FALSE(lib.visibleFolders().contains("User Presets/phonk1"));
    lib.setFolder("User Presets/phonk1");
    CHECK(lib.visibleFolders().contains("User Presets/phonk1"));
    CHECK(lib.resultCount() == 1);
    // A saved project keeps its folder and results. Old expansion preferences must not
    // unfold the sidebar on a fresh open, including projects saved by earlier versions.
    nlohmann::json state;
    lib.saveState(state);
    state["folded"] = nlohmann::json::array();
    state["open_folders"] = {"User Presets"};
    lib.setFolder("");
    lib.loadState(state);
    for (const char* key : {"presets", "user_presets", "show", "type", "tag", "genre"})
        CHECK(lib.sectionCollapsed(key));
    CHECK(lib.folder() == "User Presets/phonk1");
    CHECK(lib.resultCount() == 1);
    CHECK(lib.visibleFolders().isEmpty());
    rig.frames(2);
    std::vector<zui::Finding> findings;
    zui::writePng(zui::renderSnapshot(*rig.editor, &findings), snapshotFolder().getChildFile("library_100_folders.png"));
    for (auto& f : zui::checkLayout(*rig.editor))
        findings.push_back(f);
    for (const auto& f : findings)
        UNSCOPED_INFO(f.kind << ": " << f.where << ": " << f.detail);
    CHECK(findings.empty());
}

TEST_CASE("editor: larger text sizes keep every page free of clipped or truncated text", "[editor][snapshot][text]") {
    Rig rig("arp_fx.zpreset", true);
    rig.frames(4);
    static const char* const names[] = {"synth", "fx", "mod", "table", "play", "library", "agent"};
    for (const int size : {120, 150}) {
        rig.ctx().prefs.textSize = size;
        rig.editor->applyTheme();
        CHECK(zui::textScale() == Catch::Approx(size / 100.0));
        for (int page = 0; page < 7; ++page) {
            rig.editor->showPage(page);
            showForCheck(rig);
            std::vector<zui::Finding> findings;
            const auto image = zui::renderSnapshot(*rig.editor, &findings);
            for (auto& f : zui::checkLayout(*rig.editor))
                findings.push_back(f);
            const juce::String file = juce::String(names[page]) + "_100_text" + juce::String(size);
            zui::writePng(image, snapshotFolder().getChildFile(file + ".png"));
            for (const auto& f : findings)
                UNSCOPED_INFO(file << ": " << f.kind << ": " << f.where << ": " << f.detail);
            CHECK(findings.empty());
        }
    }
    // The preference survives the settings file (this test process's own).
    rig.ctx().prefs.textSize = 115;
    rig.ctx().prefs.helpStrip = true;
    Prefs::get().save();
    Prefs other;
    REQUIRE(other.load(Prefs::file()));
    CHECK(other.textSize == 115);
    CHECK(other.helpStrip);
    // A new settings file starts at 120 % text with the help strip hidden; one written before those
    // defaults takes them once.
    CHECK(Prefs().textSize == 120);
    CHECK_FALSE(Prefs().helpStrip);
    const juce::File old = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_old_settings_" + juce::Uuid().toString() + ".json");
    REQUIRE(old.replaceWithText("{\"format\": \"zederia.settings\", \"text_size\": 130, \"help_strip\": true, \"tooltips\": false}"));
    Prefs before;
    REQUIRE(before.load(old));
    CHECK(before.textSize == 120);
    CHECK_FALSE(before.helpStrip);
    CHECK_FALSE(before.tooltips); // everything else is kept
    old.deleteFile();
    // The preferences step the text size with - and + (5 % a step, 90 to 150 %).
    rig.ctx().prefs.textSize = 120;
    rig.editor->showPrefs();
    std::vector<zui::Button*> buttons;
    collect(rig.editor->content(), buttons);
    zui::Button *larger = nullptr, *smaller = nullptr;
    for (auto* b : buttons) {
        if (b->getTitle() == "Larger text size") larger = b;
        if (b->getTitle() == "Smaller text size") smaller = b;
    }
    REQUIRE(larger != nullptr);
    REQUIRE(smaller != nullptr);
    larger->onClick();
    CHECK(rig.ctx().prefs.textSize == 125);
    CHECK(zui::textScale() == Catch::Approx(1.25));
    for (int i = 0; i < 10; ++i)
        larger->onClick();
    CHECK(rig.ctx().prefs.textSize == 150);
    for (int i = 0; i < 20; ++i)
        smaller->onClick();
    CHECK(rig.ctx().prefs.textSize == 90);
    rig.editor->closeOverlays();
    rig.ctx().prefs.textSize = 100;
    rig.ctx().prefs.helpStrip = false;
    Prefs::get().save();
    rig.editor->applyTheme();
    CHECK(zui::textScale() == 1.0f);
}

TEST_CASE("editor: the AGENT page's Connect panel offers a config and a prompt for each client", "[editor][agent]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Agent);
    auto& agent = dynamic_cast<zui::AgentPage&>(rig.editor->page(ZederiaEditor::Agent));
    CHECK_FALSE(agent.connectShown());
    agent.showConnect(true);
    REQUIRE(agent.connectShown());
    const juce::StringArray clients{"Claude Code", "Codex", "Antigravity"};
    const juce::StringArray configs{zui::AgentPage::claudeJson(), zui::AgentPage::codexToml(), zui::AgentPage::antigravityJson()};
    for (int i = 0; i < clients.size(); ++i) {
        CAPTURE(clients[i]);
        int tabCount = 0, promptCount = 0, configCount = 0;
        zui::Button* currentTab = nullptr;
        for (auto* button : all<zui::Button>(agent)) {
            if (!zui::visibleWithin(*button, &agent))
                continue;
            if (button->tab && clients.contains(button->text())) {
                ++tabCount;
                CHECK(button->toggled() == (button->text() == clients[i]));
                if (button->toggled())
                    currentTab = button;
            }
            if (button->text() == "Copy prompt") {
                ++promptCount;
                CHECK(button->getTitle().contains(clients[i]));
            }
            if (button->text() == "Copy config") {
                ++configCount;
                CHECK(button->getTitle().contains(clients[i]));
            }
        }
        CHECK(tabCount == clients.size());
        CHECK(promptCount == 1);
        CHECK(configCount == 1);
        int previews = 0;
        for (auto* editor : all<juce::TextEditor>(agent)) {
            if (zui::visibleWithin(*editor, &agent) && editor->isReadOnly()) {
                ++previews;
                CHECK(editor->getTitle().contains(clients[i]));
                CHECK(editor->isMultiLine());
                CHECK(editor->getText() == configs[i]);
            }
        }
        CHECK(previews == 1);
        REQUIRE(currentTab != nullptr);
        if (i + 1 < clients.size())
            REQUIRE(currentTab->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    }
    CHECK_FALSE(rig.processor.agentLink().enabled());
    rig.frames(2);
    std::vector<zui::Finding> findings;
    zui::writePng(zui::renderSnapshot(*rig.editor, &findings), snapshotFolder().getChildFile("overlay_connect_100.png"));
    for (auto& f : zui::checkLayout(*rig.editor))
        findings.push_back(f);
    for (const auto& f : findings)
        UNSCOPED_INFO(f.kind << ": " << f.where << ": " << f.detail);
    CHECK(findings.empty());
    {
        const juce::ScopedValueSetter<int> large(rig.ctx().prefs.textSize, 150);
        const juce::ScopedValueSetter<bool> help(rig.ctx().prefs.helpStrip, true);
        rig.editor->applyTheme();
        for (const auto& client : clients) {
            CAPTURE(client);
            for (auto* tab : all<zui::Button>(agent))
                if (tab->tab && tab->text() == client)
                    tab->onClick();
            auto issues = zui::checkLayout(*rig.editor);
            zui::renderSnapshot(*rig.editor, &issues);
            for (const auto& issue : issues)
                UNSCOPED_INFO(issue.kind << ": " << issue.where << ": " << issue.detail);
            CHECK(issues.empty());
        }
    }
    rig.editor->applyTheme();
    agent.showConnect(false);
    CHECK_FALSE(agent.connectShown());

    const juce::String path = zui::AgentPage::serverPath();
    const auto forward = [](const juce::File& f) { return f.getFullPathName().replaceCharacter('\\', '/'); };
    CHECK((path == forward(zui::AgentPage::installedServer()) || path == forward(zui::AgentPage::developmentServer())));
    CHECK(forward(zui::AgentPage::developmentServer()).endsWith("/AppData/Local/Programs/Zederia/zederia-mcp.exe"));
    CHECK(zui::AgentPage::installedServer().getFileName() == "zederia-mcp.exe");
    CHECK(zui::AgentPage::codexToml() == "[mcp_servers.zederia]\ncommand = \"" + path + "\"\nargs = [\"--stdio\"]\n");
    for (const auto& json : {zui::AgentPage::claudeJson(), zui::AgentPage::antigravityJson()}) {
        const auto parsed = nlohmann::json::parse(json.toStdString());
        CHECK(parsed["mcpServers"]["zederia"]["command"] == path.toStdString());
        CHECK(parsed["mcpServers"]["zederia"]["args"] == nlohmann::json::array({"--stdio"}));
    }
    // Each prompt names the server, says how to add it to that client and where the guide is.
    CHECK(zui::AgentPage::claudePrompt().contains(zui::AgentPage::claudeCommand()));
    CHECK(zui::AgentPage::codexPrompt().contains(zui::AgentPage::codexToml()));
    CHECK(zui::AgentPage::antigravityPrompt().contains("mcp_config.json"));
    for (const auto& prompt : {zui::AgentPage::claudePrompt(), zui::AgentPage::codexPrompt(), zui::AgentPage::antigravityPrompt()}) {
        CHECK(prompt.contains(path));
        CHECK(prompt.contains("--stdio"));
        CHECK(prompt.contains("zederia://guide/agent"));
    }
}

TEST_CASE("editor: the header's mark is the logo, plugin/brand/zederia-mark.svg, in its colour", "[editor][brand]") {
    const juce::File svg = juce::File(juce::String(ZED_CONTENT_DIR)).getSiblingFile("plugin").getChildFile("brand").getChildFile("zederia-mark.svg");
    const auto xml = juce::XmlDocument::parse(svg);
    REQUIRE(xml != nullptr);
    CHECK(xml->getStringAttribute("fill").equalsIgnoreCase("#efb96b"));
    CHECK(xml->getStringAttribute("fill-rule") == "evenodd");
    CHECK(xml->getStringAttribute("viewBox") == "9.8 10.3 110.4 110.4");
    const auto box = zui::brand::markViewBox();
    CHECK(box == juce::Rectangle<float>(9.8f, 10.3f, 110.4f, 110.4f));
    CHECK(zui::brand::markColour() == juce::Colour(0xffefb96bu));
    CHECK(zui::theme().lamp == zui::brand::markColour()); // the header draws the mark in the lamp colour
    // The SVG's paths, point by point (M, L and Z only), against the mark the header draws.
    std::vector<std::vector<juce::Point<float>>> fromSvg, drawn;
    for (auto* path = xml->getChildByName("path"); path != nullptr; path = path->getNextElementWithTagName("path")) {
        juce::StringArray tokens;
        tokens.addTokens(path->getStringAttribute("d"), " ,", "");
        tokens.removeEmptyStrings();
        for (int i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == "M")
                fromSvg.emplace_back();
            if ((tokens[i] == "M" || tokens[i] == "L") && i + 2 < tokens.size()) {
                fromSvg.back().push_back({tokens[i + 1].getFloatValue(), tokens[i + 2].getFloatValue()});
                i += 2;
            }
        }
    }
    const juce::Path mark = zui::brand::mark();
    for (juce::Path::Iterator it(mark); it.next();) {
        if (it.elementType == juce::Path::Iterator::startNewSubPath)
            drawn.emplace_back();
        if (it.elementType == juce::Path::Iterator::startNewSubPath || it.elementType == juce::Path::Iterator::lineTo)
            drawn.back().push_back({it.x1, it.y1});
    }
    REQUIRE(fromSvg.size() == 2);
    CHECK(drawn == fromSvg);
    CHECK_FALSE(zui::brand::mark().isUsingNonZeroWinding()); // even-odd, as the SVG
}

TEST_CASE("editor: the icons are made from the logo, and the gap between the slabs stays open", "[editor][brand]") {
    const juce::File brandDir = juce::File(juce::String(ZED_CONTENT_DIR)).getSiblingFile("plugin").getChildFile("brand");
    juce::String error;
    const zui::brand::Logo logo = zui::brand::loadLogo(brandDir.getChildFile("zederia-mark.svg"), error);
    INFO(error);
    REQUIRE(logo.drawable != nullptr);
    std::vector<juce::Image> images;
    for (const int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        INFO(size << " px");
        images.push_back(zui::brand::renderIcon(logo, size));
        const auto gap = zui::brand::measureGap(images.back());
        CHECK(gap.visible());
        CHECK(gap.gap <= 0.05f);
        // The colour is the logo's: the most covered pixels are #efb96b.
        const juce::Image::BitmapData data(images.back(), juce::Image::BitmapData::readOnly);
        juce::PixelARGB solid;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if (reinterpret_cast<const juce::PixelARGB*>(data.getPixelPointer(x, y))->getAlpha() == 255)
                    solid = *reinterpret_cast<const juce::PixelARGB*>(data.getPixelPointer(x, y));
        CHECK(juce::Colour(solid.getRed(), solid.getGreen(), solid.getBlue()) == zui::brand::markColour());
    }
    // A logo drawn without the gap is caught.
    juce::Image closed = images[0].createCopy();
    {
        juce::Graphics g(closed);
        g.setColour(zui::brand::markColour());
        g.fillRect(0, 10, 16, 3);
    }
    CHECK_FALSE(zui::brand::measureGap(closed).visible());
    // The icon file in the repository is what the logo makes now (scripts/make-icons.ps1 after a change).
    const juce::MemoryBlock made = zui::brand::iconFile(images);
    juce::MemoryBlock stored;
    REQUIRE(brandDir.getChildFile("zederia.ico").loadFileAsData(stored));
    CHECK(made == stored);
}

TEST_CASE("plug-in: a new instance starts with the default preset, and Init loads it", "[plugin][default]") {
    const juce::String before = juce::SystemStats::getEnvironmentVariable("ZEDERIA_FACTORY", {});
    const auto setFactory = [](const juce::String& dir) {
        _putenv(("ZEDERIA_FACTORY=" + dir).toRawUTF8());
    };
    const juce::File factory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_default_" + juce::Uuid().toString());
    const juce::File preset = factory.getChildFile("Presets").getChildFile("Default.zpreset");
    REQUIRE(factory.getChildFile("Presets").createDirectory().wasOk());
    REQUIRE(juce::File(juce::String(ZED_CONTENT_DIR)).getChildFile("presets").getChildFile("Default.zpreset").copyFileTo(preset));
    setFactory(factory.getFullPathName());
    {
        ZederiaProcessor p;
        CHECK(p.session().presetName() == "Default");
        CHECK(p.session().currentPreset() == preset);
        CHECK_FALSE(p.session().modified());
        CHECK_FALSE(p.session().undo()); // the history starts with it
        p.session().edit("Quieter", [](Patch& q) {
            q.set(idx("osc1.level"), -20.0);
            return true;
        });
        CHECK(p.session().modified());
        p.session().initPatch();
        CHECK(p.session().presetName() == "Default");
        CHECK_FALSE(p.session().modified());
        // A default of your own wins, for Init and for new instances.
        const juce::File mine = factory.getChildFile("Mine.zpreset");
        REQUIRE(mine.replaceWithText(juce::String::fromUTF8(readPatch("lead.zpreset").c_str())));
        Prefs::get().defaultPreset = mine.getFullPathName();
        p.session().initPatch();
        CHECK(p.session().presetName() == "Test Lead");
        ZederiaProcessor q;
        CHECK(q.session().presetName() == "Test Lead");
        Prefs::get().defaultPreset = {};
    }
    // Without a default preset: the built-in init sound.
    setFactory(before);
    factory.deleteRecursively();
    ZederiaProcessor r;
    CHECK(r.session().presetName() == "Init");
    CHECK(r.session().currentPreset() == juce::File());
}

TEST_CASE("editor: factory shapes, racks, effect presets and clips are offered where they are used", "[editor][content]") {
    Rig rig;
    const juce::File factory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_menus_" + juce::Uuid().toString());
    const auto write = [](const juce::File& file, const std::string& text) {
        REQUIRE(file.getParentDirectory().createDirectory().wasOk());
        REQUIRE(file.replaceWithText(juce::String::fromUTF8(text.c_str())));
    };
    write(factory.getChildFile("Shapes/Peak.zshape"), copyShape({{{0.0, -1.0, 0.0}, {0.5, 1.0, 0.0}, {1.0, -1.0, 0.0}}}, ShapeKind::Lfo));
    write(rig.userFolder.getChildFile("Shapes/Mine/Wave.zshape"), copyShape({{{0.0, 0.0, 0.5}, {0.25, 1.0, -0.5}, {1.0, 0.0, 0.0}}}, ShapeKind::Lfo));
    Patch withReverb;
    std::vector<Issue> issues;
    REQUIRE(loadRackPreset(R"({"format":"zederia.rack","version":1,"fx":{"master":[{"type":"reverb","mix":0.3}]}})", withReverb, issues));
    write(factory.getChildFile("Racks/Hall.zrack"), saveRackPreset(withReverb));
    write(factory.getChildFile("Racks/Effects/Room.zfx"), saveSlotPreset(withReverb, 2, 0));
    Clip clip;
    clip.notes = {{0.0, 1.0, 60, 0.8}, {1.0, 1.0, 64, 0.8}};
    const auto mid = midi::clipToFile(clip);
    REQUIRE(factory.getChildFile("Clips").createDirectory().wasOk());
    REQUIRE(factory.getChildFile("Clips/Riff.mid").replaceWithData(mid.data(), mid.size()));
    rig.processor.setContentFolders(factory.getFullPathName().toStdString(), rig.userFolder.getFullPathName().toStdString());

    const auto texts = [](const juce::PopupMenu& menu) {
        juce::StringArray out;
        for (juce::PopupMenu::MenuItemIterator it(menu, true); it.next();)
            if (it.getItem().text.isNotEmpty())
                out.add(it.getItem().text);
        return out;
    };
    const auto shapes = zui::contentSections(rig.ctx(), "Shapes");
    REQUIRE(shapes.size() == 2);
    CHECK(shapes[0].first == "Factory");
    CHECK(shapes[1].first == "Yours");
    // Every curve editor's Shapes menu: the basic shapes, then the files, folders as submenus.
    const auto editors = all<zui::CurveEditor>(*rig.editor);
    REQUIRE_FALSE(editors.empty());
    zui::CurveEditor* lfo = nullptr;
    zui::CurveEditor* envelope = nullptr;
    for (auto* e : editors) {
        if (e->config().bipolar && lfo == nullptr)
            lfo = e;
        if (e->config().anchoredStart && envelope == nullptr)
            envelope = e;
    }
    REQUIRE(lfo != nullptr);
    REQUIRE(envelope != nullptr);
    juce::PopupMenu menu;
    lfo->addShapeItems(menu);
    const auto items = texts(menu);
    CHECK(items.contains("Sine"));
    CHECK(items.contains("Peak"));
    CHECK(items.contains("Mine"));
    CHECK(items.contains("Wave"));
    CHECK(items.indexOf("Sine") < items.indexOf("Peak"));
    REQUIRE(lfo->applyShapeFile(factory.getChildFile("Shapes/Peak.zshape")));
    REQUIRE(lfo->shape().points.size() == 3);
    CHECK(lfo->shape().points[1].y == 1.0);
    // An envelope takes it from silence, its levels in 0..1.
    REQUIRE(envelope->applyShapeFile(factory.getChildFile("Shapes/Peak.zshape")));
    CHECK(envelope->shape().points.front().x == 0.0);
    CHECK(envelope->shape().points.front().y == 0.0);
    CHECK(envelope->shape().points[1].y == 1.0);
    CHECK_FALSE(lfo->applyShapeFile(factory.getChildFile("Racks/Hall.zrack"))); // not a shape
    // Racks, effect presets and clips: the factory's.
    juce::File picked;
    juce::PopupMenu racks, effects, clips;
    CHECK(zui::addContentFiles(racks, zui::contentSections(rig.ctx(), "Racks"), "*.zrack", [&](const juce::File& f) { picked = f; }) == 1);
    CHECK(texts(racks).contains("Hall"));
    CHECK(zui::addContentFiles(effects, zui::contentSections(rig.ctx(), "Racks/Effects"), "*.zfx", [&](const juce::File& f) { picked = f; }) == 1);
    CHECK(texts(effects).contains("Room"));
    CHECK(zui::addContentFiles(clips, zui::contentSections(rig.ctx(), "Clips"), "*.mid;*.midi", [&](const juce::File& f) { picked = f; }) == 1);
    CHECK(texts(clips).contains("Riff"));
    for (juce::PopupMenu::MenuItemIterator it(clips, true); it.next();)
        if (it.getItem().action)
            it.getItem().action();
    CHECK(picked == factory.getChildFile("Clips/Riff.mid"));
    factory.deleteRecursively();
}

TEST_CASE("editor: client setup names the server of the installed or the development copy", "[editor][agent]") {
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_server_path");
    root.deleteRecursively();
    const juce::File installed = root.getChildFile("Program Files/Zederia/zederia-mcp.exe");
    const juce::File development = root.getChildFile("Local/Programs/Zederia/zederia-mcp.exe");
    const juce::File devVst3 = root.getChildFile("Local/Programs/Common/VST3/Zederia.vst3/Contents/x86_64-win/Zederia.vst3");
    const juce::File devStandalone = root.getChildFile("Local/Programs/Zederia/Zederia.exe");
    const juce::File installedVst3 = root.getChildFile("Common Files/VST3/Zederia.vst3/Contents/x86_64-win/Zederia.vst3");
    const juce::File buildTree = root.getChildFile("build/release/Zederia.exe");
    const auto forward = [](const juce::File& f) { return f.getFullPathName().replaceCharacter('\\', '/'); };
    using zui::AgentPage;
    // Neither exists: the installed location, or the development one for a development copy.
    CHECK(AgentPage::chooseServer(installedVst3, installed, development) == forward(installed));
    CHECK(AgentPage::chooseServer(buildTree, installed, development) == forward(installed));
    CHECK(AgentPage::chooseServer(devVst3, installed, development) == forward(development));
    // Both exist: each copy names its own; a copy that is neither names the installed one.
    REQUIRE(installed.create().wasOk());
    REQUIRE(development.create().wasOk());
    CHECK(AgentPage::chooseServer(installedVst3, installed, development) == forward(installed));
    CHECK(AgentPage::chooseServer(devVst3, installed, development) == forward(development));
    CHECK(AgentPage::chooseServer(devStandalone, installed, development) == forward(development));
    CHECK(AgentPage::chooseServer(buildTree, installed, development) == forward(installed));
    // Only one exists: that one, whoever asks.
    REQUIRE(installed.deleteFile());
    CHECK(AgentPage::chooseServer(installedVst3, installed, development) == forward(development));
    REQUIRE(installed.create().wasOk());
    REQUIRE(development.deleteFile());
    CHECK(AgentPage::chooseServer(devVst3, installed, development) == forward(installed));
    root.deleteRecursively();
}

TEST_CASE("editor: the PLAY page edits steps and clips, and clips go to and from MIDI files", "[editor][play]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Play);
    rig.frames(1);
    auto& play = dynamic_cast<zui::PlayPage&>(rig.editor->page(ZederiaEditor::Play));
    auto& roll = play.pianoRoll();
    const auto area = roll.getLocalBounds().toFloat().reduced(4.0f).withTrimmedLeft(30.0f).withTrimmedBottom(46.0f);
    roll.mouseDoubleClick(mouse(roll, {area.getX() + 10.0f, area.getCentreY()}, {}, {-1, -1}, 2));
    CHECK(rig.processor.clip(0).notes.size() == 1);
    const juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_clip_" + juce::Uuid().toString() + ".mid");
    REQUIRE(play.exportClipFile(file));
    rig.processor.session().setValue(idx("clip.slot"), 3);
    juce::String error;
    REQUIRE(play.importClipFile(file, error));
    CHECK(rig.processor.clip(2).notes.size() == 1);
    file.deleteFile();
    rig.processor.session().undo();
    CHECK(rig.processor.clip(2).notes.empty());
}

TEST_CASE("editor: the keyboard plays from the mouse and the computer keys and never leaves a note hanging", "[editor][keys]") {
    Rig rig;
    rig.frames(2);
    auto& piano = rig.editor->keyboardLayer().piano();
    std::set<int> keysDown;
    bool buttonDown = false;
    piano.keyIsDown = [&](int code) { return keysDown.count(code) != 0; };
    piano.mouseIsDown = [&] { return buttonDown; };
    const auto centre = [&](int note) { return piano.keyBounds(note).getCentre().translated(0.0f, piano.isBlack(note) ? 0.0f : 20.0f); };
    const auto settle = [&] { rig.frames(2); };

    // A click plays the key under the pointer and the release ends it.
    buttonDown = true;
    piano.mouseDown(mouse(piano, centre(60)));
    settle();
    CHECK(rig.processor.uiNoteHeld(60));
    CHECK(rig.processor.noteDown(60));
    CHECK(rig.processor.activeVoices() > 0);
    piano.mouseUp(mouse(piano, centre(60)));
    buttonDown = false;
    settle();
    CHECK_FALSE(rig.processor.uiNoteHeld(60));
    CHECK_FALSE(rig.processor.noteDown(60));
    // Dragging across the keys moves the note: the old key ends as the new one starts.
    buttonDown = true;
    piano.mouseDown(mouse(piano, centre(60)));
    piano.mouseDrag(mouse(piano, centre(62), {juce::ModifierKeys::leftButtonModifier}, centre(60)));
    settle();
    CHECK_FALSE(rig.processor.uiNoteHeld(60));
    CHECK(rig.processor.uiNoteHeld(62));
    // Dragged off the keys: nothing plays.
    piano.mouseDrag(mouse(piano, {-30.0f, 10.0f}, {juce::ModifierKeys::leftButtonModifier}, centre(60)));
    CHECK(piano.heldCount() == 0);
    piano.mouseUp(mouse(piano, {-30.0f, 10.0f}));
    buttonDown = false;
    // A release the host kept from the window: the next frame sees no button down and ends the note.
    buttonDown = true;
    piano.mouseDown(mouse(piano, centre(64)));
    settle();
    REQUIRE(rig.processor.uiNoteHeld(64));
    buttonDown = false; // no mouseUp arrives
    settle();
    CHECK_FALSE(rig.processor.uiNoteHeld(64));
    CHECK_FALSE(rig.processor.noteDown(64));

    // Computer keys: Z plays C of the keys' octave, a held key repeats without a second note, and
    // the note ends when the key is no longer down.
    rig.ctx().prefs.computerKeys = true;
    piano.setOctave(3);
    const int c3 = 48;
    keysDown.insert('Z');
    CHECK(rig.editor->keyPressed(juce::KeyPress('z')));
    CHECK(rig.editor->keyPressed(juce::KeyPress('z'))); // auto-repeat
    settle();
    CHECK(rig.processor.uiNoteHeld(c3));
    CHECK(piano.heldCount() == 1);
    keysDown.erase('Z');
    rig.editor->keyStateChanged(false);
    settle();
    CHECK_FALSE(rig.processor.uiNoteHeld(c3));
    // Two keys at once, released while the window had no focus (no key event at all): the frame clock ends both.
    keysDown.insert('X');
    keysDown.insert('M');
    CHECK(rig.editor->keyPressed(juce::KeyPress('x')));
    CHECK(rig.editor->keyPressed(juce::KeyPress('m')));
    settle();
    CHECK(rig.processor.uiNoteHeld(c3 + 2));
    CHECK(rig.processor.uiNoteHeld(c3 + 11));
    keysDown.clear();
    settle();
    CHECK(piano.heldCount() == 0);
    CHECK_FALSE(rig.processor.noteDown(c3 + 2));
    CHECK_FALSE(rig.processor.noteDown(c3 + 11));
    // The two rows: the lower from Z with S D G H J as the black keys, the upper from Q an octave
    // higher with 2 3 5 6 7 9 0 and the punctuation keys; where the rows overlap two keys play one note.
    const std::pair<char, int> layout[] = {{'Z', 0},  {'S', 1},  {'X', 2},  {'D', 3},  {'C', 4},  {'V', 5},  {'G', 6},  {'B', 7},
                                           {'H', 8},  {'N', 9},  {'J', 10}, {'M', 11}, {',', 12}, {'L', 13}, {'.', 14}, {';', 15},
                                           {'/', 16}, {'Q', 12}, {'2', 13}, {'W', 14}, {'3', 15}, {'E', 16}, {'R', 17}, {'5', 18},
                                           {'T', 19}, {'6', 20}, {'Y', 21}, {'7', 22}, {'U', 23}, {'I', 24}, {'9', 25}, {'O', 26},
                                           {'0', 27}, {'P', 28}, {'[', 29}, {'=', 30}, {']', 31}};
    for (const auto& [key, semitone] : layout) {
        INFO("key " << key);
        CHECK(zui::PianoKeys::semitoneFor(key) == semitone);
        CHECK(zui::PianoKeys::lettersFor(semitone).contains(juce::String::charToString(static_cast<juce::juce_wchar>(key))));
    }
    CHECK(zui::PianoKeys::semitoneFor('q') == 12);   // lower case
    CHECK(zui::PianoKeys::semitoneFor('<') == 12);   // with Shift
    CHECK(zui::PianoKeys::semitoneFor('@') == 13);
    CHECK(zui::PianoKeys::lettersFor(12).size() == 2); // , and Q
    for (const char key : {'A', 'F', 'K', '1', '4', '8', '-'})
        CHECK(zui::PianoKeys::semitoneFor(key) == -1);
    keysDown.insert('Q');
    keysDown.insert(',');
    CHECK(rig.editor->keyPressed(juce::KeyPress('q')));
    CHECK(rig.editor->keyPressed(juce::KeyPress(',')));
    settle();
    CHECK(rig.processor.uiNoteHeld(c3 + 12));
    keysDown.erase('Q'); // the other key still holds the note
    settle();
    CHECK(rig.processor.uiNoteHeld(c3 + 12));
    keysDown.erase(',');
    settle();
    CHECK_FALSE(rig.processor.uiNoteHeld(c3 + 12));
    keysDown.insert('5');
    CHECK(rig.editor->keyPressed(juce::KeyPress('%', juce::ModifierKeys::shiftModifier, '%'))); // Shift held: still the 5 key
    settle();
    CHECK(rig.processor.uiNoteHeld(c3 + 18));
    keysDown.clear();
    settle();
    CHECK(piano.heldCount() == 0);
    // Playing never moves the octave or the velocity: every row key leaves them as they were.
    const int velocity = piano.velocity();
    for (const auto& [key, semitone] : layout) {
        keysDown.insert(key);
        CHECK(rig.editor->keyPressed(juce::KeyPress(key)));
        keysDown.erase(key);
        rig.editor->keyStateChanged(false);
    }
    CHECK(piano.octave() == 3);
    CHECK(piano.velocity() == velocity);
    CHECK(piano.heldCount() == 0);
    // Left and Right move the octave, Down and Up the velocity; shortcuts with Ctrl are not notes.
    CHECK(rig.editor->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    CHECK(piano.octave() == 4);
    CHECK(rig.editor->keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));
    CHECK(piano.octave() == 3);
    CHECK(rig.editor->keyPressed(juce::KeyPress(juce::KeyPress::downKey)));
    CHECK(piano.velocity() < velocity);
    CHECK(rig.editor->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    CHECK(piano.velocity() == velocity);
    CHECK_FALSE(piano.computerKeyDown(juce::KeyPress('z', juce::ModifierKeys::ctrlModifier, 0)));
    CHECK_FALSE(piano.computerKeyDown(juce::KeyPress(juce::KeyPress::numberPad5, {}, '5'))); // number-pad keys are not notes
    CHECK(piano.heldCount() == 0);
    // With the computer keys switched off, letters do nothing.
    rig.ctx().prefs.computerKeys = false;
    CHECK_FALSE(rig.editor->keyPressed(juce::KeyPress('z')));
    rig.ctx().prefs.computerKeys = true;

    // The processor's own guard: a release that cannot be queued (the queue filled while audio was
    // stopped) still ends the note, because the screen no longer holds the key.
    rig.processor.keyboardState().noteOn(1, 70, 0.8f);
    for (int i = 0; i < 400; ++i)
        rig.processor.pushUiEvent(zed::MidiEvent::pitchBend(0, 0.0f));
    rig.processor.keyboardState().noteOff(1, 70, 0.0f);
    rig.run(4);
    CHECK_FALSE(rig.processor.noteDown(70));
    // Letting go of everything at once (the panic button does this, then sends all notes and sounds off).
    buttonDown = true;
    piano.mouseDown(mouse(piano, centre(72)));
    settle();
    REQUIRE(rig.processor.noteDown(72));
    rig.editor->keyboardLayer().piano().releaseAll();
    settle();
    CHECK_FALSE(rig.processor.noteDown(72));
    buttonDown = false;
    // Closing the editor while a key is held ends the note.
    keysDown.insert('C');
    CHECK(rig.editor->keyPressed(juce::KeyPress('c')));
    settle();
    REQUIRE(rig.processor.uiNoteHeld(c3 + 4));
    rig.open();
    rig.run(2);
    CHECK_FALSE(rig.processor.uiNoteHeld(c3 + 4));
    CHECK_FALSE(rig.processor.noteDown(c3 + 4));
}

TEST_CASE("editor: the pitch wheel springs back and the pitch with it, whatever happens to the events", "[editor][keys][wheel]") {
    Rig rig;
    rig.frames(2);
    auto* wheel = &rig.editor->keyboardLayer().pitchWheel();
    bool buttonDown = false;
    wheel->mouseIsDown = [&] { return buttonDown; };
    const auto top = juce::Point<float>(static_cast<float>(wheel->getWidth()) * 0.5f, 2.0f);
    const auto middle = juce::Point<float>(static_cast<float>(wheel->getWidth()) * 0.5f, static_cast<float>(wheel->getHeight()) * 0.4f);
    const juce::ModifierKeys left{juce::ModifierKeys::leftButtonModifier};
    const int bendSource = zui::Context::sourceIndex("pitchbend"), modSource = zui::Context::sourceIndex("modwheel");
    REQUIRE(bendSource >= 0);
    REQUIRE(modSource >= 0);
    const auto bend = [&] { return rig.processor.engine().sourceValue(bendSource); };
    // Rising zero crossings of the left channel over a number of blocks: the pitch that plays.
    const auto crossings = [&](int blocks) {
        int n = 0;
        float last = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            rig.run(1);
            const float* l = rig.buffer.getReadPointer(0);
            for (int i = 0; i < rig.buffer.getNumSamples(); ++i) {
                if (last < 0.0f && l[i] >= 0.0f)
                    ++n;
                last = l[i];
            }
        }
        return n;
    };
    rig.processor.keyboardState().noteOn(1, 57, 0.8f);
    rig.run(8);
    const int still = crossings(24);

    // Up and let go: the pitch bends and comes back with the wheel.
    buttonDown = true;
    wheel->mouseDown(mouse(*wheel, top, left));
    rig.run(8);
    CHECK(bend() > 0.9f);
    CHECK(crossings(24) > still);
    wheel->mouseUp(mouse(*wheel, top));
    buttonDown = false;
    CHECK(wheel->value() == 0.0f);
    rig.run(8);
    CHECK(std::abs(bend()) < 0.01f);
    CHECK(std::abs(crossings(24) - still) <= 1);

    // Moved a long way while the host did not call the audio thread (more moves than any queue
    // holds), then let go: the centre still arrives when the audio runs again.
    buttonDown = true;
    wheel->mouseDown(mouse(*wheel, top, left));
    for (int i = 0; i < 600; ++i)
        wheel->mouseDrag(mouse(*wheel, i % 2 == 0 ? middle : top, left, top));
    wheel->mouseUp(mouse(*wheel, top));
    buttonDown = false;
    rig.run(8);
    CHECK(std::abs(bend()) < 0.01f);
    // The same with the keyboard's event queue full as well.
    buttonDown = true;
    wheel->mouseDown(mouse(*wheel, top, left));
    for (int i = 0; i < 400; ++i)
        rig.processor.pushUiEvent(zed::MidiEvent::controller(0, 20, 0.0f));
    wheel->mouseUp(mouse(*wheel, top));
    buttonDown = false;
    rig.run(8);
    CHECK(std::abs(bend()) < 0.01f);

    // A release the host kept from the window: the next frame sees no button down and the wheel springs back.
    buttonDown = true;
    wheel->mouseDown(mouse(*wheel, top, left));
    rig.run(4);
    REQUIRE(bend() > 0.9f);
    buttonDown = false; // no mouseUp arrives
    rig.frames(2);
    CHECK_FALSE(wheel->held());
    CHECK(wheel->value() == 0.0f);
    rig.run(8);
    CHECK(std::abs(bend()) < 0.01f);

    // Closing the editor while the wheel is held: the pitch goes back to the centre.
    buttonDown = true;
    wheel->mouseDown(mouse(*wheel, top, left));
    rig.run(4);
    REQUIRE(bend() > 0.9f);
    rig.open();
    rig.run(8);
    CHECK(std::abs(bend()) < 0.01f);
    CHECK(rig.editor->keyboardLayer().pitchWheel().value() == 0.0f);

    // The modulation wheel stays where it is let go, and a reopened editor shows it there.
    auto& mod = rig.editor->keyboardLayer().modWheel();
    mod.mouseIsDown = [&] { return buttonDown; };
    buttonDown = true;
    mod.mouseDown(mouse(mod, top, left));
    mod.mouseUp(mouse(mod, top));
    buttonDown = false;
    rig.frames(2);
    rig.run(8);
    CHECK(rig.processor.engine().sourceValue(modSource) > 0.9f);
    rig.open();
    CHECK(rig.editor->keyboardLayer().modWheel().value() > 0.9f);
    rig.processor.keyboardState().noteOff(1, 57, 0.0f);
}
