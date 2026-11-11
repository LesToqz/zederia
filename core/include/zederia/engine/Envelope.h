// Placeholder amp envelope for milestone 1: delay, attack, hold, decay, sustain, release with
// straight segments. Milestone 3 replaces it with curved and multi-segment envelopes; the way the
// engine drives it (advance to the next control tick, restart stages from the current level)
// stays.
#pragma once

#include <algorithm>

namespace zed {

struct EnvelopeTimes { // in samples
    double delay = 0.0, attack = 0.0, hold = 0.0, decay = 0.0, release = 0.0;
    double sustain = 1.0;
};

class Envelope {
public:
    enum class Stage { Idle, Delay, Attack, Hold, Decay, Sustain, Release };

    // Starts a note. `from` is the current level, so a retrigger rises from where it is.
    void trigger(double from) {
        stage_ = Stage::Delay;
        time_ = 0.0;
        from_ = level_ = from;
    }
    // Key up. `from` is the level the release starts at.
    void release(double from) { fade(from, false); }
    // Voice stolen: fall to silence over `fadeSamples` whatever the release time is.
    void kill(double from, double fadeSamples) {
        fade(from, true);
        killTime_ = fadeSamples;
    }

    // Moves `samples` forward and returns the level at the new position.
    double advance(double samples, const EnvelopeTimes& t) {
        while (samples > 0.0 && stage_ != Stage::Idle && stage_ != Stage::Sustain) {
            const double length = stageLength(t);
            const double left = length - time_;
            if (samples < left) {
                time_ += samples;
                samples = 0.0;
            } else {
                samples -= std::max(left, 0.0);
                nextStage();
            }
        }
        level_ = levelNow(t);
        return level_;
    }

    bool finished() const { return stage_ == Stage::Idle; }
    Stage stage() const { return stage_; }

private:
    void fade(double from, bool killed) {
        stage_ = Stage::Release;
        time_ = 0.0;
        from_ = level_ = from;
        killed_ = killed;
    }
    double stageLength(const EnvelopeTimes& t) const {
        switch (stage_) {
        case Stage::Delay: return t.delay;
        case Stage::Attack: return t.attack;
        case Stage::Hold: return t.hold;
        case Stage::Decay: return t.decay;
        case Stage::Release: return killed_ ? killTime_ : t.release;
        default: return 0.0;
        }
    }
    void nextStage() {
        time_ = 0.0;
        switch (stage_) {
        case Stage::Delay: stage_ = Stage::Attack; break;
        case Stage::Attack: stage_ = Stage::Hold; break;
        case Stage::Hold: stage_ = Stage::Decay; break;
        case Stage::Decay: stage_ = Stage::Sustain; break;
        default: stage_ = Stage::Idle; break;
        }
    }
    double levelNow(const EnvelopeTimes& t) const {
        const double length = stageLength(t);
        const double x = length > 0.0 ? std::min(time_ / length, 1.0) : 1.0;
        switch (stage_) {
        case Stage::Delay: return from_;
        case Stage::Attack: return from_ + (1.0 - from_) * x;
        case Stage::Hold: return 1.0;
        case Stage::Decay: return 1.0 + (t.sustain - 1.0) * x;
        case Stage::Sustain: return t.sustain;
        case Stage::Release: return from_ * (1.0 - x);
        case Stage::Idle: break;
        }
        return 0.0;
    }

    Stage stage_ = Stage::Idle;
    double time_ = 0.0;  // samples spent in the current stage
    double from_ = 0.0;  // level the attack or release started at
    double level_ = 0.0;
    double killTime_ = 0.0;
    bool killed_ = false;
};

} // namespace zed
