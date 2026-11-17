#include "zederia/patch/History.h"

#include <algorithm>

namespace zed {

History::History(std::size_t memoryLimitBytes) : limit_(std::max<std::size_t>(memoryLimitBytes, std::size_t{1} << 20)) {
    reset(Patch());
}

std::size_t History::stateBytes(const Patch& s, const Patch* previous) {
    std::size_t bytes = sizeof(Patch) + s.values().size() * sizeof(double);
    const auto text = [](const std::string& t) { return t.size(); };
    bytes += text(s.meta.name) + text(s.meta.notes) + s.meta.tags.size() * 16;
    for (const auto& route : s.routes)
        bytes += sizeof(ModRoute) + route.curveShape.points.size() * sizeof(ShapePoint);
    for (const auto& shape : s.lfoShapes)
        bytes += shape.points.size() * sizeof(ShapePoint);
    for (const auto& shape : s.envShapes)
        bytes += shape.points.size() * sizeof(EnvPoint);
    for (const auto& clip : s.clips)
        bytes += clip.notes.size() * sizeof(ClipNote);
    for (const auto& pattern : s.arpPatterns)
        bytes += pattern.steps.size() * sizeof(ArpStep);
    for (std::size_t i = 0; i < s.tableData.size(); ++i) {
        const auto& table = s.tableData[i].table;
        if (table == nullptr)
            continue;
        if (previous != nullptr && previous->tableData[i].table == table)
            continue; // shared with the previous state: already counted
        bytes += table->frames.size() * sizeof(Frame);
    }
    return bytes;
}

void History::reset(const Patch& state, std::string label) {
    steps_.clear();
    steps_.push_back({std::move(label), {}, state, stateBytes(state, nullptr)});
    cursor_ = 0;
    bytes_ = steps_.front().bytes;
    dropped_ = 0;
}

void History::absorb(const Patch& current) {
    Step& step = steps_[static_cast<std::size_t>(cursor_)];
    if (step.state == current)
        return;
    bytes_ -= step.bytes;
    step.state = current;
    step.bytes = stateBytes(current, cursor_ > 0 ? &steps_[static_cast<std::size_t>(cursor_ - 1)].state : nullptr);
    bytes_ += step.bytes;
}

bool History::push(std::string label, const Patch& state, std::string origin) {
    if (steps_[static_cast<std::size_t>(cursor_)].state == state)
        return false;
    while (static_cast<int>(steps_.size()) > cursor_ + 1) {
        bytes_ -= steps_.back().bytes;
        steps_.pop_back();
    }
    const std::size_t bytes = stateBytes(state, &steps_.back().state);
    steps_.push_back({std::move(label), std::move(origin), state, bytes});
    bytes_ += bytes;
    cursor_ = static_cast<int>(steps_.size()) - 1;
    trim();
    return true;
}

void History::trim() {
    // Always keep the current step and the one before it.
    while (bytes_ > limit_ && cursor_ > 1) {
        bytes_ -= steps_.front().bytes;
        steps_.pop_front();
        --cursor_;
        ++dropped_;
        // The new first step now carries its tables alone.
        Step& first = steps_.front();
        bytes_ -= first.bytes;
        first.bytes = stateBytes(first.state, nullptr);
        bytes_ += first.bytes;
    }
}

const Patch* History::undo() {
    if (!canUndo())
        return nullptr;
    --cursor_;
    return &steps_[static_cast<std::size_t>(cursor_)].state;
}

const Patch* History::redo() {
    if (!canRedo())
        return nullptr;
    ++cursor_;
    return &steps_[static_cast<std::size_t>(cursor_)].state;
}

const Patch* History::jumpTo(int index) {
    if (index < 0 || index >= size())
        return nullptr;
    cursor_ = index;
    return &steps_[static_cast<std::size_t>(cursor_)].state;
}

} // namespace zed

namespace zed {

bool History::amend(std::string label, const Patch& state, std::string origin) {
    if (cursor_ == 0)
        return push(std::move(label), state, std::move(origin));
    while (static_cast<int>(steps_.size()) > cursor_ + 1) {
        bytes_ -= steps_.back().bytes;
        steps_.pop_back();
    }
    Step& step = steps_.back();
    bytes_ -= step.bytes;
    step.label = std::move(label);
    step.origin = std::move(origin);
    step.state = state;
    step.bytes = stateBytes(state, &steps_[static_cast<std::size_t>(cursor_ - 1)].state);
    bytes_ += step.bytes;
    trim();
    return true;
}

} // namespace zed
