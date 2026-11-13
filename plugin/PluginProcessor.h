// The plug-in processor: wraps the engine, exposes the automatable parameters to the host with
// stable ids, saves and restores state as a patch document, and reports latency and tail.
#pragma once

#include "zederia/content/Library.h"
#include "zederia/engine/Engine.h"
#include "zederia/patch/PatchIO.h"

#include <array>
#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <string>
#include <vector>

class ZederiaProcessor;

// A host parameter backed by one registry entry. Values live in the engine's parameter store in
// plain units; the host sees them normalized through the entry's range and skew.
class ZederiaParameter final : public juce::AudioProcessorParameterWithID {
public:
    ZederiaParameter(ZederiaProcessor& owner, const zed::ParamDef& def);

    int registryIndex() const { return index_; }

    float getValue() const override;
    void setValue(float normalized) override;
    float getDefaultValue() const override;
    juce::String getName(int maximumLength) const override;
    juce::String getLabel() const override;
    int getNumSteps() const override;
    bool isDiscrete() const override;
    bool isBoolean() const override;
    juce::String getText(float normalized, int maximumLength) const override;
    float getValueForText(const juce::String& text) const override;

private:
    ZederiaProcessor& owner_;
    int index_;
};

class ZederiaProcessor final : public juce::AudioProcessor,
                               private juce::MidiKeyboardState::Listener,
                               private juce::AsyncUpdater {
public:
    ZederiaProcessor();
    ~ZederiaProcessor() override;

    // --- juce::AudioProcessor ---
    const juce::String getName() const override { return "Zederia"; }
    void prepareToPlay(double sampleRate, int blockSize) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; } // MIDI out arrives with the play features
    double getTailLengthSeconds() const override { return engine_.tailSeconds(); }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destination) override;
    void setStateInformation(const void* data, int size) override;

    zed::Engine& engine() { return engine_; }

    // --- Parameters (any non-audio thread) ---
    // The definition that governs a value: the loaded effect's parameter for a rack slot's
    // generic parameters, otherwise the registry entry.
    const zed::ParamDef& effectiveDef(int index) const;
    double plainValue(int index) const { return engine_.params().get(index); }
    float normalizedValue(int index) const;
    // Sets a value from the interface; goes through the host when the parameter is automatable.
    void setNormalizedFromUi(int index, float normalized);
    void beginGesture(int index);
    void endGesture(int index);
    ZederiaParameter* hostParameter(int index) const { return hostParams_[static_cast<std::size_t>(index)]; }

    // --- Patch (any non-audio thread) ---
    std::string patchJson(bool compact = false);
    // Replaces the whole state. Nothing changes when the document has errors.
    zed::LoadResult loadPatchJson(std::string_view json);
    int stateGeneration() const { return generation_.load(std::memory_order_relaxed); }

    // --- Wavetables and the texture sample (any non-audio thread) ---
    // Where "factory:" and "user:" references are looked up; the standard folders by default.
    void setContentFolders(const std::string& factoryDir, const std::string& userDir);
    zed::ContentLibrary library() const;
    // Selects the table of oscillator 0..2, or the texture sample, by reference ("" is the
    // built-in one). The reference is stored in the patch; notes that are playing fade over.
    // Returns "" or what went wrong (the built-in object is then used).
    std::string setTableReference(int osc, const std::string& reference);
    std::string setTextureReference(const std::string& reference);
    std::string tableReference(int osc) const;
    std::string textureReference() const;
    // What could not be loaded when the current patch was applied.
    std::vector<std::string> assetProblems() const;

    // --- For the editor ---
    juce::MidiKeyboardState& keyboardState() { return keyboard_; }
    float meterPeak(int channel) { return meter_[static_cast<std::size_t>(channel)].exchange(0.0f, std::memory_order_relaxed); }
    int activeVoices() const { return engine_.activeVoices(); }

private:
    void handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void pushKeyboardEvent(const zed::MidiEvent& event);
    void handleAsyncUpdate() override;
    void applyPatch(const zed::Patch& patch);

    static constexpr int kMaxEventsPerBlock = 2048;
    static constexpr int kKeyboardQueue = 256;

    zed::Engine engine_;
    std::vector<ZederiaParameter*> hostParams_; // by registry index; null when not automatable

    // Everything in the patch that is not a parameter value. Guarded by documentLock_, which the
    // audio thread never takes.
    juce::CriticalSection documentLock_;
    zed::Patch document_;
    zed::ContentLibrary library_ = zed::ContentLibrary::standard();
    std::vector<std::string> assetProblems_;
    std::array<std::atomic<int>, zed::kNumFxLanes * zed::kNumFxSlots> slotTypes_;
    std::atomic<int> generation_{0};

    // On-screen keyboard: the message thread writes, the audio thread reads, without locks.
    juce::MidiKeyboardState keyboard_;
    juce::AbstractFifo keyboardFifo_{kKeyboardQueue};
    std::array<zed::MidiEvent, kKeyboardQueue> keyboardEvents_{};

    std::array<zed::MidiEvent, kMaxEventsPerBlock> blockEvents_{};
    std::array<std::atomic<float>, 2> meter_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZederiaProcessor)
};
