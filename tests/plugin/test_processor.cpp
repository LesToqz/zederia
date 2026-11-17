#include "../support/TestUtil.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Session.h"
#include "zederia/midi/MidiFile.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/util/Math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <set>

using namespace zed;
using Catch::Approx;
using zedtest::idx;

namespace {

struct Host {
    ZederiaProcessor processor;
    juce::AudioBuffer<float> buffer{2, 512};
    juce::MidiBuffer midi;
    std::vector<float> left;

    Host() {
        processor.setPlayConfigDetails(0, 2, 48000.0, 512);
        processor.prepareToPlay(48000.0, 512);
        midi.ensureSize(4096); // a host owns this buffer; it never grows inside the callback
    }
    void run(int blocks = 1) {
        for (int b = 0; b < blocks; ++b) {
            buffer.clear();
            processor.processBlock(buffer, midi);
            left.insert(left.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + buffer.getNumSamples());
            midi.clear();
        }
    }
    float peak(std::size_t from = 0) const { return zedtest::peakOf(left, from, left.size()); }
};

} // namespace

TEST_CASE("plug-in: exposes the automatable parameters under stable ids", "[plugin]") {
    const auto& reg = Registry::get();
    ZederiaProcessor processor;
    int automatable = 0;
    std::set<juce::String> ids;
    for (const auto& def : reg.params()) {
        auto* parameter = processor.hostParameter(def.index);
        INFO(def.id);
        if (!def.automatable) {
            CHECK(parameter == nullptr);
            continue;
        }
        ++automatable;
        REQUIRE(parameter != nullptr);
        CHECK(parameter->getParameterID() == juce::String(def.id));
        CHECK(ids.insert(parameter->getParameterID()).second);
        // The host's numeric id is the framework's hash of the string id; the registry predicts it.
        CHECK((static_cast<std::uint32_t>(parameter->getParameterID().hashCode()) & 0x7fffffffu) == def.hostId);
        CHECK(parameter->isAutomatable());
        CHECK(parameter->getDefaultValue() == Approx(def.toNormalized(def.def)).margin(1e-6));
        CHECK(parameter->getValue() == Approx(parameter->getDefaultValue()).margin(1e-6));
        CHECK(parameter->isDiscrete() == def.stepped());
        CHECK(parameter->getName(100).isNotEmpty());
    }
    CHECK(processor.getParameters().size() == automatable);
    CHECK(automatable > 500);
}

TEST_CASE("plug-in: parameter values, text and host notifications", "[plugin]") {
    ZederiaProcessor processor;
    auto* cutoff = processor.hostParameter(idx("filter1.cutoff"));
    REQUIRE(cutoff != nullptr);
    cutoff->setValueNotifyingHost(1.0f);
    CHECK(processor.plainValue(idx("filter1.cutoff")) == Approx(22000.0));
    CHECK(cutoff->getText(1.0f, 64) == "22000 Hz");
    CHECK(cutoff->getLabel() == "Hz");
    const float half = cutoff->getValueForText("1000");
    CHECK(processor.effectiveDef(idx("filter1.cutoff")).fromNormalized(half) == Approx(1000.0).epsilon(1e-4));

    auto* type = processor.hostParameter(idx("filter1.type"));
    CHECK(type->getNumSteps() == static_cast<int>(Registry::get().find("filter1.type")->choices.size()));
    type->setValueNotifyingHost(type->getValueForText("ladder_lp24"));
    CHECK(type->getText(type->getValue(), 64) == "ladder_lp24");
    CHECK(processor.hostParameter(idx("osc1.on"))->isBoolean());
    CHECK(processor.hostParameter(idx("osc1.position"))->getValueForText("25 %") == Approx(0.25f));
    CHECK(processor.hostParameter(idx("osc1.level"))->getText(0.0f, 64) == "-inf dB");

    // Values set from the interface reach the engine, automatable or not.
    processor.setNormalizedFromUi(idx("global.polyphony"), 1.0f);
    CHECK(processor.plainValue(idx("global.polyphony")) == 64.0);
    processor.setNormalizedFromUi(idx("osc1.pan"), 0.75f);
    CHECK(processor.plainValue(idx("osc1.pan")) == Approx(0.5));
}

