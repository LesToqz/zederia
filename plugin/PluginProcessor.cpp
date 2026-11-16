#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "zederia/midi/MidiFile.h"
#include "zederia/render/OfflineRenderer.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/rt/Denormals.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>

namespace {

// Plug-in state in a host project (plan 5.6): a small header followed by the deflated payload.
// Version 1: the patch JSON. Version 2: the patch JSON, a zero byte, then the instrument's own
// settings as JSON (controller map, program list, rack and tuning locks).
constexpr char kStateMagic[4] = {'Z', 'E', 'D', 'S'};
constexpr std::uint32_t kStateVersion = 2;
constexpr std::size_t kStateHeaderBytes = 12; // magic, version, uncompressed size



std::uint32_t readU32(const void* data) {
    const auto* b = static_cast<const std::uint8_t*>(data);
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

void writeU32(juce::MemoryOutputStream& out, std::uint32_t v) {
    const char bytes[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16),
                           static_cast<char>(v >> 24)};
    out.write(bytes, 4);
}

} // namespace

// --- ZederiaParameter ----------------------------------------------------------------------------

ZederiaParameter::ZederiaParameter(ZederiaProcessor& owner, const zed::ParamDef& def)
    : juce::AudioProcessorParameterWithID(juce::ParameterID{juce::String(def.id), 1}, juce::String(def.name),
                                          juce::AudioProcessorParameterWithIDAttributes().withAutomatable(def.automatable)),
      owner_(owner), index_(def.index) {
    // The host's id for this parameter is the framework's hash of the string id. The registry
    // computes the same hash so tests can check for collisions; make sure the two agree.
    jassert((static_cast<std::uint32_t>(juce::String(def.id).hashCode()) & 0x7fffffffu) == def.hostId);
}

float ZederiaParameter::getValue() const { return owner_.normalizedValue(index_); }

void ZederiaParameter::setValue(float normalized) {
    // Called by the host, possibly on the audio thread: one atomic store, nothing else.
    owner_.engine().params().set(index_, owner_.effectiveDef(index_).fromNormalized(normalized));
}

float ZederiaParameter::getDefaultValue() const {
    const auto& def = owner_.effectiveDef(index_);
    return static_cast<float>(def.toNormalized(def.def));
}

juce::String ZederiaParameter::getName(int maximumLength) const {
    const auto& reg = zed::Registry::get();
    const auto& own = reg.at(index_);
    const auto& def = owner_.effectiveDef(index_);
    juce::String shown(own.name);
    if (&def != &own) { // a rack slot's generic parameter takes the loaded effect's name
        const auto& module = own.module; // "fx.a3"
        for (const auto& m : reg.modules())
            if (m.id == module)
                shown = juce::String(m.name) + " " + juce::String(def.name);
    }
    return shown.substring(0, maximumLength);
}

juce::String ZederiaParameter::getLabel() const {
    return juce::String(std::string(zed::unitLabel(owner_.effectiveDef(index_).unit)));
}

int ZederiaParameter::getNumSteps() const {
    const auto& def = owner_.effectiveDef(index_);
    return def.stepped() ? def.steps() : juce::AudioProcessor::getDefaultNumParameterSteps();
}

bool ZederiaParameter::isDiscrete() const { return owner_.effectiveDef(index_).stepped(); }
bool ZederiaParameter::isBoolean() const { return owner_.effectiveDef(index_).type == zed::ParamType::Bool; }

juce::String ZederiaParameter::getText(float normalized, int maximumLength) const {
    const auto& def = owner_.effectiveDef(index_);
    return juce::String(def.format(def.fromNormalized(normalized))).substring(0, maximumLength);
}

float ZederiaParameter::getValueForText(const juce::String& text) const {
    const auto& def = owner_.effectiveDef(index_);
    const auto trimmed = text.trim();
    if (def.type == zed::ParamType::Choice) {
        const int choice = def.choiceIndex(trimmed.toStdString());
        if (choice >= 0)
            return static_cast<float>(def.toNormalized(choice));
    }
    if (def.type == zed::ParamType::Bool && (trimmed.equalsIgnoreCase("on") || trimmed.equalsIgnoreCase("off")))
        return trimmed.equalsIgnoreCase("on") ? 1.0f : 0.0f;
    if (def.unit == zed::Unit::Db && trimmed.startsWithIgnoreCase("-inf"))
        return 0.0f;
    double value = trimmed.getDoubleValue();
    if (def.unit == zed::Unit::Percent && def.type == zed::ParamType::Float)
        value /= 100.0;
    return static_cast<float>(def.toNormalized(value));
}

