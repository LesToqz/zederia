// The processor's side of editing (milestone 5): whole-state snapshots for undo, A/B and presets,
// locks, the workshop's table builds, and what the interface reads (output, CPU, audition).
#include "ProcessorParts.h"
#include "Session.h"

#include "zederia/engine/OscKernel.h"
#include "zederia/table/TableData.h"

#include <algorithm>

// --- Table builder ---------------------------------------------------------------------------------

ZederiaProcessor::TableBuilder::TableBuilder(ZederiaProcessor& owner) : juce::Thread("Zederia table builder"), owner_(owner) {
    startThread(juce::Thread::Priority::normal);
}

ZederiaProcessor::TableBuilder::~TableBuilder() {
    signalThreadShouldExit();
    wake_.signal();
    stopThread(4000);
}

void ZederiaProcessor::TableBuilder::request(int osc, std::shared_ptr<const zed::Wavetable> table) {
    {
        const juce::ScopedLock lock(lock_);
        auto& r = pending_[static_cast<std::size_t>(osc)];
        r.table = std::move(table);
        r.when = std::chrono::steady_clock::now();
        r.generation = ++generation_[static_cast<std::size_t>(osc)];
    }
    owner_.tableLatencyMs_.store(-1.0, std::memory_order_relaxed);
    wake_.signal();
}

void ZederiaProcessor::TableBuilder::cancel(int osc) {
    const juce::ScopedLock lock(lock_);
    pending_[static_cast<std::size_t>(osc)].table.reset();
    ++generation_[static_cast<std::size_t>(osc)];
}

bool ZederiaProcessor::TableBuilder::busy() const {
    const juce::ScopedLock lock(lock_);
    if (inFlight_ > 0)
        return true;
    for (const auto& r : pending_)
        if (r.table != nullptr)
            return true;
    return false;
}

void ZederiaProcessor::TableBuilder::run() {
    while (!threadShouldExit()) {
        wake_.wait(200);
        for (int osc = 0; osc < zed::kNumOscs && !threadShouldExit(); ++osc) {
            Request job;
            {
                const juce::ScopedLock lock(lock_);
                job = std::exchange(pending_[static_cast<std::size_t>(osc)], Request{});
                if (job.table != nullptr)
                    ++inFlight_;
            }
            if (job.table == nullptr)
                continue;
            const auto previous = owner_.engine_.table(osc);
            auto built = zed::TableData::build(*job.table, previous.get());
            const juce::ScopedLock lock(lock_);
            --inFlight_;
            if (job.generation != generation_[static_cast<std::size_t>(osc)])
                continue; // a newer request or a restore took over
            owner_.engine_.setTable(osc, std::move(built));
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job.when).count();
            owner_.tableLatencyMs_.store(ms, std::memory_order_relaxed);
        }
    }
}

// --- Snapshots and restoring ---------------------------------------------------------------------

zed::Patch ZederiaProcessor::snapshot() {
    const juce::ScopedLock lock(documentLock_);
    syncDocumentValues();
    return document_;
}

void ZederiaProcessor::restore(const zed::Patch& state) { applyLoaded(state, false); }

void ZederiaProcessor::loadPresetPatch(const zed::Patch& patch) { applyLoaded(patch, true); }

