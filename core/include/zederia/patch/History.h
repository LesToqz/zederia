// Undo and redo (plan 3.11): a list of named states of a patch. Every step that changes sound
// (a parameter, a route, a shape, a table edit, a preset load) adds the state after it, with a
// label such as "Osc 1 Position 25 % -> 40 %". Unlimited within a session, bounded by memory: when
// the states hold more than the limit, the oldest steps are dropped.
//
// The interface and the agent tools both record here; `origin` says who made a step ("" for the
// person at the instrument, a client's name for an agent).
#pragma once

#include "zederia/patch/Patch.h"

#include <cstddef>
#include <deque>
#include <string>

namespace zed {

class History {
public:
    explicit History(std::size_t memoryLimitBytes = std::size_t{256} << 20);

    // Starts over with one state and nothing to undo.
    void reset(const Patch& state, std::string label = "Start");
    // Changes made outside the history since the last step (host automation, MIDI controllers)
    // are folded into the current step, so undoing the next step does not undo them as well.
    void absorb(const Patch& current);
    // Adds a step after the current one; the steps that could have been redone are dropped. A
    // state equal to the current one adds nothing (returns false).
    bool push(std::string label, const Patch& state, std::string origin = {});
    // Replaces the current step (its label and state) instead of adding one: repeated small changes
    // (wheel turns, arrow keys) become one step. Without a step to amend it pushes.
    bool amend(std::string label, const Patch& state, std::string origin = {});

    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const { return cursor_ + 1 < static_cast<int>(steps_.size()); }
    // Moves back or forward one step and returns the state to apply; null when there is none.
    const Patch* undo();
    const Patch* redo();
    // Moves to any step in the list (the history list in the interface).
    const Patch* jumpTo(int index);

    int size() const { return static_cast<int>(steps_.size()); }
    int current() const { return cursor_; }
    const std::string& label(int index) const { return steps_[static_cast<std::size_t>(index)].label; }
    const std::string& origin(int index) const { return steps_[static_cast<std::size_t>(index)].origin; }
    const Patch& state(int index) const { return steps_[static_cast<std::size_t>(index)].state; }
    const Patch& currentState() const { return state(cursor_); }
    // What undo would undo, and what redo would redo ("" when nothing).
    std::string undoLabel() const { return canUndo() ? label(cursor_) : std::string(); }
    std::string redoLabel() const { return canRedo() ? label(cursor_ + 1) : std::string(); }
    std::size_t memoryBytes() const { return bytes_; }
    // Steps dropped from the start because of the memory limit since the last reset.
    int dropped() const { return dropped_; }

    // Bytes a state costs beyond what the previous one already holds (shared tables count once).
    static std::size_t stateBytes(const Patch& state, const Patch* previous);

private:
    struct Step {
        std::string label, origin;
        Patch state;
        std::size_t bytes = 0;
    };
    void trim();

    std::deque<Step> steps_;
    int cursor_ = 0;
    std::size_t limit_, bytes_ = 0;
    int dropped_ = 0;
};

} // namespace zed