TEST_CASE("plug-in: state survives a save and restore", "[plugin][state]") {
    const std::string example = zedtest::readText(zedtest::dataPath("patches/glass_pluck.zpreset"));
    juce::MemoryBlock state;
    std::string before;
    {
        ZederiaProcessor first;
        const LoadResult loaded = first.loadPatchJson(example);
        for (const auto& issue : loaded.issues)
            UNSCOPED_INFO(issue.path << ": " << issue.message);
        REQUIRE(loaded.ok());
        first.hostParameter(idx("osc1.pan"))->setValueNotifyingHost(0.25f);
        first.setNormalizedFromUi(idx("global.polyphony"), 0.0f);
        before = first.patchJson();
        first.getStateInformation(state);
    }
    REQUIRE(state.getSize() > 12);
    CHECK(state.getSize() < before.size()); // compressed
    CHECK(std::string(static_cast<const char*>(state.getData()), 4) == "ZEDS");

    ZederiaProcessor second;
    second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(second.patchJson() == before);
    CHECK(second.plainValue(idx("osc1.pan")) == Approx(-0.5));
    CHECK(second.plainValue(idx("global.polyphony")) == 1.0);
    CHECK(second.plainValue(idx("filter1.cutoff")) == 1800.0);
    const LoadResult reread = loadPatch(second.patchJson());
    REQUIRE(reread.ok());
    CHECK(reread.patch.routes.size() == 2);
    CHECK(reread.patch.meta.name == "Glass Pluck");
    CHECK(reread.patch.tables[0] == "factory:Basics/Saw Morph");

    // Saving what was restored gives the same bytes again.
    juce::MemoryBlock again;
    second.getStateInformation(again);
    CHECK(again == state);
}

TEST_CASE("plug-in: damaged state is ignored, never a crash", "[plugin][state][fuzz]") {
    ZederiaProcessor processor;
    processor.hostParameter(idx("osc1.pan"))->setValueNotifyingHost(1.0f);
    const std::string before = processor.patchJson();
    juce::MemoryBlock good;
    processor.getStateInformation(good);

    processor.setStateInformation(nullptr, 0);
    processor.setStateInformation("ZEDS", 4);
    processor.setStateInformation("not a state at all, just text", 29);
    Rng rng(77);
    for (int i = 0; i < 300; ++i) {
        juce::MemoryBlock bad(good);
        auto* bytes = static_cast<std::uint8_t*>(bad.getData());
        const int edits = 1 + static_cast<int>(rng.next() % 4);
        for (int e = 0; e < edits; ++e)
            bytes[12 + rng.next() % (bad.getSize() - 12)] = static_cast<std::uint8_t>(rng.next()); // damage the body
        const int size = rng.next() % 3 == 0 ? static_cast<int>(rng.next() % bad.getSize()) : static_cast<int>(bad.getSize());
        processor.setStateInformation(bad.getData(), size);
        // Either the damage was caught and nothing changed, or it decoded to a valid patch.
        REQUIRE(loadPatch(processor.patchJson()).ok());
    }
    processor.setStateInformation(good.getData(), static_cast<int>(good.getSize()));
    CHECK(processor.patchJson() == before);

    // A patch with errors is refused as a whole.
    const LoadResult bad = processor.loadPatchJson("{\"format\":\"zederia.patch\",\"version\":1,\"osc1\":{\"pan\":\"left\",\"level_db\":-20}}");
    CHECK_FALSE(bad.ok());
    CHECK(processor.patchJson() == before);
}

TEST_CASE("plug-in: the on-screen keyboard plays notes", "[plugin]") {
    Host host;
    host.run(2);
    CHECK(host.peak() == 0.0f);
    host.processor.keyboardState().noteOn(1, 60, 1.0f); // what a click on the keyboard does
    host.run(8);
    CHECK(host.processor.activeVoices() == 1);
    CHECK(host.peak(1024) > 0.05f);
    CHECK(host.processor.meterPeak(0) > 0.05f);
    const double f = zedtest::measureFrequency(host.left, 2048, host.left.size(), 48000.0);
    CHECK(zedtest::cents(f, noteToHz(60)) == Approx(0.0).margin(0.5));

    host.processor.keyboardState().noteOff(1, 60, 0.0f);
    host.run(12); // the default release is 50 ms
    CHECK(host.processor.activeVoices() == 0);
    const std::size_t end = host.left.size();
    host.run(2);
    CHECK(host.peak(end) == 0.0f);
}