// --- ZederiaProcessor ----------------------------------------------------------------------------

ZederiaProcessor::ZederiaProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    const auto& reg = zed::Registry::get();
    for (auto& type : slotTypes_)
        type.store(-1, std::memory_order_relaxed);
    hostParams_.assign(static_cast<std::size_t>(reg.size()), nullptr);
    for (const auto& def : reg.params()) {
        if (!def.automatable)
            continue;
        auto parameter = std::make_unique<ZederiaParameter>(*this, def);
        hostParams_[static_cast<std::size_t>(def.index)] = parameter.get();
        addParameter(parameter.release());
    }
    setLatencySamples(engine_.latencySamples());
    ccTable_.set(std::make_shared<const zed::CcTable>(zed::CcTable::build(ccMap_)));
    midiThruIndex_ = reg.indexOf("global.midi_thru");
    keyboard_.addListener(this);
    startTimerHz(30);
}

ZederiaProcessor::~ZederiaProcessor() {
    stopTimer();
    keyboard_.removeListener(this);
    cancelPendingUpdate();
}

void ZederiaProcessor::prepareToPlay(double sampleRate, int) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    haveLastBlock_ = fastRender_ = false;
    windowWall_ = windowAudio_ = 0.0;
    engine_.prepare(sampleRate);
    setLatencySamples(engine_.latencySamples()); // no quality mode and no effect adds latency
}

void ZederiaProcessor::reset() {
    engine_.reset();
    haveLastBlock_ = fastRender_ = false;
    windowWall_ = windowAudio_ = 0.0;
}

// Blocks that arrive much faster than real time mean the host is rendering: the engine switches
// to the offline quality (plan 3.12). Judged over a quarter of a second of audio, with a margin
// either way so that ordinary jitter never switches it.
bool ZederiaProcessor::detectFastRender(int frames) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (haveLastBlock_) {
        windowWall_ += std::chrono::duration<double>(now - lastBlock_).count();
        windowAudio_ += frames / sampleRate_;
        if (windowAudio_ >= 0.25) {
            const double ratio = windowWall_ / windowAudio_;
            if (ratio < 0.5)
                fastRender_ = true;
            else if (ratio > 0.8)
                fastRender_ = false;
            windowWall_ = windowAudio_ = 0.0;
        }
    }
    lastBlock_ = now;
    haveLastBlock_ = true;
    return fastRender_;
}

bool ZederiaProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && layouts.getMainInputChannelSet().isDisabled();
}

void ZederiaProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    const zed::rt::AudioThreadScope audioThread;
    const zed::rt::ScopedNoDenormals noDenormals;
    const int frames = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || frames <= 0) {
        buffer.clear();
        midi.clear();
        return;
    }
    const bool offline = isNonRealtime() || detectFastRender(frames);
    engine_.setOffline(offline);
    offlineNow_.store(offline, std::memory_order_relaxed);
    ccTable_.retirePrevious();
    if (ccTable_.take())
        ccTable_.retirePrevious();
    const zed::CcTable* controllers = ccTable_.current();
    const bool learning = learnTarget_.load(std::memory_order_relaxed) >= 0;

    // Keys from the on-screen keyboard first (they have no position inside the block), then the
    // host's events in order, each at its own sample.
    int count = 0;
    {
        const auto scope = keyboardFifo_.read(keyboardFifo_.getNumReady());
        const auto take = [&](int start, int size) {
            for (int i = 0; i < size && count < kMaxEventsPerBlock; ++i)
                blockEvents_[static_cast<std::size_t>(count++)] = keyboardEvents_[static_cast<std::size_t>(start + i)];
        };
        take(scope.startIndex1, scope.blockSize1);
        take(scope.startIndex2, scope.blockSize2);
    }
    for (const auto metadata : midi) {
        if (count >= kMaxEventsPerBlock)
            break;
        if (metadata.numBytes < 2 || metadata.numBytes > 3)
            continue;
        const std::uint8_t status = metadata.data[0];
        if ((status & 0xF0) == 0xC0) { // program change: the message thread loads the preset
            pendingProgram_.store(metadata.data[1] & 0x7f, std::memory_order_relaxed);
            continue;
        }
        if ((status & 0xF0) == 0xB0 && metadata.numBytes == 3) {
            const int channel = status & 0x0F, controller = metadata.data[1] & 0x7f;
            if (learning)
                learned_.store((channel << 8) | controller, std::memory_order_relaxed);
            if (controllers != nullptr) { // MIDI learn: the controller moves its parameter
                const auto ch = static_cast<std::size_t>(channel), cc = static_cast<std::size_t>(controller);
                const int index = controllers->param[ch][cc];
                if (index >= 0) {
                    const float position = static_cast<float>(metadata.data[2] & 0x7f) / 127.0f;
                    const float normalized = controllers->min[ch][cc] + position * (controllers->max[ch][cc] - controllers->min[ch][cc]);
                    engine_.params().set(index, effectiveDef(index).fromNormalized(normalized));
                    const auto scope = notifyFifo_.write(1);
                    if (scope.blockSize1 > 0)
                        notifyParams_[static_cast<std::size_t>(scope.startIndex1)] = index;
                }
            }
        }
        zed::MidiEvent event;
        const std::uint8_t data2 = metadata.numBytes == 3 ? metadata.data[2] : std::uint8_t{0};
        if (zed::midiFromBytes(status, metadata.data[1], data2, std::clamp(metadata.samplePosition, 0, frames - 1), event))
            blockEvents_[static_cast<std::size_t>(count++)] = event;
    }
    // MIDI thru leaves the incoming events in the buffer; the notes the play features make join them.
    if (engine_.params().get(midiThruIndex_) < 0.5)
        midi.clear();
    int generated = 0;

    zed::ProcessArgs args;
    args.left = buffer.getWritePointer(0);
    args.right = buffer.getWritePointer(1);
    args.frames = frames;
    args.midi = {blockEvents_.data(), static_cast<std::size_t>(count)};
    if (auto* head = getPlayHead()) {
        if (const auto position = head->getPosition()) {
            args.transport.bpm = position->getBpm().orFallback(120.0);
            args.transport.ppqPosition = position->getPpqPosition().orFallback(0.0);
            args.transport.playing = position->getIsPlaying();
            if (const auto signature = position->getTimeSignature()) {
                args.transport.timeSigNumerator = signature->numerator;
                args.transport.timeSigDenominator = signature->denominator;
            }
            args.transport.barStartPpq = position->getPpqPositionOfLastBarStart().orFallback(0.0);
        }
    }
    args.midiOut = midiOut_.data();
    args.midiOutCapacity = kMaxMidiOut;
    args.midiOutCount = &generated;
    engine_.process(args);
    for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
        buffer.clear(channel, 0, frames);

    // Notes for the host, as many as fit in the room the plug-in wrappers reserve (VST3 and CLAP
    // reserve kMidiOutBytes, so this never grows their buffer; an entry is a 4-byte position, a
    // 2-byte size and the 3-byte message).
    constexpr int kMidiOutBytes = 2048;
    if (generated > 0)
        midi.ensureSize(kMidiOutBytes);
    for (int i = 0; i < generated; ++i) {
        if (kMidiOutBytes - midi.data.size() < 9)
            break;
        const zed::MidiEvent& e = midiOut_[static_cast<std::size_t>(i)];
        const bool on = e.type == zed::MidiEvent::Type::NoteOn;
        const auto velocity = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(e.value * 127.0f)), on ? 1 : 0, 127));
        const std::uint8_t bytes[3] = {static_cast<std::uint8_t>((on ? 0x90 : 0x80) | (e.channel & 0x0F)), e.number, velocity};
        midi.addEvent(bytes, 3, std::clamp(e.offset, 0, frames - 1));
    }

    for (int channel = 0; channel < 2; ++channel) {
        const float peak = buffer.getMagnitude(channel, 0, frames);
        auto& held = meter_[static_cast<std::size_t>(channel)];
        if (peak > held.load(std::memory_order_relaxed))
            held.store(peak, std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* ZederiaProcessor::createEditor() { return new ZederiaEditor(*this); }

// --- Parameters ----------------------------------------------------------------------------------

const zed::ParamDef& ZederiaProcessor::effectiveDef(int index) const {
    const auto& reg = zed::Registry::get();
    int lane = 0, slot = 0, k = 0;
    if (reg.fxSlotParamAt(index, lane, slot, k)) {
        const int type = slotTypes_[static_cast<std::size_t>(lane * zed::kNumFxSlots + slot)].load(std::memory_order_relaxed);
        if (type >= 0) {
            const auto& params = reg.effectTypes()[static_cast<std::size_t>(type)].params;
            if (k < static_cast<int>(params.size()))
                return params[static_cast<std::size_t>(k)];
        }
    }
    return reg.at(index);
}

float ZederiaProcessor::normalizedValue(int index) const {
    return static_cast<float>(effectiveDef(index).toNormalized(plainValue(index)));
}

void ZederiaProcessor::setNormalizedFromUi(int index, float normalized) {
    if (auto* parameter = hostParameter(index))
        parameter->setValueNotifyingHost(normalized);
    else
        engine_.params().set(index, effectiveDef(index).fromNormalized(normalized));
}

void ZederiaProcessor::beginGesture(int index) {
    if (auto* parameter = hostParameter(index))
        parameter->beginChangeGesture();
}

void ZederiaProcessor::endGesture(int index) {
    if (auto* parameter = hostParameter(index))
        parameter->endChangeGesture();
}

// --- Patch and state -----------------------------------------------------------------------------

void ZederiaProcessor::syncDocumentValues() {
    for (int i = 0; i < engine_.params().size(); ++i)
        document_.set(i, engine_.params().get(i)); // values the patch has no use for are left alone
}

std::string ZederiaProcessor::patchJson(bool compact) {
    const juce::ScopedLock lock(documentLock_);
    syncDocumentValues();
    zed::SaveOptions options;
    options.compact = compact;
    return zed::savePatch(document_, options);
}

zed::LoadResult ZederiaProcessor::loadPatchJson(std::string_view json) {
    zed::LoadResult result = zed::loadPatch(json);
    if (result.ok())
        applyPatch(result.patch);
    return result;
}

void ZederiaProcessor::applyPatch(const zed::Patch& incoming) {
    bool layoutChanged = false;
    {
        const juce::ScopedLock lock(documentLock_);
        zed::Patch patch = incoming;
        if (engine_.rackLocked()) { // the rack stays as it is
            syncDocumentValues();
            patch.copyRackFrom(document_);
        }
        document_ = patch;
        // The tables and the sample the patch names: loaded here, off the audio thread, and
        // handed to the engine, which switches to them at the start of its next block.
        assetProblems_ = zed::applyPatchAssets(engine_, patch, library_);
        for (int lane = 0; lane < zed::kNumFxLanes; ++lane) {
            for (int slot = 0; slot < zed::kNumFxSlots; ++slot) {
                auto& type = slotTypes_[static_cast<std::size_t>(lane * zed::kNumFxSlots + slot)];
                layoutChanged = layoutChanged || type.load(std::memory_order_relaxed) != patch.fxSlotType(lane, slot);
                type.store(patch.fxSlotType(lane, slot), std::memory_order_relaxed);
            }
        }
        // Every value, and with them the routes, shapes, tempo-linked times and the tuning.
        engine_.loadPatch(patch);
    }
    // Tell the host about every value that moved.
    for (auto* parameter : hostParams_)
        if (parameter != nullptr)
            parameter->sendValueChangedMessageToListeners(parameter->getValue());
    generation_.fetch_add(1, std::memory_order_relaxed);
    if (layoutChanged)
        triggerAsyncUpdate(); // parameter names changed: the host is told on the message thread
}

// --- Wavetables and the texture sample -----------------------------------------------------------

void ZederiaProcessor::setContentFolders(const std::string& factoryDir, const std::string& userDir) {
    const juce::ScopedLock lock(documentLock_);
    library_.setFactoryDir(factoryDir);
    library_.setUserDir(userDir);
}

zed::ContentLibrary ZederiaProcessor::library() const {
    const juce::ScopedLock lock(documentLock_);
    return library_;
}

std::string ZederiaProcessor::setTableReference(int osc, const std::string& reference) {
    if (osc < 0 || osc >= zed::kNumOscs)
        return "there is no such oscillator";
    std::string error;
    {
        const juce::ScopedLock lock(documentLock_);
        auto table = library_.loadTable(reference, error);
        document_.tables[static_cast<std::size_t>(osc)] = reference;
        engine_.setTable(osc, std::move(table)); // null (not loadable) selects the built-in table
    }
    generation_.fetch_add(1, std::memory_order_relaxed);
    return error;
}

std::string ZederiaProcessor::setTextureReference(const std::string& reference) {
    std::string error;
    {
        const juce::ScopedLock lock(documentLock_);
        auto sample = library_.loadTexture(reference, error);
        document_.textureSample = reference;
        engine_.setTexture(std::move(sample));
    }
    generation_.fetch_add(1, std::memory_order_relaxed);
    return error;
}

std::string ZederiaProcessor::tableReference(int osc) const {
    const juce::ScopedLock lock(documentLock_);
    return osc >= 0 && osc < zed::kNumOscs ? document_.tables[static_cast<std::size_t>(osc)] : std::string();
}

std::string ZederiaProcessor::textureReference() const {
    const juce::ScopedLock lock(documentLock_);
    return document_.textureSample;
}

std::vector<std::string> ZederiaProcessor::assetProblems() const {
    const juce::ScopedLock lock(documentLock_);
    return assetProblems_;
}

void ZederiaProcessor::handleAsyncUpdate() {
    updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withParameterInfoChanged(true));
}

// --- Effects rack ----------------------------------------------------------------------------------

void ZederiaProcessor::publishSlotTypes() {
    for (int lane = 0; lane < zed::kNumFxLanes; ++lane)
        for (int slot = 0; slot < zed::kNumFxSlots; ++slot)
            slotTypes_[static_cast<std::size_t>(lane * zed::kNumFxSlots + slot)].store(document_.fxSlotType(lane, slot),
                                                                                       std::memory_order_relaxed);
}

void ZederiaProcessor::rackEdited(const zed::FxMoves& moves) {
    publishSlotTypes();
    engine_.setRack(document_, &moves); // the moved values reach the store with the new layout
    engine_.setRoutes(document_.routes); // routes moved with their effects
    generation_.fetch_add(1, std::memory_order_relaxed);
    triggerAsyncUpdate(); // slot parameters changed names: the host is told on the message thread
}

bool ZederiaProcessor::insertEffect(int lane, int position, const std::string& effectId) {
    const int type = zed::Registry::get().effectIndex(effectId);
    if (type < 0)
        return false;
    {
        const juce::ScopedLock lock(documentLock_);
        syncDocumentValues();
        zed::FxMoves moves;
        if (!document_.insertFxSlot(lane, position, type, &moves))
            return false;
        rackEdited(moves);
    }
    for (auto* parameter : hostParams_)
        if (parameter != nullptr)
            parameter->sendValueChangedMessageToListeners(parameter->getValue());
    return true;
}

bool ZederiaProcessor::removeEffect(int lane, int position) {
    {
        const juce::ScopedLock lock(documentLock_);
        syncDocumentValues();
        zed::FxMoves moves;
        if (!document_.removeFxSlot(lane, position, &moves))
            return false;
        rackEdited(moves);
    }
    for (auto* parameter : hostParams_)
        if (parameter != nullptr)
            parameter->sendValueChangedMessageToListeners(parameter->getValue());
    return true;
}

bool ZederiaProcessor::moveEffect(int fromLane, int fromPosition, int toLane, int toPosition) {
    {
        const juce::ScopedLock lock(documentLock_);
        syncDocumentValues();
        zed::FxMoves moves;
        if (!document_.moveFxSlot(fromLane, fromPosition, toLane, toPosition, &moves))
            return false;
        rackEdited(moves);
    }
    for (auto* parameter : hostParams_)
        if (parameter != nullptr)
            parameter->sendValueChangedMessageToListeners(parameter->getValue());
    return true;
}

void ZederiaProcessor::setRackLocked(bool locked) { engine_.setRackLocked(locked); }

std::string ZederiaProcessor::rackPresetJson() {
    const juce::ScopedLock lock(documentLock_);
    syncDocumentValues();
    return zed::saveRackPreset(document_);
}

bool ZederiaProcessor::loadRackPresetJson(std::string_view json, std::vector<zed::Issue>& issues) {
    {
        const juce::ScopedLock lock(documentLock_);
        syncDocumentValues();
        zed::Patch edited = document_;
        if (!zed::loadRackPreset(json, edited, issues))
            return false;
        document_ = edited;
        publishSlotTypes();
        engine_.setRack(document_); // a new rack: every effect is new
        engine_.setRoutes(document_.routes);
        const auto& reg = zed::Registry::get();
        for (const auto& module : reg.modules())
            if (module.kind == zed::ModuleKind::FxRack)
                for (int i = module.firstParam; i < module.firstParam + module.numParams; ++i)
                    engine_.params().set(i, document_.get(i));
        generation_.fetch_add(1, std::memory_order_relaxed);
    }
    triggerAsyncUpdate();
    return true;
}

std::string ZederiaProcessor::slotPresetJson(int lane, int slot) {
    const juce::ScopedLock lock(documentLock_);
    syncDocumentValues();
    return zed::saveSlotPreset(document_, lane, slot);
}

bool ZederiaProcessor::loadSlotPresetJson(std::string_view json, int lane, int position, std::vector<zed::Issue>& issues) {
    const juce::ScopedLock lock(documentLock_);
    syncDocumentValues();
    zed::Patch edited = document_;
    zed::FxMoves moves;
    if (!zed::loadSlotPreset(json, edited, lane, position, &moves, issues))
        return false;
    document_ = edited;
    rackEdited(moves);
    return true;
}

// --- Clips -------------------------------------------------------------------------------------------

std::string ZederiaProcessor::importClip(int slot, std::span<const std::uint8_t> midiFile) {
    if (slot < 0 || slot >= zed::kNumClips)
        return "there is no such clip slot";
    zed::Clip clip;
    std::string error;
    if (!zed::midi::clipFromFile(midiFile, clip, error))
        return error;
    {
        const juce::ScopedLock lock(documentLock_);
        document_.clips[static_cast<std::size_t>(slot)] = std::move(clip);
        engine_.setPlayData(document_);
    }
    generation_.fetch_add(1, std::memory_order_relaxed);
    return {};
}

std::vector<std::uint8_t> ZederiaProcessor::exportClip(int slot) {
    const juce::ScopedLock lock(documentLock_);
    if (slot < 0 || slot >= zed::kNumClips)
        return {};
    return zed::midi::clipToFile(document_.clips[static_cast<std::size_t>(slot)]);
}

zed::Clip ZederiaProcessor::clip(int slot) const {
    const juce::ScopedLock lock(documentLock_);
    return slot >= 0 && slot < zed::kNumClips ? document_.clips[static_cast<std::size_t>(slot)] : zed::Clip{};
}

// --- MIDI learn, programs, message-thread service ------------------------------------------------------

void ZederiaProcessor::startMidiLearn(int paramIndex) {
    learned_.store(-1, std::memory_order_relaxed);
    learnTarget_.store(hostParameter(paramIndex) != nullptr ? paramIndex : -1, std::memory_order_relaxed);
}

zed::CcMap ZederiaProcessor::ccMap() const {
    const juce::ScopedLock lock(documentLock_);
    return ccMap_;
}

void ZederiaProcessor::setCcMap(const zed::CcMap& map) {
    const juce::ScopedLock lock(documentLock_);
    ccMap_ = map;
    ccTable_.set(std::make_shared<const zed::CcTable>(zed::CcTable::build(ccMap_)));
}

void ZederiaProcessor::setProgramList(std::vector<std::string> presetFiles) {
    const juce::ScopedLock lock(documentLock_);
    programs_ = std::move(presetFiles);
    currentProgram_ = 0;
}

std::vector<std::string> ZederiaProcessor::programList() const {
    const juce::ScopedLock lock(documentLock_);
    return programs_;
}

int ZederiaProcessor::getNumPrograms() {
    const juce::ScopedLock lock(documentLock_);
    return std::max(static_cast<int>(programs_.size()), 1);
}

int ZederiaProcessor::getCurrentProgram() {
    const juce::ScopedLock lock(documentLock_);
    return currentProgram_;
}

const juce::String ZederiaProcessor::getProgramName(int index) {
    const juce::ScopedLock lock(documentLock_);
    if (index < 0 || index >= static_cast<int>(programs_.size()))
        return index == 0 ? "Default" : "";
    return juce::File(juce::String::fromUTF8(programs_[static_cast<std::size_t>(index)].c_str())).getFileNameWithoutExtension();
}

void ZederiaProcessor::setCurrentProgram(int index) {
    std::string file;
    {
        const juce::ScopedLock lock(documentLock_);
        if (index < 0 || index >= static_cast<int>(programs_.size()))
            return;
        currentProgram_ = index;
        file = programs_[static_cast<std::size_t>(index)];
    }
    loadProgramFile(file);
}

bool ZederiaProcessor::loadProgramFile(const std::string& file) {
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!zed::readFile(file, bytes, error))
        return false;
    return loadPatchJson(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())).ok();
}