void ZederiaProcessor::applyLoaded(const zed::Patch& incoming, bool honourLocks) {
    const auto& reg = zed::Registry::get();
    bool layoutChanged = false, structure = false;
    std::vector<int> changed;
    {
        const juce::ScopedLock lock(documentLock_);
        syncDocumentValues();
        zed::Patch next = incoming;
        if (honourLocks) {
            if (engine_.rackLocked())
                next.copyRackFrom(document_);
            for (int i = 0; i < reg.size(); ++i)
                if (locked_[static_cast<std::size_t>(i)])
                    next.set(i, document_.get(i));
            if (engine_.tuningLocked())
                next.tuning = document_.tuning;
        }
        // Tables and the texture: only what differs is loaded.
        for (int osc = 0; osc < zed::kNumOscs; ++osc) {
            const auto k = static_cast<std::size_t>(osc);
            if (next.tables[k] == document_.tables[k] && next.tableData[k] == document_.tableData[k])
                continue;
            tableBuilder_->cancel(osc);
            if (next.tableData[k]) {
                engine_.setTable(osc, zed::TableData::build(*next.tableData[k].table, engine_.table(osc).get()));
            } else {
                std::string error;
                auto table = library_.loadTable(next.tables[k], error);
                engine_.setTable(osc, std::move(table));
            }
        }
        if (next.textureSample != document_.textureSample) {
            std::string error;
            engine_.setTexture(library_.loadTexture(next.textureSample, error));
        }
        // Structure that the engine takes as a whole.
        structure = next.routes != document_.routes || next.lfoShapes != document_.lfoShapes ||
                    next.envShapes != document_.envShapes || next.envSync != document_.envSync ||
                    next.warpCurves != document_.warpCurves || next.tuning != document_.tuning ||
                    next.fxCurves != document_.fxCurves || next.arpPatterns != document_.arpPatterns ||
                    next.chordNotes != document_.chordNotes || next.clips != document_.clips;
        for (int lane = 0; lane < zed::kNumFxLanes; ++lane) {
            if (next.fxSlotCount(lane) != document_.fxSlotCount(lane))
                layoutChanged = true;
            for (int slot = 0; slot < zed::kNumFxSlots; ++slot)
                layoutChanged = layoutChanged || next.fxSlotType(lane, slot) != document_.fxSlotType(lane, slot);
        }
        structure = structure || layoutChanged;
        for (int i = 0; i < reg.size(); ++i)
            if (next.inUse(i) && next.get(i) != engine_.params().get(i))
                changed.push_back(i);
        document_ = std::move(next);
        publishSlotTypes();
        if (structure) {
            const bool rackWas = engine_.rackLocked(), tuningWas = engine_.tuningLocked();
            if (!honourLocks) {
                engine_.setRackLocked(false);
                engine_.setTuningLocked(false);
            }
            engine_.loadPatch(document_);
            engine_.setRackLocked(rackWas);
            engine_.setTuningLocked(tuningWas);
        } else {
            for (const int i : changed)
                engine_.params().set(i, document_.get(i));
        }
    }
    if (structure) {
        for (auto* parameter : hostParams_)
            if (parameter != nullptr)
                parameter->sendValueChangedMessageToListeners(parameter->getValue());
    } else {
        for (const int i : changed)
            if (auto* parameter = hostParameter(i))
                parameter->sendValueChangedMessageToListeners(parameter->getValue());
    }
    generation_.fetch_add(1, std::memory_order_relaxed);
    if (layoutChanged)
        triggerAsyncUpdate();
}

void ZederiaProcessor::setTableLive(int osc, std::shared_ptr<const zed::Wavetable> table) {
    if (osc < 0 || osc >= zed::kNumOscs || table == nullptr)
        return;
    {
        const juce::ScopedLock lock(documentLock_);
        document_.tableData[static_cast<std::size_t>(osc)].table = table;
    }
    tableBuilder_->request(osc, std::move(table));
    generation_.fetch_add(1, std::memory_order_relaxed);
}

bool ZederiaProcessor::tableBuildPending() const { return tableBuilder_->busy(); }

std::shared_ptr<const zed::Wavetable> ZederiaProcessor::editableTable(int osc) {
    if (osc < 0 || osc >= zed::kNumOscs)
        return nullptr;
    std::string reference;
    zed::ContentLibrary library;
    {
        const juce::ScopedLock lock(documentLock_);
        if (const auto& data = document_.tableData[static_cast<std::size_t>(osc)])
            return data.table;
        reference = document_.tables[static_cast<std::size_t>(osc)];
        library = library_;
    }
    if (!reference.empty()) {
        std::vector<std::uint8_t> bytes;
        std::string error;
        auto table = std::make_shared<zed::Wavetable>();
        const std::string path = library.tablePath(reference);
        if (!path.empty() && zed::readFile(path, bytes, error) && zed::decodeWavetable(bytes, *table, error))
            return table;
    }
    static const auto builtin = std::make_shared<const zed::Wavetable>(zed::TableData::builtinWavetable());
    return builtin;
}