TEST_CASE("plug-in: host MIDI is sample-accurate and the callback is real-time safe", "[plugin][rt]") {
    rt::resetViolations();
    Host host;
    host.run(1); // settle the first parameter sync
    host.midi.addEvent(juce::MidiMessage::noteOn(1, 69, 1.0f), 200);
    host.midi.addEvent(juce::MidiMessage::pitchWheel(1, 12000), 300);
    host.midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 301);
    host.run(1);
    CHECK(zedtest::peakOf(host.left, 512, 512 + 201) == 0.0f);
    CHECK(zedtest::peakOf(host.left, 512 + 201, 1024) > 0.0f);

    // Automation and notes from several directions at once.
    Rng rng(3);
    for (int block = 0; block < 300; ++block) {
        for (int i = 0; i < 6; ++i) {
            const int note = 30 + static_cast<int>(rng.next() % 70);
            if (rng.next() % 2)
                host.midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<float>(rng.uniform())), static_cast<int>(rng.next() % 512));
            else
                host.midi.addEvent(juce::MidiMessage::noteOff(1, note), static_cast<int>(rng.next() % 512));
        }
        if (block % 7 == 0)
            for (auto* parameter : host.processor.getParameters())
                parameter->setValue(static_cast<float>(rng.uniform())); // as a host does on the audio thread
        host.run(1);
    }
    INFO("last violation: " << rt::lastViolation());
    CHECK(rt::violationCount() == 0);
    bool finite = true;
    for (const float s : host.left)
        finite = finite && std::isfinite(s);
    CHECK(finite);
}

TEST_CASE("plug-in: reports latency, tail and layout", "[plugin]") {
    ZederiaProcessor processor;
    CHECK(processor.getLatencySamples() == 0);
    CHECK(processor.getTailLengthSeconds() >= 0.05);
    processor.setNormalizedFromUi(idx("env1.release"), 1.0f);
    CHECK(processor.getTailLengthSeconds() >= 30.0);
    CHECK(processor.acceptsMidi());
    CHECK(processor.getTotalNumOutputChannels() == 2);
    CHECK(processor.getTotalNumInputChannels() == 0);
    juce::AudioProcessor::BusesLayout mono;
    mono.outputBuses.add(juce::AudioChannelSet::mono());
    CHECK_FALSE(processor.isBusesLayoutSupported(mono));
}

TEST_CASE("plug-in: a rack slot's generic parameters take the loaded effect's names and ranges", "[plugin]") {
    const auto& reg = Registry::get();
    ZederiaProcessor processor;
    auto* p10 = processor.hostParameter(idx("fx.a1.p10"));
    REQUIRE(p10 != nullptr);
    CHECK(p10->getName(100) == "FX A1 Param 10");
    REQUIRE(processor.loadPatchJson("{\"format\":\"zederia.patch\",\"version\":1,"
                                    "\"fx\":{\"a\":[{\"type\":\"delay\",\"feedback\":0.8,\"time_l_ms\":250}]}}").ok());
    CHECK(p10->getName(100) == "FX A1 Feedback");
    CHECK(p10->getValue() == Approx(0.8f));
    auto* time = processor.hostParameter(idx("fx.a1.p3"));
    CHECK(time->getName(100) == "FX A1 Time L");
    CHECK(time->getLabel() == "ms");
    CHECK(time->getText(time->getValue(), 64) == "250 ms");
    time->setValueNotifyingHost(1.0f);
    CHECK(processor.plainValue(idx("fx.a1.p3")) == Approx(4000.0));
    const LoadResult saved = loadPatch(processor.patchJson());
    REQUIRE(saved.ok());
    CHECK(saved.patch.fxSlotType(0, 0) == reg.effectIndex("delay"));
    CHECK(saved.patch.get(idx("fx.a1.p3")) == Approx(4000.0));
}

