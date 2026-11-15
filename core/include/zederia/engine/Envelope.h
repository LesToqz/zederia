// Envelopes (plan 3.7). Envelope is the standard one: delay, attack, hold, decay, sustain and
// release, with a curve for the attack, the decay and the release. MultiEnvelope is the
// multi-segment mode of envelopes 2 to 4: up to 32 points with a sustain point and a loop region.
//
// The engine drives both the same way: advance to the next control tick and read the level
// there. Segment times are exact whatever the step size, and stages restart from the current
// level, so retriggers and releases never jump.
#pragma once

#include "zederia/patch/Shape.h"

#include <algorithm>

namespace zed {

struct EnvelopeTimes { // in samples
    double delay = 0.0, attack = 0.0, hold = 0.0, decay = 0.0, release = 0.0;
    double sustain = 1.0;
    double attackCurve = 0.0, decayCurve = 0.0, releaseCurve = 0.0; // -1..1, see curveValue()
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
    double level() const { return level_; }

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
        case Stage::Attack: return from_ + (1.0 - from_) * curveValue(x, t.attackCurve);
        case Stage::Hold: return 1.0;
        case Stage::Decay: return 1.0 + (t.sustain - 1.0) * curveValue(x, t.decayCurve);
        case Stage::Sustain: return t.sustain;
        case Stage::Release: return from_ * (1.0 - curveValue(x, killed_ ? 0.0 : t.releaseCurve));
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

// A multi-segment envelope as the audio thread reads it: times already in samples.
struct MultiEnvelopeDef {
    struct Point {
        double samples = 0.0, level = 0.0, curve = 0.0;
    };
    int count = 0;
    Point points[kMaxEnvPoints];
    int sustainPoint = -1, loopStart = -1, loopEnd = -1;
};

class MultiEnvelope {
public:
    void trigger(double from) {
        segment_ = 0;
        time_ = 0.0;
        from_ = level_ = from;
        released_ = false;
        running_ = true;
    }
    // Key up: leaves the sustain point or the loop and carries on from the current level through
    // the points after it.
    void release(const MultiEnvelopeDef& d) {
        if (!running_ || released_)
            return;
        released_ = true;
        const int hold = d.sustainPoint >= 0 ? d.sustainPoint : d.loopEnd;
        if (hold >= 0 && segment_ <= hold) {
            segment_ = hold + 1;
            time_ = 0.0;
            from_ = level_;
        }
    }

    double advance(double samples, const MultiEnvelopeDef& d) {
        int guard = 0; // a loop made of zero-length segments must not spin for ever
        while (running_ && samples > 0.0 && guard++ < 4 * kMaxEnvPoints) {
            if (segment_ >= d.count) {
                running_ = false;
                break;
            }
            const MultiEnvelopeDef::Point& p = d.points[segment_];
            const double left = p.samples - time_;
            if (samples < left) {
                time_ += samples;
                samples = 0.0;
                break;
            }
            samples -= std::max(left, 0.0);
            // Arrived at point `segment_`.
            level_ = from_ = p.level;
            time_ = 0.0;
            if (!released_ && segment_ == d.sustainPoint) {
                holding_ = true;
                return level_;
            }
            if (!released_ && segment_ == d.loopEnd && d.loopStart >= 0 && d.loopStart < d.loopEnd) {
                from_ = level_ = d.points[d.loopStart].level;
                segment_ = d.loopStart + 1;
            } else {
                ++segment_;
            }
        }
        holding_ = false;
        if (running_ && segment_ < d.count) {
            const MultiEnvelopeDef::Point& p = d.points[segment_];
            const double x = p.samples > 0.0 ? std::min(time_ / p.samples, 1.0) : 1.0;
            level_ = from_ + (p.level - from_) * curveValue(x, p.curve);
        } else {
            running_ = false;
        }
        return level_;
    }

    bool finished() const { return !running_; }
    double level() const { return level_; }

private:
    int segment_ = 0;   // the point being travelled to
    double time_ = 0.0; // samples spent on the way there
    double from_ = 0.0, level_ = 0.0;
    bool released_ = false, running_ = false, holding_ = false;
};

} // namespace zed