// --- Locks -------------------------------------------------------------------------------------------

void ZederiaProcessor::setParamLocked(int index, bool locked) {
    const juce::ScopedLock lock(documentLock_);
    if (index >= 0 && index < static_cast<int>(locked_.size()))
        locked_[static_cast<std::size_t>(index)] = locked;
}

bool ZederiaProcessor::paramLocked(int index) const {
    const juce::ScopedLock lock(documentLock_);
    return index >= 0 && index < static_cast<int>(locked_.size()) && locked_[static_cast<std::size_t>(index)];
}

std::vector<bool> ZederiaProcessor::lockMask() const {
    const juce::ScopedLock lock(documentLock_);
    return locked_;
}

// --- What the interface reads ------------------------------------------------------------------------

int ZederiaProcessor::readOutput(float* dest, int count) const {
    count = std::clamp(count, 0, kOutputRing);
    const int write = outputWrite_.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
        dest[i] = outputRing_[static_cast<std::size_t>((write - count + i + kOutputRing) & (kOutputRing - 1))];
    return count;
}

void ZederiaProcessor::audition(const std::vector<zed::NoteSpec>& notes) { audition(notes, 1.0f); }

void ZederiaProcessor::audition(const std::vector<zed::NoteSpec>& notes, float capGain) {
    auto plan = std::make_shared<AuditionPlan>();
    plan->capGain = std::clamp(capGain, 0.0f, 1.0f);
    double end = 0.0;
    for (const auto& n : notes)
        end = std::max(end, n.start + n.duration);
    plan->capUntil = static_cast<std::int64_t>((end + 2.0) * sampleRate_);
    for (const auto& n : notes) {
        const auto at = [&](double seconds) { return static_cast<int>(std::lround(std::max(0.0, seconds) * sampleRate_)); };
        plan->events.push_back(zed::MidiEvent::noteOn(at(n.start), n.note, static_cast<float>(n.velocity)));
        plan->events.push_back(zed::MidiEvent::noteOff(at(n.start + n.duration), n.note));
    }
    std::stable_sort(plan->events.begin(), plan->events.end(), [](const zed::MidiEvent& a, const zed::MidiEvent& b) {
        return a.offset < b.offset || (a.offset == b.offset && a.type == zed::MidiEvent::Type::NoteOff && b.type != a.type);
    });
    auditionLeft_.store(static_cast<std::int64_t>(plan->events.size()), std::memory_order_relaxed);
    auditionPlan_.set(std::move(plan));
}

void ZederiaProcessor::stopAudition() {
    auto plan = std::make_shared<AuditionPlan>();
    if (const auto playing = auditionPlan_.latest())
        for (const auto& e : playing->events)
            if (e.type == zed::MidiEvent::Type::NoteOn)
                plan->events.push_back(zed::MidiEvent::noteOff(0, e.number));
    auditionLeft_.store(static_cast<std::int64_t>(plan->events.size()), std::memory_order_relaxed);
    auditionPlan_.set(std::move(plan));
}

std::string ZederiaProcessor::uiState() const {
    const juce::ScopedLock lock(documentLock_);
    return uiState_;
}

void ZederiaProcessor::setUiState(std::string state) {
    const juce::ScopedLock lock(documentLock_);
    uiState_ = std::move(state);
}

zed::PatchMeta ZederiaProcessor::meta() const {
    const juce::ScopedLock lock(documentLock_);
    return document_.meta;
}