TEST_CASE("plug-in: the editor opens at its default size and follows the processor", "[plugin][editor]") {
    Host host;
    std::unique_ptr<juce::AudioProcessorEditor> base(host.processor.createEditor());
    auto* editor = dynamic_cast<ZederiaEditor*>(base.get());
    REQUIRE(editor != nullptr);
    CHECK(editor->getWidth() == 1320);
    CHECK(editor->getHeight() == 820);
    editor->setSize(700, 450); // the constrainer is the host's business; the content scales with any size
    editor->setScalePercent(150);
    CHECK(editor->getWidth() == 1980);

    // The keyboard component drives the same state the processor listens to.
    host.processor.keyboardState().noteOn(1, 64, 0.9f);
    host.run(4);
    CHECK(host.peak() > 0.0f);
    base.reset(); // closing the editor releases held keys
    host.run(12);
    CHECK(host.processor.activeVoices() == 0);
}

// --- Milestone 2: sources, tables and textures ---------------------------------------------------

#include "../support/Analysis.h"
#include "zederia/content/Recipe.h"

#include <filesystem>

namespace {

// The starter content built into a temporary folder, once per test process. The tests run in
// parallel, one process each, so every process has a folder of its own and removes it at the end.
struct FactoryFolder {
    std::string path;
    FactoryFolder() {
        const auto dir = std::filesystem::temp_directory_path() /
                         ("zederia_test_plugin_factory_" + juce::Uuid().toString().toStdString());
        const ContentBuild built = buildContentFolder(ZED_CONTENT_DIR, dir.string());
        REQUIRE(built.errors.empty());
        path = dir.string();
    }
    ~FactoryFolder() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

const std::string& factoryFolder() {
    static const FactoryFolder folder;
    return folder.path;
}

} // namespace

TEST_CASE("plug-in: all three oscillators, Sub and Texture play", "[plugin][sources]") {
    const auto& reg = Registry::get();
    const auto level = [&](const std::vector<const char*>& on) {
        Host host;
        host.processor.engine().params().set(idx("osc1.on"), 0.0);
        for (const char* id : on)
            host.processor.engine().params().set(idx(id), 1.0);
        host.midi.addEvent(juce::MidiMessage::noteOn(1, 57, 0.9f), 0);
        host.run(40);
        return zedtest::rmsOf(host.left, 4800, host.left.size());
    };
    CHECK(level({}) == 0.0);
    const double osc1 = level({"osc1.on"}), osc2 = level({"osc2.on"}), osc3 = level({"osc3.on"});
    const double sub = level({"sub.on"}), texture = level({"texture.on"});
    CHECK(osc1 > 0.02);
    CHECK(osc2 == Approx(osc1).epsilon(0.2)); // identical oscillators (random start phases)
    CHECK(osc3 == Approx(osc1).epsilon(0.2));
    CHECK(sub > 0.02);
    CHECK(texture > 0.005);
    CHECK(level({"osc1.on", "osc2.on", "osc3.on", "sub.on", "texture.on"}) > osc1);

    // The Classic engine through the host parameter path (engine is not automatable: set directly).
    Host classic;
    classic.processor.engine().params().set(idx("osc1.engine"), reg.find("osc1.engine")->choiceIndex("classic"));
    classic.midi.addEvent(juce::MidiMessage::noteOn(1, 57, 0.9f), 0);
    classic.run(20);
    CHECK(classic.peak(2400) > 0.02f);
}

TEST_CASE("plug-in: tables and the texture load by reference, change while playing and survive a state round trip",
          "[plugin][sources][state]") {
    zed::rt::resetViolations();
    Host host;
    host.processor.setContentFolders(factoryFolder(), "");
    CHECK(host.processor.library().listTables().size() == 12);
    CHECK(host.processor.library().listTextures().size() == 6);

    host.midi.addEvent(juce::MidiMessage::noteOn(1, 48, 0.9f), 0);
    host.run(10);
    CHECK(host.processor.setTableReference(0, "factory:Formant/Vowel Drift").empty());
    CHECK(host.processor.setTextureReference("factory:Wind").empty());
    host.processor.engine().params().set(idx("texture.on"), 1.0);
    host.run(20); // the swap happens inside the callback, under the guard
    CHECK(host.processor.engine().table(0)->name() == "Vowel Drift");
    CHECK(host.processor.engine().texture()->name == "Wind");
    CHECK(host.processor.tableReference(0) == "factory:Formant/Vowel Drift");
    CHECK(zed::rt::violationCount() == 0);
    bool finite = true;
    for (const float s : host.left)
        finite = finite && std::isfinite(s);
    CHECK(finite);

    // A reference that cannot be loaded is kept in the patch, reported, and played with the built-in table.
    const std::string problem = host.processor.setTableReference(1, "factory:Nowhere/Missing");
    CHECK(problem.find("Missing") != std::string::npos);
    CHECK(host.processor.engine().table(1)->name() == "Basic Shapes");

    juce::MemoryBlock state;
    host.processor.getStateInformation(state);
    Host other;
    other.processor.setContentFolders(factoryFolder(), "");
    other.processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(other.processor.tableReference(0) == "factory:Formant/Vowel Drift");
    CHECK(other.processor.tableReference(1) == "factory:Nowhere/Missing");
    CHECK(other.processor.textureReference() == "factory:Wind");
    CHECK(other.processor.engine().table(0)->name() == "Vowel Drift");
    CHECK(other.processor.engine().texture()->name == "Wind");
    REQUIRE(other.processor.assetProblems().size() == 1);
    CHECK(other.processor.assetProblems()[0].find("osc2") != std::string::npos);
    other.midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.9f), 0);
    other.run(20);
    CHECK(other.peak(2400) > 0.01f);
    CHECK(zed::rt::violationCount() == 0);
}

