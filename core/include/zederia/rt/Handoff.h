// Hands a large immutable object (a wavetable, a sample, a curve) from any thread to the audio
// thread without locks, and brings the replaced object back to be freed off the audio thread
// (plan 5.3). The audio thread only moves pointers; it never allocates, frees or drops the last
// reference to anything.
#pragma once

#include "zederia/rt/AudioThreadGuard.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace zed::rt {

template <class T>
class Handoff {
public:
    Handoff() {
        for (auto& slot : retired_)
            slot.store(nullptr, std::memory_order_relaxed);
    }
    ~Handoff() {
        delete pending_.exchange(nullptr);
        delete current_;
        delete previous_;
        collect();
    }
    Handoff(const Handoff&) = delete;
    Handoff& operator=(const Handoff&) = delete;

    // --- Any non-audio thread ---------------------------------------------------------------
    // Offers a new object; the audio thread picks it up at the start of a block. An offer that
    // was not picked up yet is replaced. Also frees what the audio thread has retired.
    void set(std::shared_ptr<const T> value) {
        const std::lock_guard<Mutex> lock(mutex_);
        collectLocked();
        latest_ = value;
        delete pending_.exchange(new Node{std::move(value)}, std::memory_order_acq_rel);
    }
    // The object most recently offered (whether or not the audio thread has taken it yet).
    std::shared_ptr<const T> latest() const {
        const std::lock_guard<Mutex> lock(mutex_);
        return latest_;
    }
    void collect() {
        const std::lock_guard<Mutex> lock(mutex_);
        collectLocked();
    }

    // --- Audio thread ------------------------------------------------------------------------
    const T* current() const noexcept { return current_ != nullptr ? current_->value.get() : nullptr; }
    // The object that was current before the last take(), until retirePrevious() succeeds.
    const T* previous() const noexcept { return previous_ != nullptr ? previous_->value.get() : nullptr; }
    bool hasPending() const noexcept { return pending_.load(std::memory_order_relaxed) != nullptr; }
    // Makes the offered object current. Call only when previous() is null. Returns false when
    // nothing was offered.
    bool take() noexcept {
        if (previous_ != nullptr || pending_.load(std::memory_order_relaxed) == nullptr)
            return false;
        Node* node = pending_.exchange(nullptr, std::memory_order_acq_rel);
        if (node == nullptr)
            return false;
        previous_ = current_;
        current_ = node;
        return true;
    }
    // Gives the previous object back to be freed elsewhere. Returns false when the return queue
    // is full; try again in a later block.
    bool retirePrevious() noexcept {
        if (previous_ == nullptr)
            return true;
        for (auto& slot : retired_) {
            Node* expected = nullptr;
            if (slot.compare_exchange_strong(expected, previous_, std::memory_order_acq_rel)) {
                previous_ = nullptr;
                return true;
            }
        }
        return false;
    }

private:
    struct Node {
        std::shared_ptr<const T> value;
    };
    void collectLocked() {
        for (auto& slot : retired_)
            delete slot.exchange(nullptr, std::memory_order_acq_rel);
    }

    mutable Mutex mutex_; // serializes the non-audio side only
    std::shared_ptr<const T> latest_;
    std::atomic<Node*> pending_{nullptr};
    std::array<std::atomic<Node*>, 8> retired_;
    Node* current_ = nullptr;  // audio thread only (and set() before audio starts, through take())
    Node* previous_ = nullptr; // audio thread only
};

} // namespace zed::rt