void ZederiaProcessor::setMeta(const zed::PatchMeta& meta) {
    {
        const juce::ScopedLock lock(documentLock_);
        document_.meta = meta;
    }
    generation_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<zed::ModRoute> ZederiaProcessor::routes() const {
    const juce::ScopedLock lock(documentLock_);
    return document_.routes;
}

std::array<std::string, zed::kNumMacros> ZederiaProcessor::macroNames() const {
    const juce::ScopedLock lock(documentLock_);
    return document_.macroNames;
}

// --- Structure: reading and live edits ---------------------------------------------------------------

void ZederiaProcessor::readDocument(const std::function<void(const zed::Patch&)>& reader) const {
    const juce::ScopedLock lock(documentLock_);
    reader(document_);
}

void ZederiaProcessor::setLfoShapeLive(int lfo, const zed::Shape& shape) {
    if (lfo < 0 || lfo >= zed::kNumLfos)
        return;
    {
        const juce::ScopedLock lock(documentLock_);
        document_.lfoShapes[static_cast<std::size_t>(lfo)] = shape;
    }
    engine_.setLfoShape(lfo, shape);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void ZederiaProcessor::setEnvShapeLive(int env, const zed::EnvShape& shape) {
    if (env < 0 || env >= zed::kNumEnvs)
        return;
    {
        const juce::ScopedLock lock(documentLock_);
        document_.envShapes[static_cast<std::size_t>(env)] = shape;
    }
    engine_.setEnvShape(env, shape);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void ZederiaProcessor::setWarpCurveLive(int osc, int slot, const zed::Shape& shape) {
    if (osc < 0 || osc >= zed::kNumOscs || slot < 0 || slot > 1)
        return;
    {
        const juce::ScopedLock lock(documentLock_);
        document_.warpCurves[static_cast<std::size_t>(osc)][static_cast<std::size_t>(slot)] = shape;
    }
    if (shape.empty()) {
        engine_.setWarpCurve(osc, slot, {});
    } else {
        std::array<float, zed::osc::kCurvePoints + 1> values{};
        zed::compileShape(shape, values.data(), zed::osc::kCurvePoints);
        engine_.setWarpCurve(osc, slot, values);
    }
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void ZederiaProcessor::setFxCurveLive(int lane, int slot, int curve, const zed::Shape& shape) {
    if (lane < 0 || lane >= zed::kNumFxLanes || slot < 0 || slot >= zed::kNumFxSlots || curve < 0 || curve > 1)
        return;
    const juce::ScopedLock lock(documentLock_);
    syncDocumentValues();
    document_.fxCurves[static_cast<std::size_t>(lane)][static_cast<std::size_t>(slot)][static_cast<std::size_t>(curve)] = shape;
    engine_.setRack(document_); // same effects in the same slots: they continue, with the new curve
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void ZederiaProcessor::setRouteCurveLive(int route, const zed::Shape& shape) {
    const juce::ScopedLock lock(documentLock_);
    if (route < 0 || route >= static_cast<int>(document_.routes.size()))
        return;
    auto& r = document_.routes[static_cast<std::size_t>(route)];
    r.curve = zed::ModCurve::Drawn;
    r.curveShape = shape;
    engine_.setRoutes(document_.routes);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void ZederiaProcessor::setArpPatternLive(int pattern, const zed::ArpPattern& steps) {
    const juce::ScopedLock lock(documentLock_);
    if (pattern < 0 || pattern >= zed::kNumArpPatterns)
        return;
    document_.arpPatterns[static_cast<std::size_t>(pattern)] = steps;
    engine_.setPlayData(document_);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void ZederiaProcessor::setClipLive(int slot, const zed::Clip& clip) {
    const juce::ScopedLock lock(documentLock_);
    if (slot < 0 || slot >= zed::kNumClips)
        return;
    document_.clips[static_cast<std::size_t>(slot)] = clip;
    engine_.setPlayData(document_);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

void ZederiaProcessor::setChordNotesLive(const std::vector<int>& notes) {
    const juce::ScopedLock lock(documentLock_);
    document_.chordNotes = notes;
    engine_.setPlayData(document_);
    generation_.fetch_add(1, std::memory_order_relaxed);
}
