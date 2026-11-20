// The interface's states (the interface review): a module that is off looks off in grayscale and
// follows host automation; the wavetable view's own 3D / 2D control; keys that travel the same for
// the mouse, the computer keys and MIDI; page tabs that screen readers announce as selected and
// that the arrow keys move; the dock's open graph apart from the routing selection; and each page's
// empty and selected states (FX, MOD, TABLE, PLAY, LIBRARY, AGENT). Snapshots of the states go to
// build/<preset>/snapshots/states.
#include "../support/TestUtil.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Prefs.h"
#include "Session.h"
#include "AgentLink.h"
#include "ui/AgentPage.h"
#include "ui/Audit.h"
#include "ui/Dock.h"
#include "ui/FxPage.h"
#include "ui/LibraryPage.h"
#include "ui/ModPage.h"
#include "ui/PlayPage.h"
#include "ui/SynthPage.h"
#include "ui/TablePage.h"
#include "zederia/link/Link.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>

using namespace zed;
using zedtest::idx;

namespace {

const bool kPrefsRedirected = [] {
    Prefs::useFile(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_states_settings_" + juce::Uuid().toString() + ".json"));
    return true;
}();

juce::File statesFolder() { return juce::File(juce::String(ZED_SNAPSHOT_DIR)).getChildFile("states"); }

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
        userFolder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("zederia_test_states_" + juce::Uuid().toString());
        userFolder.createDirectory();
        if (withLibrary) {
            const auto presets = userFolder.getChildFile("Presets").getChildFile("Tests");
            presets.createDirectory();
            for (const char* name : {"bass", "pad", "pluck"})
                presets.getChildFile(juce::String(name) + ".zpreset")
                    .replaceWithText(juce::String::fromUTF8(zedtest::readText(zedtest::dataPath(std::string("patches/") + name + ".zpreset")).c_str()));
        }
        processor.setContentFolders("", userFolder.getFullPathName().toStdString());
        if (patch != nullptr) {
            REQUIRE(processor.loadPatchJson(zedtest::readText(zedtest::dataPath(std::string("patches/") + patch))).ok());
            processor.session().resetHistory("Opened");
        }
        base.reset(processor.createEditor());
        editor = dynamic_cast<ZederiaEditor*>(base.get());
        REQUIRE(editor != nullptr);
    }
    ~Rig() {
        base.reset();
        if (userFolder.isDirectory())
            userFolder.deleteRecursively();
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            buffer.clear();
            processor.processBlock(buffer, midi);
            midi.clear();
            editor->tickFrame();
        }
    }
    zui::Context& ctx() { return editor->context(); }
    // A snapshot with the automatic checks: none may find anything.
    void snapshot(const juce::String& name) {
        frames(2);
        std::vector<zui::Finding> findings;
        const auto image = zui::renderSnapshot(*editor, &findings);
        for (auto& f : zui::checkLayout(*editor))
            findings.push_back(f);
        zui::writePng(image, statesFolder().getChildFile(name + ".png"));
        for (const auto& f : findings)
            UNSCOPED_INFO(name << ": " << f.kind << ": " << f.where << ": " << f.detail);
        CHECK(findings.empty());
    }
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

zui::Card* cardWithPower(juce::Component& root, const char* id) {
    for (auto* c : all<zui::Card>(root))
        if (c->powerSwitch != nullptr && c->powerSwitch->param() == idx(id))
            return c;
    return nullptr;
}

zui::WavetableView* tableView(juce::Component& root, int osc) {
    for (auto* v : all<zui::WavetableView>(root))
        if (v->osc() == osc)
            return v;
    return nullptr;
}

juce::MouseEvent mouse(juce::Component& c, juce::Point<float> at, juce::ModifierKeys mods = {}) {
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, at, now, 1, false);
}

// The luminance of a pixel (Rec. 709 weights), 0..1: what a grayscale view shows.
double luminance(juce::Colour c) { return (0.2126 * c.getRed() + 0.7152 * c.getGreen() + 0.0722 * c.getBlue()) / 255.0; }

// The brightest pixel of an area of an image: a lit trace shows as a high peak.
double peakLuminance(const juce::Image& image, juce::Rectangle<int> area) {
    double peak = 0.0;
    area = area.getIntersection(image.getBounds());
    for (int y = area.getY(); y < area.getBottom(); ++y)
        for (int x = area.getX(); x < area.getRight(); ++x)
            peak = std::max(peak, luminance(image.getPixelAt(x, y)));
    return peak;
}

} // namespace

