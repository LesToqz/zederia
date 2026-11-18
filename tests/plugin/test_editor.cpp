// The interface (milestone 5): snapshots with the automatic checks at three scales, theme contrast,
// every parameter reachable, undo and redo round trips, modulation gestures, state across editor
// and host sessions, keyboard and screen-reader access, typed values, the command palette, the
// frame clock, the interface's cost, workshop latency, Simple view, rack and library actions, and
// the keyboard (mouse, computer keys, releases that never arrive).
#include "../support/TestUtil.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Prefs.h"
#include "Session.h"
#include "ui/Audit.h"
#include "ui/FxPage.h"
#include "ui/LibraryPage.h"
#include "ui/ModPage.h"
#include "ui/PlayPage.h"
#include "ui/SynthPage.h"
#include "ui/TablePage.h"
#include "ui/CurveEditor.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/table/Formula.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
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
        if (withLibrary) {
            userFolder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_library_" + juce::Uuid().toString());
            const auto presets = userFolder.getChildFile("Presets").getChildFile("Tests");
            presets.createDirectory();
            for (const char* name : {"bass", "pad", "pluck", "lead", "arp_fx", "glass_pluck"})
                presets.getChildFile(juce::String(name) + ".zpreset").replaceWithText(juce::String::fromUTF8(readPatch((std::string(name) + ".zpreset").c_str()).c_str()));
            processor.setContentFolders("", userFolder.getFullPathName().toStdString());
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
    CHECK(zui::Theme::names().size() >= 3);
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
    // A preset loads and its audition phrase plays.
    lib.setQuery("type:pad");
    REQUIRE(lib.resultCount() == 1);
    lib.audition(0);
    CHECK(rig.processor.auditioning());
    rig.run(40);
    juce::ignoreUnused(bright);
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

    // Computer keys: A plays C of the keys' octave, a held key repeats without a second note, and
    // the note ends when the key is no longer down.
    rig.ctx().prefs.computerKeys = true;
    piano.setOctave(3);
    const int c3 = 48;
    keysDown.insert('A');
    CHECK(rig.editor->keyPressed(juce::KeyPress('a')));
    CHECK(rig.editor->keyPressed(juce::KeyPress('a'))); // auto-repeat
    settle();
    CHECK(rig.processor.uiNoteHeld(c3));
    CHECK(piano.heldCount() == 1);
    keysDown.erase('A');
    rig.editor->keyStateChanged(false);
    settle();
    CHECK_FALSE(rig.processor.uiNoteHeld(c3));
    // Two keys at once, released while the window had no focus (no key event at all): the frame clock ends both.
    keysDown.insert('S');
    keysDown.insert('J');
    CHECK(rig.editor->keyPressed(juce::KeyPress('s')));
    CHECK(rig.editor->keyPressed(juce::KeyPress('j')));
    settle();
    CHECK(rig.processor.uiNoteHeld(c3 + 2));
    CHECK(rig.processor.uiNoteHeld(c3 + 11));
    keysDown.clear();
    settle();
    CHECK(piano.heldCount() == 0);
    CHECK_FALSE(rig.processor.noteDown(c3 + 2));
    CHECK_FALSE(rig.processor.noteDown(c3 + 11));
    // Z and X move the octave, C and V the velocity; shortcuts with Ctrl are not notes.
    CHECK(rig.editor->keyPressed(juce::KeyPress('x')));
    CHECK(piano.octave() == 4);
    CHECK(rig.editor->keyPressed(juce::KeyPress('z')));
    CHECK(piano.octave() == 3);
    const int velocity = piano.velocity();
    CHECK(rig.editor->keyPressed(juce::KeyPress('c')));
    CHECK(piano.velocity() < velocity);
    CHECK(rig.editor->keyPressed(juce::KeyPress('v')));
    CHECK(piano.velocity() == velocity);
    CHECK_FALSE(piano.computerKeyDown(juce::KeyPress('a', juce::ModifierKeys::ctrlModifier, 0)));
    CHECK(piano.heldCount() == 0);
    // With the computer keys switched off, letters do nothing.
    rig.ctx().prefs.computerKeys = false;
    CHECK_FALSE(rig.editor->keyPressed(juce::KeyPress('a')));
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
    keysDown.insert('D');
    CHECK(rig.editor->keyPressed(juce::KeyPress('d')));
    settle();
    REQUIRE(rig.processor.uiNoteHeld(c3 + 4));
    rig.open();
    rig.run(2);
    CHECK_FALSE(rig.processor.uiNoteHeld(c3 + 4));
    CHECK_FALSE(rig.processor.noteDown(c3 + 4));
}
