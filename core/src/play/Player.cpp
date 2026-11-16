#include "zederia/play/Player.h"

#include "zederia/engine/Engine.h"

#include <algorithm>
#include <cmath>

namespace zed::play {
namespace {

constexpr std::uint16_t kScales[] = {
    0b101010110101, // major: 0 2 4 5 7 9 11
    0b010110101101, // minor: 0 2 3 5 7 8 10
    0b011010101101, // dorian: 0 2 3 5 7 9 10
    0b010110101011, // phrygian: 0 1 3 5 7 8 10
    0b101011010101, // lydian: 0 2 4 6 7 9 11
    0b011010110101, // mixolydian: 0 2 4 5 7 9 10
    0b010101101011, // locrian: 0 1 3 5 6 8 10
    0b100110101101, // harmonic minor: 0 2 3 5 7 8 11
    0b101010101101, // melodic minor: 0 2 3 5 7 9 11
    0b001010010101, // major pentatonic: 0 2 4 7 9
    0b010010101001, // minor pentatonic: 0 3 5 7 10
    0b010011101001, // blues: 0 3 5 6 7 10
    0b010101010101, // whole tone: 0 2 4 6 8 10
    0b111111111111, // chromatic
};

constexpr std::uint8_t kChordStage = 0, kArp = 1, kClip = 2;

bool isNote(const MidiEvent& e) { return e.type == MidiEvent::Type::NoteOn || e.type == MidiEvent::Type::NoteOff; }

} // namespace

std::uint16_t scaleMask(int scale) noexcept {
    return scale >= 0 && scale < static_cast<int>(std::size(kScales)) ? kScales[scale] : kScales[0];
}

int snapToScale(int key, int root, std::uint16_t mask) noexcept {
    const auto in = [&](int k) { return ((mask >> (((k - root) % 12 + 12) % 12)) & 1u) != 0; };
    if (in(key))
        return key;
    for (int d = 1; d <= 6; ++d) {
        if (in(key - d) && key - d >= 0)
            return key - d;
        if (in(key + d) && key + d <= 127)
            return key + d;
    }
    return key;
}

std::shared_ptr<const PlayData> PlayData::fromPatch(const Patch& patch) {
    auto data = std::make_shared<PlayData>();
    for (int p = 0; p < kNumArpPatterns; ++p)
        for (int i = 0; i < kMaxArpSteps; ++i)
            data->steps[p][i] = patch.arpPatterns[static_cast<std::size_t>(p)].step(i);
    data->chordCount = 0;
    for (const int semis : patch.chordNotes)
        if (data->chordCount < kMaxChordNotes)
            data->chord[data->chordCount++] = std::clamp(semis, -48, 48);
    if (data->chordCount == 0)
        data->chord[data->chordCount++] = 0;
    for (int c = 0; c < kNumClips; ++c) {
        const Clip& clip = patch.clips[static_cast<std::size_t>(c)];
        auto& out = data->clips[c];
        out.notes = clip.notes;
        std::stable_sort(out.notes.begin(), out.notes.end(),
                         [](const ClipNote& a, const ClipNote& b) { return a.start < b.start; });
        out.lengthBeats = std::max(clip.lengthBeats(), 1.0 / 16.0);
        out.root = clip.root;
    }
    return data;
}

void Player::prepare(double sampleRate) { sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0; }

void Player::reset(std::uint64_t seed) {
    rng_ = seed * 0x2545F4914F6CDD1Dull + 0x9E3779B97F4A7C15ull;
    now_ = 0;
    pendingCount_ = 0;
    outCount_ = seq_ = 0;
    std::fill(&mapped_[0][0], &mapped_[0][0] + 128 * kMaxChordNotes, std::uint8_t{0});
    std::fill(std::begin(mappedCount_), std::end(mappedCount_), std::uint8_t{0});
    std::fill(std::begin(refs_), std::end(refs_), std::uint8_t{0});
    std::fill(std::begin(direct_), std::end(direct_), std::uint8_t{0});
    strumUp_ = true;
    mappedKeys_ = 0;
    heldCount_ = physical_ = 0;
    order_ = 0;
    seqCount_ = 0;
    seqDirty_ = true;
    running_ = wasArp_ = false;
    cursor_ = laneStep_ = chordStep_ = 0;
    walkDir_ = 1;
    nextStepAt_ = stepCounter_ = 0;
    lastGridIndex_ = -1.0;
    startNow_ = false;
    soundingCount_ = 0;
    lastStepCount_ = 0;
    clipRunning_ = false;
    clipKey_ = -1;
    clipKeysDown_ = 0;
    transportWasPlaying_ = false;
}

std::uint32_t Player::random() noexcept {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return static_cast<std::uint32_t>(rng_ >> 32);
}

// --- Output ---------------------------------------------------------------------------------------

void Player::emit(const MidiEvent& e, std::int64_t at, std::uint8_t origin) noexcept {
    if (outCount_ >= kMaxEvents)
        return;
    OutEvent& o = outBuf_[outCount_++];
    o.event = e;
    o.event.offset = static_cast<int>(std::max<std::int64_t>(at - now_, 0));
    o.seq = seq_++;
    o.generated = isNote(e) && (shaping_ || origin != kChordStage);
}

void Player::schedule(const MidiEvent& e, std::int64_t at, std::uint8_t origin, std::uint8_t inputKey) noexcept {
    if (pendingCount_ >= kMaxPending) { // never lose a note-off: play it now instead
        if (e.type == MidiEvent::Type::NoteOff)
            emit(e, now_, origin);
        return;
    }
    Timed& t = pending_[pendingCount_++];
    t.at = at;
    t.event = e;
    t.origin = origin;
    t.inputKey = inputKey;
}

void Player::startNote(int key, float velocity, std::int64_t at, std::int64_t offAt, std::uint8_t origin) noexcept {
    // The same key still sounding from before: it ends where the new one starts.
    for (int i = 0; i < soundingCount_; ++i) {
        if (sounding_[i].key != key || sounding_[i].origin != origin)
            continue;
        for (int p = 0; p < pendingCount_; ++p) {
            if (pending_[p].origin == origin && pending_[p].event.type == MidiEvent::Type::NoteOff && pending_[p].event.number == key) {
                pending_[p].at = std::min(pending_[p].at, at);
                break;
            }
        }
    }
    const MidiEvent on = MidiEvent::noteOn(0, key, std::clamp(velocity, 1.0f / 127.0f, 1.0f));
    at = std::max(at, now_);
    schedule(on, at, origin);
    schedule(MidiEvent::noteOff(0, key), std::max(offAt, at + 1), origin);
    if (soundingCount_ < static_cast<int>(std::size(sounding_)))
        sounding_[soundingCount_++] = {key, offAt, origin};
}

void Player::stopSounding(std::uint8_t origin, std::int64_t at) noexcept {
    // Every note-off this origin still owes is due now; its note-ons that have not happened are dropped.
    int kept = 0;
    for (int p = 0; p < pendingCount_; ++p) {
        Timed& t = pending_[p];
        if (t.origin == origin) {
            if (t.event.type == MidiEvent::Type::NoteOff)
                emit(t.event, std::max(at, now_), origin);
            continue;
        }
        pending_[kept++] = t;
    }
    pendingCount_ = kept;
    kept = 0;
    for (int i = 0; i < soundingCount_; ++i)
        if (sounding_[i].origin != origin)
            sounding_[kept++] = sounding_[i];
    soundingCount_ = kept;
}

void Player::allOff(std::int64_t at) noexcept {
    stopSounding(kArp, at);
    stopSounding(kClip, at);
    pendingCount_ = 0;
    std::fill(&mapped_[0][0], &mapped_[0][0] + 128 * kMaxChordNotes, std::uint8_t{0});
    std::fill(std::begin(mappedCount_), std::end(mappedCount_), std::uint8_t{0});
    std::fill(std::begin(refs_), std::end(refs_), std::uint8_t{0});
    mappedKeys_ = 0;
    std::fill(std::begin(direct_), std::end(direct_), std::uint8_t{0});
    heldCount_ = physical_ = 0;
    seqDirty_ = true;
    running_ = false;
    clipRunning_ = false;
    clipKeysDown_ = 0;
}

// --- Chord stage ------------------------------------------------------------------------------------

void Player::stageOn(int key, float velocity, std::int64_t at) noexcept {
    if (s_.arpOn) {
        arpAdd(key, velocity, at);
        return;
    }
    direct_[key] = 1;
    emit(MidiEvent::noteOn(0, key, velocity), at, kChordStage);
}

void Player::stageOff(int key, std::int64_t at) noexcept {
    if (s_.arpOn) {
        arpRemove(key);
        return;
    }
    direct_[key] = 0;
    emit(MidiEvent::noteOff(0, key), at, kChordStage);
}

void Player::chordNoteOn(const MidiEvent* group, int count, std::int64_t at) noexcept {
    const std::uint16_t mask = scaleMask(s_.scale);
    struct Out {
        int key;
        float velocity;
        int input;
    } outs[128];
    int outCount = 0;
    for (int g = 0; g < count; ++g) {
        const int input = group[g].number;
        if (mappedCount_[input] > 0)
            chordNoteOff(input, at); // the same key again without a release: start it over
        const bool inScale = ((mask >> (((input - s_.key) % 12 + 12) % 12)) & 1u) != 0;
        if (s_.scaleLock && s_.filter && !inScale)
            continue;
        const int base = s_.scaleLock ? snapToScale(input, s_.key, mask) : input;
        const int notes = s_.chordMemory ? data_->chordCount : 1;
        for (int i = 0; i < notes; ++i) {
            int key = base + (s_.chordMemory ? data_->chord[i] : 0);
            if (key < 0 || key > 127)
                continue;
            if (s_.scaleLock) {
                const bool in = ((mask >> (((key - s_.key) % 12 + 12) % 12)) & 1u) != 0;
                if (!in && s_.filter)
                    continue;
                key = snapToScale(key, s_.key, mask);
            }
            bool seen = false;
            for (int m = 0; m < mappedCount_[input]; ++m)
                seen = seen || mapped_[input][m] == key;
            if (seen || mappedCount_[input] >= kMaxChordNotes)
                continue;
            if (mappedCount_[input] == 0)
                ++mappedKeys_;
            mapped_[input][mappedCount_[input]++] = static_cast<std::uint8_t>(key);
            if (refs_[key]++ == 0 && outCount < 128)
                outs[outCount++] = {key, group[g].value, input};
        }
    }
    if (outCount == 0)
        return;
    const auto strum = static_cast<std::int64_t>(s_.strumMs * 0.001 * sampleRate_);
    if (strum <= 0 || outCount == 1) {
        for (int i = 0; i < outCount; ++i)
            stageOn(outs[i].key, outs[i].velocity, at);
        return;
    }
    bool up = s_.strumDirection == 0 || (s_.strumDirection == 2 && strumUp_);
    if (s_.strumDirection == 2)
        strumUp_ = !strumUp_;
    std::sort(outs, outs + outCount, [&](const Out& a, const Out& b) { return up ? a.key < b.key : a.key > b.key; });
    for (int i = 0; i < outCount; ++i) {
        if (i == 0)
            stageOn(outs[i].key, outs[i].velocity, at);
        else
            schedule(MidiEvent::noteOn(0, outs[i].key, outs[i].velocity), at + strum * i, kChordStage,
                     static_cast<std::uint8_t>(outs[i].input));
    }
}

void Player::chordNoteOff(int input, std::int64_t at) noexcept {
    for (int m = 0; m < mappedCount_[input]; ++m) {
        const int key = mapped_[input][m];
        if (refs_[key] == 0 || --refs_[key] > 0)
            continue;
        bool cancelled = false; // a strummed note that has not started yet never starts
        for (int p = 0; p < pendingCount_ && !cancelled; ++p) {
            Timed& t = pending_[p];
            if (t.origin == kChordStage && t.event.type == MidiEvent::Type::NoteOn && t.event.number == key) {
                pending_[p] = pending_[--pendingCount_];
                cancelled = true;
            }
        }
        if (!cancelled)
            stageOff(key, at);
    }
    if (mappedCount_[input] > 0)
        --mappedKeys_;
    mappedCount_[input] = 0;
}

// --- Arpeggiator ------------------------------------------------------------------------------------

void Player::arpAdd(int key, float velocity, std::int64_t at) noexcept {
    const bool newPhrase = physical_ == 0;
    ++physical_;
    if (newPhrase && s_.latch)
        heldCount_ = 0; // a new chord after all keys were let go replaces the latched one
    bool present = false;
    for (int i = 0; i < heldCount_; ++i)
        present = present || held_[i].key == key;
    if (!present && heldCount_ < 128)
        held_[heldCount_++] = {key, velocity, order_++};
    seqDirty_ = true;
    if (newPhrase && (s_.restart || !running_)) {
        if (s_.restart) { // the pattern starts over; without Restart it goes on where it was
            cursor_ = laneStep_ = chordStep_ = 0;
            walkDir_ = 1;
        }
        startNow_ = true;
        startAt_ = at;
    }
    running_ = true;
}

void Player::arpRemove(int key) noexcept {
    physical_ = std::max(physical_ - 1, 0);
    if (s_.latch)
        return;
    int kept = 0;
    for (int i = 0; i < heldCount_; ++i)
        if (held_[i].key != key)
            held_[kept++] = held_[i];
    heldCount_ = kept;
    seqDirty_ = true;
    if (heldCount_ == 0)
        running_ = false;
}

void Player::buildSequence() noexcept {
    seqDirty_ = false;
    Held base[128];
    std::copy(held_, held_ + heldCount_, base);
    if (s_.arpMode == ArpMode::AsPlayed)
        std::sort(base, base + heldCount_, [](const Held& a, const Held& b) { return a.order < b.order; });
    else
        std::sort(base, base + heldCount_, [](const Held& a, const Held& b) { return a.key < b.key; });
    seqCount_ = 0;
    const int octaves = std::clamp(s_.octaves, 1, 4);
    for (int o = 0; o < octaves; ++o) {
        for (int i = 0; i < heldCount_ && seqCount_ < static_cast<int>(std::size(seq)); ++i) {
            seq[seqCount_] = base[i].key + 12 * o;
            seqVelocity[seqCount_++] = base[i].velocity;
        }
    }
}

void Player::arpStep(std::int64_t at, std::int64_t stepLength) noexcept {
    if (seqDirty_)
        buildSequence();
    if (seqCount_ == 0)
        return;
    const int laneLength = std::clamp(s_.steps, 1, kMaxArpSteps);
    const ArpStep step = data_->steps[std::clamp(s_.pattern, 0, kNumArpPatterns - 1)][laneStep_ % laneLength];
    laneStep_ = (laneStep_ + 1) % laneLength;
    const auto gateLength = [&](std::int64_t length) {
        return std::max<std::int64_t>(static_cast<std::int64_t>(std::llround(s_.gate * static_cast<double>(length))), 1);
    };
    if (step.state == StepState::Rest) {
        lastStepCount_ = 0;
        return;
    }
    if (step.state == StepState::Tie) // the note before holds through it (see the lookahead below)
        return;
    // Which notes this step plays.
    int keys[128];
    float velocities[128];
    int count = 0;
    const int n = seqCount_;
    if (s_.arpMode == ArpMode::Chord) {
        const int baseCount = std::max(n / std::clamp(s_.octaves, 1, 4), 1);
        const int octave = chordStep_ % std::clamp(s_.octaves, 1, 4);
        ++chordStep_;
        for (int i = 0; i < baseCount && count < 128; ++i) {
            keys[count] = seq[i] + 12 * octave;
            velocities[count++] = seqVelocity[i];
        }
    } else {
        int index = 0;
        switch (s_.arpMode) {
        case ArpMode::Up:
        case ArpMode::AsPlayed: index = cursor_ % n; break;
        case ArpMode::Down: index = n - 1 - cursor_ % n; break;
        case ArpMode::UpDown: {
            const int period = n <= 2 ? n : 2 * n - 2;
            const int c = cursor_ % period;
            index = c < n ? c : 2 * n - 2 - c;
            break;
        }
        case ArpMode::UpDownRepeat: {
            const int c = cursor_ % (2 * n);
            index = c < n ? c : 2 * n - 1 - c;
            break;
        }
        case ArpMode::Random: index = static_cast<int>(random() % static_cast<std::uint32_t>(n)); break;
        case ArpMode::Walk: index = std::clamp(cursor_, 0, n - 1); break;
        case ArpMode::Chord: break;
        }
        if (s_.arpMode == ArpMode::Walk) {
            int next = index + ((random() & 1u) != 0 ? 1 : -1);
            if (next < 0)
                next = n > 1 ? 1 : 0;
            if (next >= n)
                next = n > 1 ? n - 2 : 0;
            cursor_ = next;
        } else {
            cursor_ = (cursor_ + 1) % (2 * n * 64);
        }
        keys[0] = seq[index];
        velocities[0] = seqVelocity[index];
        count = 1;
    }
    const int ratchet = std::clamp(step.ratchet, 1, 4);
    const std::int64_t sub = std::max<std::int64_t>(stepLength / ratchet, 1);
    // Tie steps that follow keep the last note of this step sounding through them.
    int ties = 0;
    while (ties < laneLength - 1 &&
           data_->steps[std::clamp(s_.pattern, 0, kNumArpPatterns - 1)][(laneStep_ + ties) % laneLength].state == StepState::Tie)
        ++ties;
    lastStepCount_ = 0;
    for (int i = 0; i < count; ++i) {
        const int key = keys[i] + step.transpose;
        const float velocity = velocities[i] * static_cast<float>(step.velocity);
        if (key < 0 || key > 127 || velocity <= 0.0f)
            continue;
        for (int r = 0; r < ratchet; ++r) {
            const std::int64_t on = at + sub * r;
            const std::int64_t held = r == ratchet - 1 ? stepLength * ties : 0;
            startNote(key, velocity, on, on + held + gateLength(sub), kArp);
        }
        if (lastStepCount_ < static_cast<int>(std::size(lastStepNotes_)))
            lastStepNotes_[lastStepCount_++] = key;
    }
}

// Steps between `a` and `b` (absolute samples) of this block.
void Player::runArp(const Transport& transport, std::int64_t a, std::int64_t b) noexcept {
    if (!s_.arpOn || !running_ || heldCount_ == 0)
        return;
    const double samplesPerBeat = 60.0 / std::max(transport.bpm, 1.0) * sampleRate_;
    const double stepSamples = std::max(s_.stepBeats * samplesPerBeat, 1.0);
    const auto stepLength = static_cast<std::int64_t>(std::llround(stepSamples));
    const double swingSamples = std::clamp(s_.swing, 0.0, 1.0) * 0.5 * stepSamples;
    if (transport.playing) {
        // Steps sit on the song's grid; a phrase starts with a step at once.
        const double beatsPerSample = 1.0 / samplesPerBeat;
        if (startNow_ && startAt_ >= a && startAt_ < b) {
            startNow_ = false;
            arpStep(startAt_, stepLength);
            const double ppq = transport.ppqPosition + static_cast<double>(startAt_ - now_) * beatsPerSample;
            lastGridIndex_ = std::floor((ppq + 0.25 * s_.stepBeats) / s_.stepBeats);
        }
        const double ppqA = transport.ppqPosition + static_cast<double>(a - now_) * beatsPerSample;
        const double ppqB = transport.ppqPosition + static_cast<double>(b - now_) * beatsPerSample;
        for (double index = std::max(lastGridIndex_ + 1.0, std::floor(ppqA / s_.stepBeats) - 1.0);; index += 1.0) {
            const bool odd = std::fmod(index, 2.0) != 0.0;
            const double ppq = index * s_.stepBeats + (odd ? swingSamples * beatsPerSample : 0.0);
            if (ppq >= ppqB)
                break;
            if (ppq < ppqA - 1.0e-9)
                continue;
            const auto at = now_ + static_cast<std::int64_t>(std::ceil((ppq - transport.ppqPosition) * samplesPerBeat - 1.0e-6));
            lastGridIndex_ = index;
            arpStep(std::clamp(at, a, b - 1), stepLength);
            if (!running_ || heldCount_ == 0)
                break;
        }
        return;
    }
    // Free running from the first key.
    if (startNow_ && startAt_ >= a && startAt_ < b) {
        startNow_ = false;
        nextStepAt_ = startAt_;
        stepCounter_ = 0;
    }
    while (true) {
        const double base = static_cast<double>(nextStepAt_);
        const auto at = static_cast<std::int64_t>(std::llround(base + ((stepCounter_ & 1) != 0 ? swingSamples : 0.0)));
        if (at >= b)
            break;
        if (at >= a)
            arpStep(at, stepLength);
        nextStepAt_ += stepLength;
        ++stepCounter_;
        if (!running_ || heldCount_ == 0)
            break;
    }
}

// --- Clip player -------------------------------------------------------------------------------------

void Player::runClip(const Transport& transport, std::int64_t a, std::int64_t b) noexcept {
    if (!s_.clipOn)
        return;
    const auto& clip = data_->clips[std::clamp(s_.clipSlot, 0, kNumClips - 1)];
    if (clip.notes.empty())
        return;
    const double samplesPerBeat = 60.0 / std::max(transport.bpm, 1.0) * sampleRate_;
    const double length = clip.lengthBeats;
    double beatA = 0.0, beatB = 0.0;
    std::int64_t origin = 0; // the absolute sample where clip beat 0 of the current pass is
    int transpose = 0;
    if (s_.clipFromKey) {
        if (!clipRunning_)
            return;
        beatA = static_cast<double>(a - clipStart_) / samplesPerBeat;
        beatB = static_cast<double>(b - clipStart_) / samplesPerBeat;
        origin = clipStart_;
        transpose = clipTranspose_;
        if (!s_.clipLoop && beatA >= length) {
            clipRunning_ = false;
            return;
        }
    } else {
        if (!transport.playing)
            return;
        beatA = transport.ppqPosition + static_cast<double>(a - now_) / samplesPerBeat;
        beatB = transport.ppqPosition + static_cast<double>(b - now_) / samplesPerBeat;
        origin = now_ - static_cast<std::int64_t>(std::llround(transport.ppqPosition * samplesPerBeat));
    }
    // A note belongs to this stretch when its sample position does: a position is computed the same
    // way in every block, so a note on a block boundary plays exactly once.
    const double firstPass = std::floor(beatA / length) - 1.0, lastPass = std::floor(beatB / length) + 1.0;
    for (double pass = std::max(firstPass, 0.0); pass <= lastPass; pass += 1.0) {
        if (!s_.clipLoop && pass > 0.0)
            break;
        for (const ClipNote& note : clip.notes) {
            if (note.start >= length)
                continue;
            const double start = pass * length + note.start;
            const auto at = origin + static_cast<std::int64_t>(std::ceil(start * samplesPerBeat - 1.0e-6));
            if (at < a || at >= b)
                continue;
            const int key = note.key + transpose;
            if (key < 0 || key > 127)
                continue;
            const double end = std::min(note.start + note.length, s_.clipLoop ? note.start + note.length : length);
            const auto off = origin + static_cast<std::int64_t>(std::ceil((pass * length + end) * samplesPerBeat - 1.0e-6));
            startNote(key, static_cast<float>(note.velocity), at, off, kClip);
        }
    }
}

// --- The block ------------------------------------------------------------------------------------------

int Player::process(const Settings& settings, const PlayData& data, const Transport& transport, int frames,
                    std::span<const MidiEvent> in, MidiEvent* out, MidiEvent* generated, int generatedCapacity,
                    int& generatedCount) noexcept {
    const Settings before = s_;
    s_ = settings;
    data_ = &data;
    outCount_ = 0;
    seq_ = 0;
    shaping_ = s_.arpOn || s_.chordMemory || s_.scaleLock || s_.strumMs > 0.0 || s_.clipOn;
    const std::int64_t blockEnd = now_ + frames;

    // Switches that changed since the last block.
    if (wasArp_ && !s_.arpOn) { // the arpeggiator's notes end; keys still held stay silent
        stopSounding(kArp, now_);
        heldCount_ = physical_ = 0;
        running_ = false;
    } else if (!wasArp_ && s_.arpOn) { // keys sounding directly hand over to the arpeggiator
        for (int key = 0; key < 128; ++key) {
            if (direct_[key] == 0)
                continue;
            direct_[key] = 0;
            emit(MidiEvent::noteOff(0, key), now_, kChordStage);
            arpAdd(key, 0.8f, now_);
        }
    }
    wasArp_ = s_.arpOn;
    if (s_.arpOn && !s_.latch && before.latch && physical_ == 0) {
        heldCount_ = 0;
        running_ = false;
        seqDirty_ = true;
    }
    if (s_.arpMode != before.arpMode || s_.octaves != before.octaves)
        seqDirty_ = true;
    if ((!s_.clipOn && before.clipOn) || s_.clipSlot != before.clipSlot || s_.clipFromKey != before.clipFromKey ||
        (transportWasPlaying_ && !transport.playing && !s_.clipFromKey)) {
        stopSounding(kClip, now_);
        if (!s_.clipOn || s_.clipFromKey != before.clipFromKey)
            clipRunning_ = false;
    }
    transportWasPlaying_ = transport.playing;
    if (s_.arpOn && transport.playing) { // the host jumped back (a loop): the grid starts over there
        const double gridNow = std::floor(transport.ppqPosition / std::max(s_.stepBeats, 1.0e-6));
        if (lastGridIndex_ > gridNow + 1.0)
            lastGridIndex_ = gridNow - 1.0;
    }

    // Nothing to shape and nothing left over: the events pass unchanged (MPE channels included).
    if (!shaping_ && outCount_ == 0 && pendingCount_ == 0 && soundingCount_ == 0 && mappedKeys_ == 0) {
        std::size_t n = 0;
        for (; n < in.size() && n < static_cast<std::size_t>(kMaxEvents); ++n) {
            out[n] = in[n];
            out[n].offset = std::clamp(in[n].offset, 0, std::max(frames - 1, 0));
        }
        generatedCount = 0;
        now_ = blockEnd;
        return static_cast<int>(n);
    }

    // Walk the block in time order: steps, clip notes and scheduled events up to each incoming
    // event, then that event.
    std::int64_t from = now_;
    const auto advanceTo = [&](std::int64_t until) {
        runArp(transport, from, until);
        runClip(transport, from, until);
        while (true) { // scheduled events before `until`, earliest first
            int pick = -1;
            for (int p = 0; p < pendingCount_; ++p)
                if (pending_[p].at < until && (pick < 0 || pending_[p].at < pending_[pick].at ||
                                               (pending_[p].at == pending_[pick].at &&
                                                pending_[p].event.type == MidiEvent::Type::NoteOff &&
                                                pending_[pick].event.type != MidiEvent::Type::NoteOff)))
                    pick = p;
            if (pick < 0)
                break;
            const Timed t = pending_[pick];
            pending_[pick] = pending_[--pendingCount_];
            if (t.origin == kChordStage) {
                if (t.event.type == MidiEvent::Type::NoteOn)
                    stageOn(t.event.number, t.event.value, t.at);
                else
                    stageOff(t.event.number, t.at);
            } else {
                emit(t.event, t.at, t.origin);
                if (t.event.type == MidiEvent::Type::NoteOff) {
                    for (int i = 0; i < soundingCount_; ++i) {
                        if (sounding_[i].key == t.event.number && sounding_[i].origin == t.origin) {
                            sounding_[i] = sounding_[--soundingCount_];
                            break;
                        }
                    }
                }
            }
        }
        from = until;
    };

    std::size_t i = 0;
    while (i < in.size()) {
        const int offset = std::clamp(in[i].offset, 0, std::max(frames - 1, 0));
        const std::int64_t at = now_ + offset;
        advanceTo(at);
        // All note-ons at this moment form one group (a chord to strum).
        MidiEvent group[128];
        int groupCount = 0;
        while (i < in.size() && std::clamp(in[i].offset, 0, std::max(frames - 1, 0)) == offset) {
            const MidiEvent& e = in[i++];
            if (e.type == MidiEvent::Type::Controller && (e.number == 120 || e.number == 123)) {
                allOff(at);
                emit(e, at, kChordStage);
                continue;
            }
            if (!isNote(e)) {
                emit(e, at, kChordStage);
                continue;
            }
            const bool on = e.type == MidiEvent::Type::NoteOn;
            if (s_.clipOn && s_.clipFromKey) { // the keys start and transpose the clip
                const auto& clip = data_->clips[std::clamp(s_.clipSlot, 0, kNumClips - 1)];
                if (on) {
                    stopSounding(kClip, at); // every key starts the clip over, transposed to it
                    clipRunning_ = true;
                    clipStart_ = at;
                    clipTranspose_ = e.number - clip.root;
                    clipKey_ = e.number;
                    ++clipKeysDown_;
                } else {
                    clipKeysDown_ = std::max(clipKeysDown_ - 1, 0);
                    if (clipKeysDown_ == 0 && !s_.clipLatch) {
                        stopSounding(kClip, at);
                        clipRunning_ = false;
                    }
                }
                continue;
            }
            if (on) {
                if (groupCount < 128)
                    group[groupCount++] = e;
            } else {
                chordNoteOff(e.number, at);
            }
        }
        if (groupCount > 0)
            chordNoteOn(group, groupCount, at);
    }
    advanceTo(blockEnd);

    // Sorted by time; at the same sample a note-off comes before a note-on.
    std::sort(outBuf_, outBuf_ + outCount_, [](const OutEvent& x, const OutEvent& y) {
        if (x.event.offset != y.event.offset)
            return x.event.offset < y.event.offset;
        const bool xOff = x.event.type == MidiEvent::Type::NoteOff, yOff = y.event.type == MidiEvent::Type::NoteOff;
        if (xOff != yOff)
            return xOff;
        return x.seq < y.seq;
    });
    generatedCount = 0;
    for (int n = 0; n < outCount_; ++n) {
        out[n] = outBuf_[n].event;
        if (outBuf_[n].generated && generated != nullptr && generatedCount < generatedCapacity)
            generated[generatedCount++] = outBuf_[n].event;
    }
    now_ = blockEnd;
    return outCount_;
}

} // namespace zed::play
