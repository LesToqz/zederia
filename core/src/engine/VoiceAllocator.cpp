#include "zederia/engine/VoiceAllocator.h"

#include <algorithm>

namespace zed {

using Kind = VoiceAction::Kind;

void VoiceAllocator::reset() {
    slots_.fill(Slot{});
    heldCount_ = 0;
    monoSlot_ = -1;
    counter_ = 0;
    pedal_ = false;
}

int VoiceAllocator::sounding() const {
    int n = 0;
    for (const auto& s : slots_)
        n += (s.state == State::Held || s.state == State::Sustained || s.state == State::Releasing) ? 1 : 0;
    return n;
}

int VoiceAllocator::findFree() const {
    for (int i = 0; i < kVoiceSlots; ++i)
        if (slots_[static_cast<std::size_t>(i)].state == State::Free)
            return i;
    return -1;
}

int VoiceAllocator::oldestDying() const {
    int best = -1;
    for (int i = 0; i < kVoiceSlots; ++i) {
        const auto& s = slots_[static_cast<std::size_t>(i)];
        if (s.state == State::Dying && (best < 0 || s.age < slots_[static_cast<std::size_t>(best)].age))
            best = i;
    }
    return best;
}

int VoiceAllocator::pickVictim() const {
    const auto isSounding = [](const Slot& s) {
        return s.state == State::Held || s.state == State::Sustained || s.state == State::Releasing;
    };
    int oldest = -1, quietest = -1, quietestReleasing = -1;
    for (int i = 0; i < kVoiceSlots; ++i) {
        const auto& s = slots_[static_cast<std::size_t>(i)];
        if (!isSounding(s))
            continue;
        const auto better = [&](int current, bool byLevel) {
            if (current < 0)
                return true;
            const auto& c = slots_[static_cast<std::size_t>(current)];
            if (byLevel && s.level != c.level)
                return s.level < c.level;
            return s.age < c.age;
        };
        if (better(oldest, false)) oldest = i;
        if (better(quietest, true)) quietest = i;
        if (s.state == State::Releasing && better(quietestReleasing, true)) quietestReleasing = i;
    }
    switch (steal_) {
    case StealMode::Quietest: return quietest;
    case StealMode::Release: return quietestReleasing >= 0 ? quietestReleasing : oldest;
    case StealMode::Oldest: break;
    }
    return oldest;
}

void VoiceAllocator::start(int note, float velocity, Actions& out) {
    int slot = findFree();
    Kind kind = Kind::Start;
    if (slot < 0) {
        // Every spare slot is still fading: take over the one that has faded longest.
        slot = oldestDying();
        kind = Kind::Retrigger;
    }
    if (slot < 0)
        return;
    slots_[static_cast<std::size_t>(slot)] = {State::Held, note, ++counter_, velocity};
    out.push(kind, slot, note, velocity);
    if (mode_ != VoiceMode::Poly)
        monoSlot_ = slot;
}

void VoiceAllocator::configure(VoiceMode mode, int polyphony, StealMode steal, NotePriority priority, Actions& out) {
    polyphony = std::clamp(polyphony, 1, kMaxPolyphony);
    if (mode != mode_) {
        allNotesOff(out);
        mode_ = mode;
    }
    steal_ = steal;
    priority_ = priority;
    polyphony_ = polyphony;
    while (sounding() > polyphony_) {
        const int victim = pickVictim();
        if (victim < 0)
            break;
        slots_[static_cast<std::size_t>(victim)].state = State::Dying;
        out.push(Kind::Kill, victim);
        if (victim == monoSlot_)
            monoSlot_ = -1;
    }
}

void VoiceAllocator::noteOn(int note, float velocity, Actions& out) {
    note = std::clamp(note, 0, 127);
    if (mode_ != VoiceMode::Poly)
        return monoNoteOn(note, velocity, out);

    // The same key again: let the earlier voice go so one key never holds two voices open.
    for (int i = 0; i < kVoiceSlots; ++i) {
        auto& s = slots_[static_cast<std::size_t>(i)];
        if ((s.state == State::Held || s.state == State::Sustained) && s.note == note) {
            s.state = State::Releasing;
            out.push(Kind::Release, i);
        }
    }
    while (sounding() >= polyphony_) {
        const int victim = pickVictim();
        if (victim < 0)
            break;
        slots_[static_cast<std::size_t>(victim)].state = State::Dying;
        out.push(Kind::Kill, victim);
    }
    start(note, velocity, out);
}

void VoiceAllocator::noteOff(int note, Actions& out) {
    note = std::clamp(note, 0, 127);
    if (mode_ != VoiceMode::Poly)
        return monoNoteOff(note, out);
    for (int i = 0; i < kVoiceSlots; ++i) {
        auto& s = slots_[static_cast<std::size_t>(i)];
        if (s.state != State::Held || s.note != note)
            continue;
        if (pedal_) {
            s.state = State::Sustained;
        } else {
            s.state = State::Releasing;
            out.push(Kind::Release, i);
        }
    }
}

void VoiceAllocator::removeHeld(int note) {
    for (int i = 0; i < heldCount_; ++i) {
        if (held_[static_cast<std::size_t>(i)].note != note)
            continue;
        for (int j = i; j + 1 < heldCount_; ++j)
            held_[static_cast<std::size_t>(j)] = held_[static_cast<std::size_t>(j + 1)];
        --heldCount_;
        return;
    }
}

const VoiceAllocator::HeldKey* VoiceAllocator::selectHeld() const {
    if (heldCount_ == 0)
        return nullptr;
    const HeldKey* best = &held_[static_cast<std::size_t>(heldCount_ - 1)]; // last pressed
    if (priority_ == NotePriority::Last)
        return best;
    for (int i = 0; i < heldCount_; ++i) {
        const HeldKey& k = held_[static_cast<std::size_t>(i)];
        if (priority_ == NotePriority::Low ? k.note < best->note : k.note > best->note)
            best = &k;
    }
    return best;
}

void VoiceAllocator::monoNoteOn(int note, float velocity, Actions& out) {
    removeHeld(note);
    if (heldCount_ < static_cast<int>(held_.size()))
        held_[static_cast<std::size_t>(heldCount_++)] = {note, velocity};
    const HeldKey target = *selectHeld();

    const bool haveVoice = monoSlot_ >= 0 && slots_[static_cast<std::size_t>(monoSlot_)].state != State::Free &&
                           slots_[static_cast<std::size_t>(monoSlot_)].state != State::Dying;
    if (!haveVoice) {
        start(target.note, target.velocity, out);
        return;
    }
    auto& s = slots_[static_cast<std::size_t>(monoSlot_)];
    const bool keyWasDown = s.state == State::Held;
    if (keyWasDown && s.note == target.note)
        return; // a lower-priority key was added; the sounding note stays
    const bool legato = mode_ == VoiceMode::Legato && keyWasDown;
    s.note = target.note;
    s.state = State::Held;
    s.age = ++counter_;
    out.push(legato ? Kind::Glide : Kind::Retrigger, monoSlot_, target.note, target.velocity);
}

void VoiceAllocator::monoNoteOff(int note, Actions& out) {
    removeHeld(note);
    if (monoSlot_ < 0)
        return;
    auto& s = slots_[static_cast<std::size_t>(monoSlot_)];
    if (s.state != State::Held || s.note != note)
        return;
    if (const HeldKey* target = selectHeld()) {
        // Fall back to a key that is still down.
        s.note = target->note;
        out.push(mode_ == VoiceMode::Legato ? Kind::Glide : Kind::Retrigger, monoSlot_, target->note, target->velocity);
    } else if (pedal_) {
        s.state = State::Sustained;
    } else {
        s.state = State::Releasing;
        out.push(Kind::Release, monoSlot_);
    }
}

void VoiceAllocator::sustainPedal(bool down, Actions& out) {
    pedal_ = down;
    if (down)
        return;
    for (int i = 0; i < kVoiceSlots; ++i) {
        auto& s = slots_[static_cast<std::size_t>(i)];
        if (s.state == State::Sustained) {
            s.state = State::Releasing;
            out.push(Kind::Release, i);
        }
    }
}

void VoiceAllocator::allNotesOff(Actions& out) {
    heldCount_ = 0;
    pedal_ = false;
    for (int i = 0; i < kVoiceSlots; ++i) {
        auto& s = slots_[static_cast<std::size_t>(i)];
        if (s.state == State::Held || s.state == State::Sustained) {
            s.state = State::Releasing;
            out.push(Kind::Release, i);
        }
    }
}

void VoiceAllocator::allSoundOff(Actions& out) {
    heldCount_ = 0;
    pedal_ = false;
    monoSlot_ = -1;
    for (int i = 0; i < kVoiceSlots; ++i) {
        auto& s = slots_[static_cast<std::size_t>(i)];
        if (s.state != State::Free && s.state != State::Dying) {
            s.state = State::Dying;
            out.push(Kind::Kill, i);
        }
    }
}

void VoiceAllocator::voiceFinished(int slot) {
    slots_[static_cast<std::size_t>(slot)].state = State::Free;
    if (slot == monoSlot_)
        monoSlot_ = -1;
}

} // namespace zed
