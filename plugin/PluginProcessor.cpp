#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "zederia/rt/AudioThreadGuard.h"
#include "zederia/rt/Denormals.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Plug-in state in a host project (plan 5.6): a small header followed by the patch JSON, deflated.
constexpr char kStateMagic[4] = {'Z', 'E', 'D', 'S'};
constexpr std::uint32_t kStateVersion = 1;
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
    keyboard_.addListener(this);
}

ZederiaProcessor::~ZederiaProcessor() {
    keyboard_.removeListener(this);
    cancelPendingUpdate();
}

void ZederiaProcessor::prepareToPlay(double sampleRate, int) {
    engine_.prepare(sampleRate);
    setLatencySamples(engine_.latencySamples());
}

void ZederiaProcessor::reset() { engine_.reset(); }

bool ZederiaProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && layouts.getMainInputChannelSet().isDisabled();
}

void ZederiaProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    const zed::rt::AudioThreadScope audioThread;
    const zed::rt::ScopedNoDenormals noDenormals;
    const int frames = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || frames <= 0) {
        buffer.clear();
        return;
    }

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
        zed::MidiEvent event;
        const std::uint8_t data2 = metadata.numBytes == 3 ? metadata.data[2] : std::uint8_t{0};
        if (zed::midiFromBytes(metadata.data[0], metadata.data[1], data2, std::clamp(metadata.samplePosition, 0, frames - 1), event))
            blockEvents_[static_cast<std::size_t>(count++)] = event;
    }
    midi.clear();

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
    engine_.process(args);
    for (int channel = 2; channel < buffer.getNumChannels(); ++channel)
        buffer.clear(channel, 0, frames);

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

std::string ZederiaProcessor::patchJson(bool compact) {
    const juce::ScopedLock lock(documentLock_);
    for (int i = 0; i < engine_.params().size(); ++i)
        document_.set(i, engine_.params().get(i)); // values the patch has no use for are left alone
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

void ZederiaProcessor::applyPatch(const zed::Patch& patch) {
    bool layoutChanged = false;
    {
        const juce::ScopedLock lock(documentLock_);
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

void ZederiaProcessor::getStateInformation(juce::MemoryBlock& destination) {
    engine_.collectGarbage(); // a convenient moment off the audio thread to free replaced tables
    const std::string json = patchJson();
    juce::MemoryOutputStream out(destination, false);
    out.write(kStateMagic, 4);
    writeU32(out, kStateVersion);
    writeU32(out, static_cast<std::uint32_t>(json.size()));
    {
        juce::GZIPCompressorOutputStream deflate(out, 6);
        deflate.write(json.data(), json.size());
    }
    out.flush();
}

void ZederiaProcessor::setStateInformation(const void* data, int size) {
    // Whatever arrives, never crash and never end up half-loaded: anything unreadable leaves the
    // current state as it is.
    if (data == nullptr || size < static_cast<int>(kStateHeaderBytes) || std::memcmp(data, kStateMagic, 4) != 0)
        return;
    const auto* bytes = static_cast<const char*>(data);
    if (readU32(bytes + 4) != kStateVersion)
        return;
    const std::size_t length = readU32(bytes + 8);
    if (length == 0 || length > zed::kMaxPatchBytes)
        return;
    juce::MemoryInputStream packed(bytes + kStateHeaderBytes, static_cast<std::size_t>(size) - kStateHeaderBytes, false);
    juce::GZIPDecompressorInputStream inflate(packed);
    std::string json(length, '\0');
    const auto got = inflate.read(json.data(), static_cast<int>(length));
    if (got != static_cast<int>(length))
        return;
    loadPatchJson(json);
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