TEST_CASE("editor: a module that is off reads as off without colour, stays editable and follows host automation", "[editor][power]") {
    Rig rig; // Init: OSC 1 on, OSC 2 and 3 off, both filters off
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(3);
    auto* view1 = tableView(*rig.editor, 0);
    auto* view2 = tableView(*rig.editor, 1);
    REQUIRE(view1 != nullptr);
    REQUIRE(view2 != nullptr);
    CHECK(view1->drawnActive());
    CHECK_FALSE(view2->drawnActive());
    auto* card2 = cardWithPower(*rig.editor, "osc2.on");
    REQUIRE(card2 != nullptr);
    CHECK(card2->bypassed());
    CHECK_FALSE(cardWithPower(*rig.editor, "osc1.on")->bypassed());
    auto* level2 = visibleWithParam<zui::Knob>(*rig.editor, "osc2.level");
    auto* level1 = visibleWithParam<zui::Knob>(*rig.editor, "osc1.level");
    REQUIRE(level2 != nullptr);
    REQUIRE(level1 != nullptr);
    CHECK(level2->bypassed());
    CHECK_FALSE(level1->bypassed());
    auto* cutoff = visibleWithParam<zui::Knob>(*rig.editor, "filter1.cutoff");
    REQUIRE(cutoff != nullptr);
    CHECK(cutoff->bypassed());

    // In grayscale the oscillator that sounds is the one with the bright trace.
    auto image = zui::renderSnapshot(*rig.editor);
    const auto areaOf = [&](juce::Component& c) { return rig.editor->getLocalArea(&c, c.getLocalBounds()).reduced(8, 22); };
    const double lit = peakLuminance(image, areaOf(*view1)), off = peakLuminance(image, areaOf(*view2));
    INFO("peak luminance, on " << lit << ", off " << off);
    CHECK(lit > 0.9);
    CHECK(off < 0.65);

    // A bypassed module stays editable.
    const double before = rig.processor.plainValue(idx("osc2.level"));
    CHECK(level2->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    CHECK(rig.processor.plainValue(idx("osc2.level")) > before);

    // Host automation switches OSC 2 on: the display, the header and the controls follow with the
    // table and position unchanged.
    auto* host = rig.processor.hostParameter(idx("osc2.on"));
    REQUIRE(host != nullptr);
    host->setValueNotifyingHost(1.0f);
    rig.frames(3);
    CHECK(view2->drawnActive());
    CHECK_FALSE(card2->bypassed());
    CHECK_FALSE(level2->bypassed());
    image = zui::renderSnapshot(*rig.editor);
    CHECK(peakLuminance(image, areaOf(*view2)) > 0.9);
    host->setValueNotifyingHost(0.0f);
    rig.frames(3);
    CHECK_FALSE(view2->drawnActive());
    CHECK(level2->bypassed());
}

TEST_CASE("editor: the wavetable view switches 3D and 2D with a control of its own and keeps the Table view through Classic", "[editor][wavetable]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(2);
    auto* view = tableView(*rig.editor, 0);
    REQUIRE(view != nullptr);
    auto& button = view->viewButton();
    CHECK(button.isVisible());
    CHECK(button.getWantsKeyboardFocus());
    CHECK(button.getTitle().contains("3D"));
    CHECK(view->showing3d());
    // A click on the plot changes nothing; the control does, and the keyboard reaches it.
    view->mouseDown(mouse(*view, view->getLocalBounds().getCentre().toFloat()));
    CHECK(view->showing3d());
    CHECK(button.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    CHECK_FALSE(view->show3d);
    CHECK(button.text() == "2D");
    CHECK(button.keyPressed(juce::KeyPress(juce::KeyPress::spaceKey)));
    CHECK(view->show3d);
    // Table -> Classic -> Table: Classic draws its cycle and hides the control; a click on the
    // Classic plot does nothing; Table comes back in the view it had.
    rig.processor.session().setValue(idx("osc1.engine"), 1.0);
    rig.frames(2);
    CHECK_FALSE(view->showing3d());
    CHECK_FALSE(button.isVisible());
    view->mouseDown(mouse(*view, view->getLocalBounds().getCentre().toFloat()));
    rig.processor.session().setValue(idx("osc1.engine"), 0.0);
    rig.frames(2);
    CHECK(view->showing3d());
    CHECK(button.isVisible());
    // The same round trip from 2D.
    view->setShow3d(false);
    rig.processor.session().setValue(idx("osc1.engine"), 1.0);
    rig.frames(2);
    rig.processor.session().setValue(idx("osc1.engine"), 0.0);
    rig.frames(2);
    CHECK_FALSE(view->show3d);
    CHECK(button.text() == "2D");
    // Position and Level keep their places in Table and Classic.
    auto* level = visibleWithParam<zui::Knob>(*rig.editor, "osc1.level");
    REQUIRE(level != nullptr);
    const auto tableLevel = level->getBoundsInParent();
    const auto tablePosition = visibleWithParam<zui::Knob>(*rig.editor, "osc1.position")->getBoundsInParent();
    rig.processor.session().setValue(idx("osc1.engine"), 1.0);
    rig.frames(2);
    CHECK(level->getBoundsInParent() == tableLevel);
    auto* shape = visibleWithParam<zui::Knob>(*rig.editor, "osc1.classic.shape");
    REQUIRE(shape != nullptr);
    CHECK(shape->getBoundsInParent() == tablePosition);
    rig.snapshot("synth_osc1_classic");
    rig.processor.session().setValue(idx("osc1.engine"), 0.0);
    rig.frames(2);
}

TEST_CASE("editor: keys go down the same for the mouse, the computer keys and MIDI, and come back up", "[editor][keys][travel]") {
    Rig rig;
    rig.frames(2);
    auto& piano = rig.editor->keyboardLayer().piano();
    std::set<int> keysDown;
    bool buttonDown = false;
    piano.keyIsDown = [&](int code) { return keysDown.count(code) != 0; };
    piano.mouseIsDown = [&] { return buttonDown; };
    const auto centre = [&](int note) { return piano.keyBounds(note).getCentre().translated(0.0f, piano.isBlack(note) ? 0.0f : 20.0f); };
    // A black key repaints its shadow too: the area reaches below the key.
    CHECK(piano.paintArea(61).getBottom() > piano.keyBounds(61).getBottom() + 4.0f);
    CHECK(piano.paintArea(61).contains(piano.keyBounds(61).getSmallestIntegerContainer()));
    // The mouse.
    buttonDown = true;
    piano.mouseDown(mouse(piano, centre(60)));
    rig.frames(2);
    CHECK(piano.shownDown(60));
    // The computer keys: X plays D of the keys' octave; held with the mouse's C a neighbouring
    // white key; S plays the black key between them.
    rig.ctx().prefs.computerKeys = true;
    piano.setOctave(4); // Z is C4 (60)
    keysDown.insert('S');
    CHECK(rig.editor->keyPressed(juce::KeyPress('s')));
    rig.frames(2);
    CHECK(piano.shownDown(61));
    CHECK(piano.shownDown(60)); // a white key and the black key beside it, held together
    // MIDI from the host.
    rig.midi.addEvent(juce::MidiMessage::noteOn(1, 67, 0.8f), 0);
    rig.frames(2);
    CHECK(piano.shownDown(67));
    std::vector<zui::Finding> none;
    zui::writePng(zui::renderSnapshot(*rig.editor, &none), statesFolder().getChildFile("keys_held_mouse_computer_midi.png"));
    // Each comes back up: the mouse dragged off the keys, the computer key let go, the MIDI note off.
    piano.mouseDrag(mouse(piano, {-30.0f, 10.0f}, juce::ModifierKeys::leftButtonModifier));
    piano.mouseUp(mouse(piano, {-30.0f, 10.0f}));
    buttonDown = false;
    keysDown.clear();
    rig.editor->keyStateChanged(false);
    rig.midi.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
    rig.frames(3);
    CHECK_FALSE(piano.shownDown(60));
    CHECK_FALSE(piano.shownDown(61));
    CHECK_FALSE(piano.shownDown(67));
    CHECK(piano.heldCount() == 0);
}

TEST_CASE("editor: page tabs are announced as selected, move with the arrow keys, and look apart from a module's tabs", "[editor][tabs][access]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(1);
    zui::TabStrip* pages = nullptr;
    for (auto* t : all<zui::TabStrip>(*rig.editor))
        if (t->large)
            pages = t;
    REQUIRE(pages != nullptr);
    const auto state = [&](int i) { return pages->tab(i).createAccessibilityHandler()->getCurrentState(); };
    REQUIRE(pages->tab(0).createAccessibilityHandler()->getRole() == juce::AccessibilityRole::radioButton);
    CHECK(state(0).isChecked());
    CHECK_FALSE(state(1).isChecked());
    // Right on the SYNTH tab (where Tab brings the keyboard) shows FX, Left on FX goes back; Left on
    // the first tab stays there.
    CHECK(pages->tab(0).keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    CHECK(rig.editor->currentPage() == ZederiaEditor::Fx);
    CHECK(state(1).isChecked());
    CHECK_FALSE(state(0).isChecked());
    CHECK(pages->tab(1).keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));
    CHECK(rig.editor->currentPage() == ZederiaEditor::Synth);
    CHECK(pages->tab(0).keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));
    CHECK(rig.editor->currentPage() == ZederiaEditor::Synth);
    // An arrow with Ctrl is not the tabs' (the editor's shortcuts and the computer keys keep it).
    CHECK_FALSE(pages->tab(0).keyPressed(juce::KeyPress(juce::KeyPress::rightKey, juce::ModifierKeys::ctrlModifier, 0)));
    // Enter on a tab shows its page (keyboard), and the selection follows pages shown from code.
    CHECK(pages->tab(1).keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    CHECK(rig.editor->currentPage() == ZederiaEditor::Fx);
    CHECK(state(1).isChecked());
    rig.editor->showPage(ZederiaEditor::Synth);
    CHECK(state(0).isChecked());
    CHECK_FALSE(state(1).isChecked());
    // A module's tabs are a set of their own, also announced.
    zui::TabStrip* local = nullptr;
    for (auto* t : all<zui::TabStrip>(rig.editor->page(ZederiaEditor::Synth)))
        if (!t->large && zui::visibleWithin(*t, rig.editor))
            local = t;
    REQUIRE(local != nullptr);
    CHECK(local->tab(local->selected()).createAccessibilityHandler()->getCurrentState().isChecked());
}

TEST_CASE("editor: the dock's open graph keeps its name and tab while the routing selection changes", "[editor][dock][modulation]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Synth);
    rig.frames(1);
    std::map<juce::String, zui::DragChip*> chips;
    for (auto* c : all<zui::DragChip>(*rig.editor))
        chips[c->source()] = c;
    REQUIRE(chips.count("lfo2") == 1);
    // Open LFO 2: its chip is the open tab, the card says LFO 2, and it is chosen for routing.
    chips["lfo2"]->mouseUp(mouse(*chips["lfo2"], {5.0f, 5.0f}));
    rig.frames(1);
    auto& dock = rig.editor->dock();
    CHECK(dock.selected() == "lfo2");
    CHECK(chips["lfo2"]->isOpen());
    CHECK(rig.ctx().selectedSource == "lfo2");
    CHECK(dock.shownTitle() == "LFO 2");
    // A second click clears the routing selection; the graph stays LFO 2.
    chips["lfo2"]->mouseUp(mouse(*chips["lfo2"], {5.0f, 5.0f}));
    CHECK(rig.ctx().selectedSource.isEmpty());
    CHECK(dock.selected() == "lfo2");
    CHECK(chips["lfo2"]->isOpen());
    // Another source dropped on a control: that route is the one being edited, the graph is still LFO 2.
    auto* cutoff = visibleWithParam<zui::Knob>(*rig.editor, "filter1.cutoff");
    REQUIRE(cutoff != nullptr);
    cutoff->itemDropped(juce::DragAndDropTarget::SourceDetails("mod:env2", nullptr, {}));
    rig.frames(2);
    CHECK(rig.ctx().selectedSource == "env2");
    CHECK(dock.selected() == "lfo2");
    CHECK(chips["lfo2"]->isOpen());
    CHECK_FALSE(chips["env2"]->isOpen());
    REQUIRE(cutoff->arcs().size() == 1);
    CHECK(cutoff->arcs()[0].selected);
    rig.snapshot("dock_lfo2_open_env2_routing");
}

TEST_CASE("editor: the FX rack offers its first empty position, and a lane's list and strips agree", "[editor][fx]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Fx);
    auto& fx = dynamic_cast<zui::FxPage&>(rig.editor->page(ZederiaEditor::Fx));
    fx.select(1, 0); // Lane B, empty
    rig.frames(2);
    std::vector<zui::Button*> adds;
    for (auto* b : all<zui::Button>(fx))
        if (b->getTitle().startsWith("Add an effect at") && zui::visibleWithin(*b, rig.editor))
            adds.push_back(b);
    REQUIRE(adds.size() == 1);
    CHECK(adds[0]->getTitle() == "Add an effect at B1 of Lane B");
    rig.snapshot("fx_lane_b_empty");
    // Add two effects to Lane B, reorder and bypass one: the chain's row and the rack's strip say the same.
    REQUIRE(fx.insertEffect(1, 0, 0));
    REQUIRE(fx.insertEffect(1, 1, 1));
    rig.frames(2);
    REQUIRE(fx.slotCount(1) == 2);
    const int first = fx.slotType(1, 0), second = fx.slotType(1, 1);
    REQUIRE(fx.moveEffect(1, 1, 1, 0));
    rig.frames(2);
    CHECK(fx.slotType(1, 0) == second);
    CHECK(fx.slotType(1, 1) == first);
    rig.processor.session().setValue(Registry::get().fxSlotOn(1, 0), 0.0);
    rig.frames(2);
    adds.clear();
    for (auto* b : all<zui::Button>(fx))
        if (b->getTitle().startsWith("Add an effect at") && zui::visibleWithin(*b, rig.editor))
            adds.push_back(b);
    REQUIRE(adds.size() == 1);
    CHECK(adds[0]->getTitle() == "Add an effect at B3 of Lane B");
    // Both the strip's power and its controls read off; the row and the strip use the one switch.
    std::vector<zui::Toggle*> powers;
    for (auto* t : all<zui::Toggle>(fx))
        if (t->param() == Registry::get().fxSlotOn(1, 0) && zui::visibleWithin(*t, rig.editor))
            powers.push_back(t);
    REQUIRE(powers.size() == 1);
    CHECK(powers[0]->value() < 0.5);
    for (auto* k : all<zui::Knob>(fx))
        if (k->param() == Registry::get().fxSlotMix(1, 0))
            CHECK(k->bypassed());
    rig.snapshot("fx_lane_b_two_effects_first_bypassed");
    rig.processor.session().undo();
    rig.frames(2);
    CHECK(rig.processor.plainValue(Registry::get().fxSlotOn(1, 0)) >= 0.5);
}

TEST_CASE("editor: the MOD page's actions follow the routes, the search and the selection", "[editor][mod]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Mod);
    auto& mod = dynamic_cast<zui::ModPage&>(rig.editor->page(ZederiaEditor::Mod));
    rig.frames(2);
    // No routes: Add route is the one action; nothing to select, replace, mute or remove; no inspector.
    CHECK(mod.addButton().accentStyle);
    CHECK(mod.addButton().isEnabled());
    for (auto* b : {&mod.replaceButton(), &mod.muteButton(), &mod.removeButton(), &mod.selectAllButton()})
        CHECK_FALSE(b->isEnabled());
    CHECK_FALSE(mod.inspectorShown());
    rig.snapshot("mod_no_routes");
    rig.processor.session().addRoute("lfo1", idx("filter1.cutoff"), 0.4);
    rig.processor.session().addRoute("env2", idx("osc1.position"), 0.3);
    rig.frames(2);
    CHECK(mod.selectAllButton().isEnabled());
    CHECK_FALSE(mod.removeButton().isEnabled());
    // A search that matches nothing: nothing to select, Clear search beside the message.
    mod.setSearch("reverb");
    rig.frames(1);
    CHECK(mod.shownRoutes().empty());
    CHECK_FALSE(mod.selectAllButton().isEnabled());
    CHECK(mod.clearSearchButton().isVisible());
    rig.snapshot("mod_search_no_match");
    mod.clearSearchButton().onClick();
    CHECK(mod.shownRoutes().size() == 2);
    CHECK_FALSE(mod.clearSearchButton().isVisible());
    // One checked row: the bulk actions apply to it.
    mod.setSelected(0, true);
    CHECK(mod.removeButton().isEnabled());
    CHECK(mod.muteButton().text() == "Mute selected");
    // Several: the actions say how many; Select all checks the rows on show, again clears them.
    mod.setSelected(1, true);
    CHECK(mod.muteButton().text() == "Mute 2 selected");
    CHECK(mod.selectAllButton().text() == "Clear selection");
    mod.selectAllButton().onClick();
    CHECK(mod.selectedRoutes().empty());
    CHECK_FALSE(mod.removeButton().isEnabled());
    mod.setSearch("lfo");
    mod.selectAllButton().onClick();
    CHECK(mod.selectedRoutes() == std::vector<int>{0}); // only the routes the search shows
    mod.setSearch("");
    // The inspector shows for a focused route only.
    mod.focusRoute(1);
    CHECK(mod.inspectorShown());
    rig.snapshot("mod_two_routes_focused");
    mod.muteButton().onClick();
    CHECK(rig.processor.routes()[0].mute);
}

TEST_CASE("editor: the workshop keeps the frame and the drawing shape in view through a stroke and its undo", "[editor][table]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Table);
    auto& table = dynamic_cast<zui::TablePage&>(rig.editor->page(ZederiaEditor::Table));
    rig.frames(1);
    // The common shapes are named for screen readers; the rarer ones sit in one labelled selector.
    const auto buttons = table.toolButtons();
    REQUIRE(buttons.size() == 10);
    for (auto* b : buttons)
        CHECK(b->getTitle().isNotEmpty());
    CHECK(buttons[3]->getTitle() == "Draw: Ramp up");
    CHECK(buttons[7]->getTitle() == "More shapes");
    CHECK(buttons[8]->latch); // Snap
    CHECK(buttons[8]->toggled());
    CHECK(buttons[9]->text() == "Grid 16");
    table.setFrame(1);
    buttons[3]->onClick(); // Ramp up
    CHECK(table.toolName() == "Ramp up");
    CHECK(buttons[3]->toggled());
    CHECK_FALSE(buttons[0]->toggled());
    const zed::Frame before = table.table().frames[1];
    table.apply("Draw on frame 2", [](zed::Wavetable& t) {
        zed::drawSegment(t.frames[1], 0.0, 0.5, zed::DrawShape::RampUp, -1.0f, 1.0f);
        return true;
    });
    rig.frames(1);
    CHECK(table.table().frames[1] != before);
    CHECK(table.frame() == 1);
    rig.snapshot("table_frame2_ramp");
    rig.processor.session().undo();
    rig.frames(2);
    CHECK(table.table().frames[1] == before);
    CHECK(table.frame() == 1);
    CHECK(table.toolName() == "Ramp up");
    CHECK(buttons[3]->toggled());
    // A shape from the selector seats the selector and names it.
    table.setTool("Arch up");
    CHECK(buttons[7]->toggled());
    CHECK(buttons[7]->text() == "Arch up");
    CHECK_FALSE(buttons[3]->toggled());
}

TEST_CASE("editor: the step lane shows the stored pattern neutral, its bypass, and the step that sounds", "[editor][play]") {
    Rig rig("arp_fx.zpreset"); // its arpeggiator is on
    rig.editor->showPage(ZederiaEditor::Play);
    auto& play = dynamic_cast<zui::PlayPage&>(rig.editor->page(ZederiaEditor::Play));
    rig.frames(2);
    // Stopped: no key held, no step lit.
    CHECK(rig.processor.engine().arpStep() < 0);
    rig.snapshot("play_arp_stopped");
    // Running: a held chord plays the lane; the step that sounded is the one marked.
    rig.midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
    rig.midi.addEvent(juce::MidiMessage::noteOn(1, 64, 0.8f), 0);
    rig.frames(12);
    const int playing = rig.processor.engine().arpStep();
    CHECK(playing >= 0);
    CHECK(playing < static_cast<int>(rig.processor.plainValue(idx("arp.steps"))));
    rig.snapshot("play_arp_running");
    // Bypassed: the header says so, nothing is marked, the pattern can still be edited.
    rig.processor.session().setValue(idx("arp.on"), 0.0);
    rig.frames(4);
    CHECK(rig.processor.engine().arpStep() < 0);
    auto* arp = cardWithPower(play, "arp.on");
    REQUIRE(arp != nullptr);
    CHECK(arp->bypassed());
    auto& lane = play.stepLane();
    lane.grabKeyboardFocus();
    zed::ArpPattern before;
    rig.processor.readDocument([&](const zed::Patch& p) { before = p.arpPatterns[0]; });
    lane.keyPressed(juce::KeyPress(juce::KeyPress::downKey)); // lowers the focused step's velocity
    zed::ArpPattern after;
    rig.processor.readDocument([&](const zed::Patch& p) { after = p.arpPatterns[0]; });
    CHECK(after.step(0).velocity < before.step(0).velocity);
    rig.snapshot("play_arp_bypassed");
    rig.midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    rig.midi.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
    rig.frames(2);
}

TEST_CASE("editor: the library says what is empty, with the action beside it, and shows its tools for a selection", "[editor][library]") {
    {
        Rig empty(nullptr, false); // no presets at all
        empty.editor->showPage(ZederiaEditor::Library);
        auto& lib = dynamic_cast<zui::LibraryPage&>(empty.editor->page(ZederiaEditor::Library));
        lib.rescan();
        CHECK(lib.emptyState() == zui::LibraryPage::Empty::Library);
        REQUIRE(lib.emptyAction() != nullptr);
        CHECK(lib.emptyAction()->text() == "Import pack");
        CHECK(lib.emptyMessage().startsWith("No presets yet"));
        CHECK_FALSE(lib.selectionToolsShown());
        empty.snapshot("library_empty");
        // The message is in front of the list: its pixels are not the list's background.
        empty.frames(1);
    }
    Rig rig(nullptr, true);
    rig.editor->showPage(ZederiaEditor::Library);
    auto& lib = dynamic_cast<zui::LibraryPage&>(rig.editor->page(ZederiaEditor::Library));
    lib.rescan();
    REQUIRE(lib.resultCount() == 3);
    CHECK(lib.emptyState() == zui::LibraryPage::Empty::None);
    CHECK(lib.emptyAction() == nullptr);
    CHECK_FALSE(lib.selectionToolsShown());
    // An empty folder.
    lib.setFolder("Bass");
    CHECK(lib.emptyState() == zui::LibraryPage::Empty::Folder);
    CHECK(lib.emptyAction()->text() == "Show all presets");
    rig.snapshot("library_empty_folder");
    lib.emptyAction()->onClick();
    CHECK(lib.folder().isEmpty());
    CHECK(lib.resultCount() == 3);
    // A search with no match.
    lib.setQuery("type:choir");
    CHECK(lib.emptyState() == zui::LibraryPage::Empty::Search);
    CHECK(lib.emptyAction()->text() == "Clear search");
    rig.snapshot("library_no_match");
    lib.emptyAction()->onClick();
    CHECK(lib.resultCount() == 3);
    // A selection shows the tools that edit it.
    lib.selectRow(0);
    CHECK(lib.selectionToolsShown());
    rig.snapshot("library_selected");
}

TEST_CASE("editor: the AGENT page's permissions are checkboxes for every agent, and the link switch is not the setup action", "[editor][agent]") {
    Rig rig;
    rig.editor->showPage(ZederiaEditor::Agent);
    rig.frames(2);
    auto& agent = rig.editor->page(ZederiaEditor::Agent);
    std::vector<zui::Button*> checkboxes, switches;
    zui::Button* setup = nullptr;
    for (auto* b : all<zui::Button>(agent)) {
        if (!zui::visibleWithin(*b, rig.editor))
            continue;
        if (b->checkbox)
            checkboxes.push_back(b);
        if (b->switchStyle)
            switches.push_back(b);
        if (b->text() == "Client setup")
            setup = b;
    }
    // Six saved permissions, readable with the link off: each a checkbox announcing its state.
    CHECK_FALSE(rig.processor.agentLink().enabled());
    REQUIRE(checkboxes.size() == 6);
    const auto saved = zed::link::loadPermissions();
    const bool values[] = {saved.readPatch, saved.editPatch, saved.writeZederia, saved.writeElsewhere, saved.audition, saved.readSamples};
    for (std::size_t i = 0; i < 6; ++i) {
        const auto handler = checkboxes[i]->createAccessibilityHandler();
        CHECK(handler->getRole() == juce::AccessibilityRole::toggleButton);
        CHECK(handler->getCurrentState().isCheckable());
        CHECK(handler->getCurrentState().isChecked() == values[i]);
    }
    // The link: a switch with its state beside it. Setup: an action that only opens the panel.
    REQUIRE(switches.size() == 1);
    CHECK(switches[0]->text() == "This instance: off");
    REQUIRE(setup != nullptr);
    CHECK_FALSE(setup->switchStyle);
    CHECK_FALSE(setup->accentStyle);
    setup->onClick();
    CHECK(dynamic_cast<zui::AgentPage&>(agent).connectShown());
    CHECK_FALSE(rig.processor.agentLink().enabled()); // opening setup switched nothing on
    dynamic_cast<zui::AgentPage&>(agent).showConnect(false);
    rig.snapshot("agent_link_off");
}