TEST_CASE("plug-in: tables and textures chosen as the editor does play", "[plugin][editor][sources]") {
    Host host;
    host.processor.setContentFolders(factoryFolder(), "");
    std::unique_ptr<juce::AudioProcessorEditor> base(host.processor.createEditor());
    REQUIRE(dynamic_cast<ZederiaEditor*>(base.get()) != nullptr);
    const auto tables = host.processor.library().listTables();
    const auto textures = host.processor.library().listTextures();
    CHECK(tables.size() == 12);
    CHECK(textures.size() == 6);
    // The source cards change tables and samples through the session, as one undo step each.
    host.processor.session().edit("Osc 2 table", [&](zed::Patch& p) {
        p.tables[1] = tables[3];
        return true;
    });
    CHECK(host.processor.tableReference(1) == tables[3]);
    host.processor.session().edit("Texture sample", [&](zed::Patch& p) {
        p.textureSample = textures[1];
        return true;
    });
    CHECK(host.processor.textureReference() == textures[1]);
    host.processor.session().undo();
    CHECK(host.processor.textureReference().empty());
    host.processor.session().undo();
    CHECK(host.processor.tableReference(1).empty());

    for (const char* id : {"osc2.on", "osc3.on", "sub.on", "texture.on"})
        host.processor.session().setValue(idx(id), 1.0);
    host.processor.keyboardState().noteOn(1, 52, 0.9f);
    host.run(20);
    CHECK(host.peak(2400) > 0.02f);
    base.reset();
}

// --- Milestone 4: MIDI out and thru, MIDI learn, programs, the rack, offline quality ---------------

namespace {

// Runs blocks and keeps what the processor leaves in the host's MIDI buffer.
std::vector<juce::MidiMessage> runCapturing(Host& host, int blocks) {
    std::vector<juce::MidiMessage> out;
    for (int b = 0; b < blocks; ++b) {
        host.buffer.clear();
        host.processor.processBlock(host.buffer, host.midi);
        for (const auto metadata : host.midi)
            out.push_back(metadata.getMessage());
        host.midi.clear();
    }
    return out;
}

} // namespace

TEST_CASE("plug-in: the arpeggiator's notes go to the host; MIDI thru passes the input", "[plugin][midi]") {
    Host host;
    REQUIRE(host.processor.loadPatchJson("{\"format\":\"zederia.patch\",\"version\":1,\"arp\":{\"on\":true}}").ok());
    host.run(1);
    host.midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
    host.midi.addEvent(juce::MidiMessage::noteOn(1, 67, 0.8f), 0);
    const auto made = runCapturing(host, 30); // about 0.3 s: three sixteenths at 120 bpm
    int ons = 0;
    for (const auto& m : made) {
        ons += m.isNoteOn() ? 1 : 0;
        CHECK((m.isNoteOn() || m.isNoteOff()));
    }
    CHECK(ons >= 2);

    // Thru: the incoming controller comes back out unchanged.
    host.processor.setNormalizedFromUi(idx("global.midi_thru"), 1.0f);
    host.run(1);
    host.midi.addEvent(juce::MidiMessage::controllerEvent(3, 20, 99), 17);
    bool found = false;
    for (const auto& m : runCapturing(host, 1))
        found = found || (m.isController() && m.getControllerNumber() == 20 && m.getControllerValue() == 99 && m.getChannel() == 3);
    CHECK(found);
}