void ZederiaProcessor::serviceMessageThread() {
    // MIDI learn: the controller that arrived is bound to the waiting parameter.
    const int learned = learned_.exchange(-1, std::memory_order_relaxed);
    if (learned >= 0) {
        const int target = learnTarget_.exchange(-1, std::memory_order_relaxed);
        if (target >= 0) {
            zed::CcMap map = ccMap();
            map.unbindParam(zed::Registry::get().at(target).id);
            zed::CcBinding binding;
            binding.controller = learned & 0x7f;
            binding.param = zed::Registry::get().at(target).id;
            map.bind(binding);
            setCcMap(map);
        }
    }
    // Values that controllers moved: the host hears about them here, off the audio thread.
    const auto scope = notifyFifo_.read(notifyFifo_.getNumReady());
    const auto tell = [&](int start, int size) {
        for (int i = 0; i < size; ++i) {
            const int index = notifyParams_[static_cast<std::size_t>(start + i)];
            if (auto* parameter = hostParameter(index))
                parameter->sendValueChangedMessageToListeners(parameter->getValue());
        }
    };
    tell(scope.startIndex1, scope.blockSize1);
    tell(scope.startIndex2, scope.blockSize2);
    // A program change from the host's MIDI.
    const int program = pendingProgram_.exchange(-1, std::memory_order_relaxed);
    if (program >= 0)
        setCurrentProgram(program);
    engine_.collectGarbage();
}

