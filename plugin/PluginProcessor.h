// The plug-in processor: wraps the engine, exposes the automatable parameters to the host with
// stable ids, saves and restores state as a patch document, and reports latency and tail.
#pragma once

#include "zederia/content/Library.h"
#include "zederia/engine/Engine.h"
#include "zederia/midi/CcMap.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/render/OfflineRenderer.h"
#include "zederia/rt/Handoff.h"

#include <array>
#include <atomic>
#include <chrono>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <functional>
#include <span>
#include <string>
#include <vector>

class ZederiaProcessor;
class Session;
class AgentLink;

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
                               private juce::AsyncUpdater,
                               private juce::Timer {
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
    bool producesMidi() const override { return true; } // notes from the arpeggiator, chords and clips; MIDI thru
    double getTailLengthSeconds() const override { return engine_.tailSeconds(); }
    // Programs are the presets of the program list (setProgramList); a program change selects one.
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
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

    // --- Effects rack (message thread) ---
    // Edits fade in the engine, so none of them clicks; values, curves and routes move with their
    // effect. Each returns false when the edit is not possible.
    bool insertEffect(int lane, int position, const std::string& effectId);
    bool removeEffect(int lane, int position);
    bool moveEffect(int fromLane, int fromPosition, int toLane, int toPosition);
    // While locked, loading a patch keeps the current rack.
    void setRackLocked(bool locked);
    bool rackLocked() const { return engine_.rackLocked(); }
    std::string rackPresetJson();
    bool loadRackPresetJson(std::string_view json, std::vector<zed::Issue>& issues);
    std::string slotPresetJson(int lane, int slot);
    bool loadSlotPresetJson(std::string_view json, int lane, int position, std::vector<zed::Issue>& issues);

    // --- Clips (message thread) ---
    // Reads a Standard MIDI File into clip slot 0..7. Returns "" or what went wrong.
    std::string importClip(int slot, std::span<const std::uint8_t> midiFile);
    std::vector<std::uint8_t> exportClip(int slot);
    zed::Clip clip(int slot) const;

    // --- MIDI learn (message thread) ---
    // The next controller that arrives is bound to the parameter (any channel).
    void startMidiLearn(int paramIndex);
    void cancelMidiLearn() { learnTarget_.store(-1, std::memory_order_relaxed); }
    int midiLearnTarget() const { return learnTarget_.load(std::memory_order_relaxed); }
    zed::CcMap ccMap() const;
    void setCcMap(const zed::CcMap& map);

    // --- Programs (message thread) ---
    void setProgramList(std::vector<std::string> presetFiles);
    std::vector<std::string> programList() const;

    // Message-thread housekeeping: finishes MIDI learn, tells the host about values that MIDI
    // controllers moved, loads a program that a program change asked for, frees what the audio
    // thread retired. A timer calls it; tests call it directly.
    void serviceMessageThread();
    // True while the engine renders with the offline quality (the host renders faster than real time).
    bool renderingOffline() const { return offlineNow_.load(std::memory_order_relaxed); }

    // --- For the editor ---
    juce::MidiKeyboardState& keyboardState() { return keyboard_; }
    float meterPeak(int channel) { return meter_[static_cast<std::size_t>(channel)].exchange(0.0f, std::memory_order_relaxed); }
    int activeVoices() const { return engine_.activeVoices(); }
    // Events from the interface (panic, audition): played at the start of the next block.
    void pushUiEvent(const zed::MidiEvent& event) { pushKeyboardEvent(event); }
    // The pitch and modulation wheels on the screen (pitch -1..1, modulation 0..1). Their latest
    // position, not a queue of moves: the next block plays it, so a full queue or a host that stops
    // calling the audio thread while the wheel moves can never leave the pitch where the wheel is not.
    void setScreenWheel(bool pitch, float value) { (pitch ? screenBend_ : screenMod_).store(value, std::memory_order_relaxed); }
    float screenWheel(bool pitch) const { return (pitch ? screenBend_ : screenMod_).load(std::memory_order_relaxed); }
    // Keys down from any input (the host's notes and the on-screen keyboard), as the last audio
    // block saw them: for the keyboard's display.
    bool noteDown(int key) const {
        return key >= 0 && key < 128 && ((downShown_[static_cast<std::size_t>(key >> 6)].load(std::memory_order_relaxed) >> (key & 63)) & 1u) != 0;
    }
    // Keys the on-screen keyboard holds (message thread's view).
    bool uiNoteHeld(int key) const {
        return key >= 0 && key < 128 && ((uiHeld_[static_cast<std::size_t>(key >> 6)].load(std::memory_order_relaxed) >> (key & 63)) & 1u) != 0;
    }

    // --- Whole states: undo, A/B, presets (message thread; ProcessorEditing.cpp) -------------------
    // The current patch: the document with the engine's current values.
    zed::Patch snapshot();
    // Makes the instrument match `state`, changing only what differs (values, routes, shapes, the
    // rack, tables, play data, text). Locks are ignored: an undo restores exactly.
    void restore(const zed::Patch& state);
    // A preset load: like restore, but locked parameters keep their values and a locked rack or
    // tuning stays (plan 3.11). Tables load by reference (or from the patch's own data).
    void loadPresetPatch(const zed::Patch& patch);
    // A table edited in the workshop, while the pointer moves: kept in the document at once and
    // built on a worker thread, which hands it to the engine (plan 5.8: audio within 50 ms).
    void setTableLive(int osc, std::shared_ptr<const zed::Wavetable> table);
    // Milliseconds from the last setTableLive() to the engine having the new table; -1 while building.
    double lastTableLatencyMs() const { return tableLatencyMs_.load(std::memory_order_relaxed); }
    bool tableBuildPending() const;
    // The patch's text (name, tags ...) and the routes, without copying the whole patch.
    zed::PatchMeta meta() const;
    void setMeta(const zed::PatchMeta& meta); // text only: not an undo step
    std::vector<zed::ModRoute> routes() const;
    std::array<std::string, zed::kNumMacros> macroNames() const;
    // Reads the document's structure (shapes, clips, rack layout ...) under its lock. Parameter
    // values in it may be older than the engine's; use plainValue() for those.
    void readDocument(const std::function<void(const zed::Patch&)>& reader) const;
    // Live edits while the pointer moves (the editor records the undo step when it is released):
    // the document and the engine take the new structure at once.
    void setLfoShapeLive(int lfo, const zed::Shape& shape);
    void setEnvShapeLive(int env, const zed::EnvShape& shape);
    void setWarpCurveLive(int osc, int slot, const zed::Shape& shape);
    void setFxCurveLive(int lane, int slot, int curve, const zed::Shape& shape);
    void setRouteCurveLive(int route, const zed::Shape& shape);
    void setArpPatternLive(int pattern, const zed::ArpPattern& steps);
    void setClipLive(int slot, const zed::Clip& clip);
    void setChordNotesLive(const std::vector<int>& notes);
    // The editable form of an oscillator's table: the patch's own data, the file it names, or the built-in table.
    std::shared_ptr<const zed::Wavetable> editableTable(int osc);

    // --- Locks (plan 3.11): survive preset changes ----------------------------------------------
    void setParamLocked(int index, bool locked);
    bool paramLocked(int index) const;
    std::vector<bool> lockMask() const;

    // --- What the interface shows ----------------------------------------------------------------
    // The newest `count` samples of the output (mono), oldest first; fewer when not enough exist.
    int readOutput(float* dest, int count) const;
    // Audio callback time against real time, in percent: smoothed average, and the peak since the last call.
    float cpuAverage() const { return cpuAverage_.load(std::memory_order_relaxed); }
    float cpuPeak() { return cpuPeak_.exchange(0.0f, std::memory_order_relaxed); }
    double hostBpm() const { return hostBpm_.load(std::memory_order_relaxed); }
    bool hostPlaying() const { return hostPlaying_.load(std::memory_order_relaxed); }
    double currentSampleRate() const { return sampleRate_; }
    // Plays a short phrase through the instrument (library audition). Note times in seconds.
    void audition(const std::vector<zed::NoteSpec>& notes);
    // An agent's audition (milestone 6): the output is held at or below `capGain` (peak) while it
    // plays and for two seconds after its last note.
    void audition(const std::vector<zed::NoteSpec>& notes, float capGain);
    // Agent Link (milestone 6): off by default; the AGENT page switches it.
    AgentLink& agentLink() { return *agentLink_; }
    void stopAudition();
    bool auditioning() const { return auditionLeft_.load(std::memory_order_relaxed) > 0; }

    // The instance's interface state (open page, selections), kept in the host project.
    std::string uiState() const;
    void setUiState(std::string state);
    // Counts restores of the interface state from the host (a project opened while an editor is
    // open): an open editor then takes the restored state instead of saving its own over it.
    int uiStateRestores() const { return uiRestores_.load(std::memory_order_relaxed); }
    Session& session() { return *session_; }

private:
    void handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void pushKeyboardEvent(const zed::MidiEvent& event);
    void handleAsyncUpdate() override;
    void timerCallback() override { serviceMessageThread(); }
    void applyPatch(const zed::Patch& patch);
    void syncDocumentValues(); // documentLock_ held: the document takes the engine's current values
    void rackEdited(const zed::FxMoves& moves); // documentLock_ held: hand the edited rack over
    void publishSlotTypes();
    bool loadProgramFile(const std::string& file);
    bool detectFastRender(int frames) noexcept;

    static constexpr int kMaxEventsPerBlock = 2048;
    static constexpr int kKeyboardQueue = 256;
    static constexpr int kNotifyQueue = 512;
    static constexpr int kMaxMidiOut = 1024;

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
    // The keys the screen holds (the message thread's word) against the keys the audio thread
    // started from the screen: a started key the screen no longer holds is released, so a lost
    // release (a full queue while audio was stopped) can never leave a note hanging.
    std::array<std::atomic<std::uint64_t>, 2> uiHeld_{};
    std::array<std::uint64_t, 2> uiStarted_{};
    std::array<std::uint64_t, 2> uiEnded_{}; // ended here before their own release arrived: that release is dropped
    // The screen's wheels (message thread) and what the audio thread last played of them.
    std::atomic<float> screenBend_{0.0f}, screenMod_{0.0f};
    float playedBend_ = 0.0f, playedMod_ = 0.0f;
    // Keys down from any input, for the keyboard's display (audio thread, then published).
    std::array<std::uint64_t, 2> down_{};
    std::array<std::atomic<std::uint64_t>, 2> downShown_{};

    std::array<zed::MidiEvent, kMaxEventsPerBlock> blockEvents_{};
    std::array<std::atomic<float>, 2> meter_{};

    // MIDI out, MIDI learn and program change.
    std::array<zed::MidiEvent, kMaxMidiOut> midiOut_{};
    zed::CcMap ccMap_;                       // documentLock_
    zed::rt::Handoff<zed::CcTable> ccTable_; // what the audio thread reads
    std::atomic<int> learnTarget_{-1};
    std::atomic<int> learned_{-1}; // (channel << 8) | controller, from the audio thread
    juce::AbstractFifo notifyFifo_{kNotifyQueue};
    std::array<int, kNotifyQueue> notifyParams_{};
    std::atomic<int> pendingProgram_{-1};
    std::vector<std::string> programs_; // documentLock_
    int currentProgram_ = 0;
    int midiThruIndex_ = 0;

    // Offline quality: the host says it renders offline, or blocks arrive faster than real time.
    std::chrono::steady_clock::time_point lastBlock_{};
    bool haveLastBlock_ = false, fastRender_ = false;
    double windowWall_ = 0.0, windowAudio_ = 0.0, sampleRate_ = 48000.0;
    std::atomic<bool> offlineNow_{false};

    // Milestone 5: the interface's view of the audio, locks, audition, table builds, the session.
    void applyLoaded(const zed::Patch& state, bool honourLocks); // documentLock_ not held
    void buildTables();                                          // the table builder thread's work
    static constexpr int kOutputRing = 8192;
    std::array<float, kOutputRing> outputRing_{};
    std::atomic<int> outputWrite_{0};
    std::atomic<float> cpuAverage_{0.0f}, cpuPeak_{0.0f};
    std::atomic<double> hostBpm_{120.0};
    std::atomic<bool> hostPlaying_{false};
    struct AuditionPlan {
        std::vector<zed::MidiEvent> events; // offsets in samples from the start of the plan
        float capGain = 1.0f;               // agent auditions: the output's peak is held at or below this
        std::int64_t capUntil = 0;          // samples from the start of the plan
    };
    float auditionGain_ = 1.0f;             // audio thread: the gain that holds the cap
    zed::rt::Handoff<AuditionPlan> auditionPlan_;
    std::int64_t auditionPos_ = 0; // audio thread
    std::size_t auditionNext_ = 0; // audio thread
    std::atomic<std::int64_t> auditionLeft_{0};
    std::vector<bool> locked_;           // documentLock_
    std::string uiState_;                // documentLock_
    std::atomic<int> uiRestores_{0};
    // Workshop table builds: the newest request per oscillator, built off the message thread.
    class TableBuilder;
    std::unique_ptr<TableBuilder> tableBuilder_;
    std::atomic<double> tableLatencyMs_{0.0};
    std::unique_ptr<Session> session_;
    std::unique_ptr<AgentLink> agentLink_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZederiaProcessor)
};