TEST_CASE("plug-in: MIDI learn binds a controller, which then moves the parameter", "[plugin][midi]") {
    rt::resetViolations();
    Host host;
    const int cutoff = idx("filter1.cutoff");
    host.processor.startMidiLearn(cutoff);
    host.midi.addEvent(juce::MidiMessage::controllerEvent(2, 74, 64), 5);
    host.run(1);
    host.processor.serviceMessageThread();
    CHECK(host.processor.midiLearnTarget() == -1);
    REQUIRE(host.processor.ccMap().bindings.size() == 1);
    CHECK(host.processor.ccMap().bindings[0].controller == 74);
    CHECK(host.processor.ccMap().bindings[0].param == "filter1.cutoff");
    host.midi.addEvent(juce::MidiMessage::controllerEvent(1, 74, 127), 0);
    host.run(1);
    CHECK(host.processor.plainValue(cutoff) == Approx(host.processor.effectiveDef(cutoff).max).epsilon(1e-6));
    host.midi.addEvent(juce::MidiMessage::controllerEvent(9, 74, 0), 0);
    host.run(1);
    CHECK(host.processor.plainValue(cutoff) == Approx(host.processor.effectiveDef(cutoff).min).epsilon(1e-6));
    host.processor.serviceMessageThread(); // tells the host
    CHECK(rt::violationCount() == 0);

    // The map is part of the host project, and saves and loads on its own.
    juce::MemoryBlock state;
    host.processor.getStateInformation(state);
    ZederiaProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(restored.ccMap() == host.processor.ccMap());
    CcMap reloaded;
    std::vector<Issue> issues;
    REQUIRE(loadCcMap(saveCcMap(host.processor.ccMap()), reloaded, issues));
    CHECK(reloaded == host.processor.ccMap());
    CHECK_FALSE(loadCcMap("{\"format\":\"zederia.ccmap\",\"bindings\":[{\"cc\":300,\"param\":\"filter1.cutoff\"}]}", reloaded, issues));
}

TEST_CASE("plug-in: a program change loads a preset from the program list", "[plugin][programs]") {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("zederia-programs-" + juce::String(juce::Time::currentTimeMillis()) + "-" +
                                       juce::String(juce::Random::getSystemRandom().nextInt(1000000)));
    REQUIRE(dir.createDirectory());
    std::vector<std::string> files;
    for (const char* name : {"First", "Second"}) {
        const auto file = dir.getChildFile(juce::String(name) + ".zpreset");
        Patch p;
        p.meta.name = name;
        REQUIRE(file.replaceWithText(savePatch(p)));
        files.push_back(file.getFullPathName().toStdString());
    }
    Host host;
    host.processor.setProgramList(files);
    CHECK(host.processor.getNumPrograms() == 2);
    CHECK(host.processor.getProgramName(1) == "Second");
    host.midi.addEvent(juce::MidiMessage::programChange(1, 1), 0);
    host.run(1);
    host.processor.serviceMessageThread();
    CHECK(host.processor.getCurrentProgram() == 1);
    CHECK(loadPatch(host.processor.patchJson()).patch.meta.name == "Second");
    host.processor.setCurrentProgram(0);
    CHECK(loadPatch(host.processor.patchJson()).patch.meta.name == "First");
    dir.deleteRecursively();
}

