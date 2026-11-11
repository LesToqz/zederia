#include "../support/TestUtil.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
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

TEST_CASE("plug-in: the temporary editor opens, lists parameters and follows the processor", "[plugin][editor]") {
    Host host;
    std::unique_ptr<juce::AudioProcessorEditor> base(host.processor.createEditor());
    auto* editor = dynamic_cast<ZederiaEditor*>(base.get());
    REQUIRE(editor != nullptr);
    CHECK(editor->getWidth() >= 640);
    CHECK(editor->visibleParameterCount() == Registry::get().size());
    editor->setSearchText("cutoff");
    CHECK(editor->visibleParameterCount() >= 2);
    CHECK(editor->visibleParameterCount() < 40);
    editor->setSearchText("no such parameter anywhere");
    CHECK(editor->visibleParameterCount() == 0);
    editor->setSearchText("");
    editor->setSize(700, 450);
    editor->setSize(1500, 1000);

    // The keyboard component drives the same state the processor listens to.
    host.processor.keyboardState().noteOn(1, 64, 0.9f);
    host.run(4);
    CHECK(host.peak() > 0.0f);
    base.reset(); // closing the editor releases held keys
    host.run(12);
    CHECK(host.processor.activeVoices() == 0);
}