void ZederiaProcessor::getStateInformation(juce::MemoryBlock& destination) {
    engine_.collectGarbage(); // a convenient moment off the audio thread to free replaced tables
    std::string payload = patchJson();
    {
        // The instrument's own settings: they belong to this instance, not to the sound.
        nlohmann::ordered_json instrument = nlohmann::ordered_json::object();
        instrument["cc_map"] = nlohmann::ordered_json::parse(zed::saveCcMap(ccMap()));
        instrument["programs"] = programList();
        instrument["rack_locked"] = engine_.rackLocked();
        instrument["tuning_locked"] = engine_.tuningLocked();
        // Host parameters the patch has no use for (the amount of a route that does not exist, the
        // values of an empty rack slot): the patch keeps them at their defaults, but a host expects
        // back exactly what it set, so they travel here.
        nlohmann::ordered_json unused = nlohmann::ordered_json::object();
        {
            const juce::ScopedLock lock(documentLock_);
            const auto& reg = zed::Registry::get();
            for (int i = 0; i < reg.size(); ++i) {
                const double value = engine_.params().get(i);
                if (hostParameter(i) != nullptr && !document_.inUse(i) && value != reg.at(i).def)
                    unused[reg.at(i).id] = value;
            }
        }
        if (!unused.empty())
            instrument["unused_values"] = std::move(unused);
        payload.push_back('\0');
        payload += instrument.dump();
    }
    juce::MemoryOutputStream out(destination, false);
    out.write(kStateMagic, 4);
    writeU32(out, kStateVersion);
    writeU32(out, static_cast<std::uint32_t>(payload.size()));
    {
        juce::GZIPCompressorOutputStream deflate(out, 6);
        deflate.write(payload.data(), payload.size());
    }
    out.flush();
}