TEST_CASE("plug-in: rack edits rename the slot parameters and keep each effect's values", "[plugin][rack]") {
    Host host;
    REQUIRE(host.processor.insertEffect(0, 0, "delay"));
    REQUIRE(host.processor.insertEffect(0, 0, "reverb"));
    CHECK(host.processor.hostParameter(idx("fx.a1.p3"))->getName(64).contains("Decay"));
    CHECK(host.processor.hostParameter(idx("fx.a2.p3"))->getName(64).contains("Time L"));
    host.processor.hostParameter(idx("fx.a2.p10"))->setValueNotifyingHost(0.8f); // the delay's feedback
    const double feedback = host.processor.plainValue(idx("fx.a2.p10"));
    host.run(4);
    REQUIRE(host.processor.moveEffect(0, 1, 2, 0)); // the delay to Master
    CHECK(host.processor.plainValue(idx("fx.m1.p10")) == Approx(feedback));
    CHECK(host.processor.hostParameter(idx("fx.m1.p10"))->getName(64).contains("Feedback"));
    host.run(4);
    REQUIRE(host.processor.removeEffect(0, 0));
    CHECK_FALSE(host.processor.insertEffect(0, 9, "eq"));
    CHECK_FALSE(host.processor.insertEffect(0, 0, "no such effect"));
    const LoadResult saved = loadPatch(host.processor.patchJson());
    REQUIRE(saved.ok());
    CHECK(saved.patch.fxSlotCount(0) == 0);
    CHECK(saved.patch.fxSlotType(2, 0) == Registry::get().effectIndex("delay"));

    // A locked rack survives a preset change, and the lock is part of the host project.
    host.processor.setRackLocked(true);
    REQUIRE(host.processor.loadPatchJson("{\"format\":\"zederia.patch\",\"version\":1}").ok());
    CHECK(loadPatch(host.processor.patchJson()).patch.fxSlotType(2, 0) == Registry::get().effectIndex("delay"));
    juce::MemoryBlock state;
    host.processor.getStateInformation(state);
    ZederiaProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(restored.rackLocked());
    CHECK(loadPatch(restored.patchJson()).patch.fxSlotType(2, 0) == Registry::get().effectIndex("delay"));

    // Rack presets through the processor.
    const std::string rack = host.processor.rackPresetJson();
    std::vector<Issue> issues;
    REQUIRE(restored.loadRackPresetJson(rack, issues));
    CHECK(restored.rackPresetJson() == rack);
}

TEST_CASE("plug-in: a host rendering offline gets the offline quality", "[plugin][quality]") {
    Host host;
    host.processor.setNormalizedFromUi(idx("filter1.on"), 1.0f);
    host.run(2);
    host.processor.setNonRealtime(true);
    host.run(2);
    CHECK(host.processor.renderingOffline());
    CHECK(host.processor.engine().filterView(0).oversample == 4); // the offline quality is High by default
    host.processor.setNonRealtime(false);
    CHECK(host.processor.getLatencySamples() == 0);
}

TEST_CASE("plug-in: clips import from a MIDI file and export again", "[plugin][clip]") {
    Clip clip;
    clip.notes = {{0.0, 1.0, 60, 0.5}, {2.0, 0.5, 67, 1.0}};
    clip.key = "G major";
    const auto file = midi::clipToFile(clip);
    ZederiaProcessor processor;
    CHECK(processor.importClip(3, file).empty());
    CHECK(processor.clip(3).notes.size() == 2);
    CHECK(processor.clip(3).key == "G major");
    CHECK(processor.exportClip(3) == file);
    CHECK_FALSE(processor.importClip(3, std::vector<std::uint8_t>{1, 2, 3}).empty());
    CHECK_FALSE(processor.importClip(9, file).empty());
}

TEST_CASE("plug-in: every host parameter comes back from the host project as it was set", "[plugin][state]") {
    // What a host's state check does: set every parameter, save, load into a new instance, compare,
    // and save again. Values the patch has no use for (a route that does not exist, an empty slot)
    // must come back too.
    ZederiaProcessor first;
    Rng rng(17);
    for (auto* parameter : first.getParameters())
        parameter->setValueNotifyingHost(static_cast<float>(rng.uniform()));
    juce::MemoryBlock state;
    first.getStateInformation(state);
    ZederiaProcessor second;
    second.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    int differences = 0;
    std::string firstDifference;
    for (int i = 0; i < first.getParameters().size(); ++i) {
        const float a = first.getParameters()[i]->getValue(), b = second.getParameters()[i]->getValue();
        if (std::abs(a - b) > 1.0e-6f) {
            if (differences++ == 0)
                firstDifference = first.getParameters()[i]->getName(64).toStdString();
        }
    }
    INFO("first difference: " << firstDifference);
    CHECK(differences == 0);
    juce::MemoryBlock again;
    second.getStateInformation(again);
    CHECK(again == state);
}