void ZederiaProcessor::setStateInformation(const void* data, int size) {
    // Whatever arrives, never crash and never end up half-loaded: anything unreadable leaves the
    // current state as it is.
    if (data == nullptr || size < static_cast<int>(kStateHeaderBytes) || std::memcmp(data, kStateMagic, 4) != 0)
        return;
    const auto* bytes = static_cast<const char*>(data);
    const std::uint32_t version = readU32(bytes + 4);
    if (version != 1 && version != 2)
        return;
    const std::size_t length = readU32(bytes + 8);
    if (length == 0 || length > zed::kMaxPatchBytes)
        return;
    juce::MemoryInputStream packed(bytes + kStateHeaderBytes, static_cast<std::size_t>(size) - kStateHeaderBytes, false);
    juce::GZIPDecompressorInputStream inflate(packed);
    std::string payload(length, '\0');
    const auto got = inflate.read(payload.data(), static_cast<int>(length));
    if (got != static_cast<int>(length))
        return;
    std::string_view json(payload);
    std::string_view instrument;
    if (version == 2) {
        const auto split = payload.find('\0');
        if (split == std::string::npos)
            return;
        json = std::string_view(payload).substr(0, split);
        instrument = std::string_view(payload).substr(split + 1);
    }
    // A restored project brings its own rack and tuning, whatever this instance had locked.
    const bool rackWasLocked = engine_.rackLocked(), tuningWasLocked = engine_.tuningLocked();
    engine_.setRackLocked(false);
    engine_.setTuningLocked(false);
    if (!loadPatchJson(json).ok()) {
        engine_.setRackLocked(rackWasLocked);
        engine_.setTuningLocked(tuningWasLocked);
        return;
    }
    if (instrument.empty())
        return;
    // The instrument's settings: anything unreadable is skipped, the patch stays loaded.
    try {
        const auto doc = nlohmann::ordered_json::parse(instrument);
        if (!doc.is_object())
            return;
        if (doc.contains("cc_map")) {
            zed::CcMap map;
            std::vector<zed::Issue> issues;
            if (zed::loadCcMap(doc["cc_map"].dump(), map, issues))
                setCcMap(map);
        }
        if (doc.contains("programs") && doc["programs"].is_array()) {
            std::vector<std::string> files;
            for (const auto& f : doc["programs"])
                if (f.is_string() && files.size() < 128)
                    files.push_back(f.get<std::string>());
            setProgramList(std::move(files));
        }
        if (doc.contains("rack_locked") && doc["rack_locked"].is_boolean())
            engine_.setRackLocked(doc["rack_locked"].get<bool>());
        if (doc.contains("tuning_locked") && doc["tuning_locked"].is_boolean())
            engine_.setTuningLocked(doc["tuning_locked"].get<bool>());
        if (doc.contains("unused_values") && doc["unused_values"].is_object()) {
            const juce::ScopedLock lock(documentLock_);
            const auto& reg = zed::Registry::get();
            for (const auto& [id, value] : doc["unused_values"].items()) {
                const int index = reg.indexOf(id);
                if (index < 0 || hostParameter(index) == nullptr || document_.inUse(index) || !value.is_number())
                    continue;
                const auto& def = effectiveDef(index);
                engine_.params().set(index, std::clamp(value.get<double>(), def.min, def.max));
            }
        }
    } catch (const std::exception&) {
    }
}

// --- On-screen keyboard --------------------------------------------------------------------------

void ZederiaProcessor::pushKeyboardEvent(const zed::MidiEvent& event) {
    const auto scope = keyboardFifo_.write(1);
    if (scope.blockSize1 > 0)
        keyboardEvents_[static_cast<std::size_t>(scope.startIndex1)] = event;
    else if (scope.blockSize2 > 0)
        keyboardEvents_[static_cast<std::size_t>(scope.startIndex2)] = event;
}

void ZederiaProcessor::handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) {
    pushKeyboardEvent(zed::MidiEvent::noteOn(0, note, velocity, channel - 1));
}

void ZederiaProcessor::handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float velocity) {
    pushKeyboardEvent(zed::MidiEvent::noteOff(0, note, velocity, channel - 1));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ZederiaProcessor(); }
