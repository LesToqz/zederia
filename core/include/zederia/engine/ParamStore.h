// Live parameter values shared between threads (plan 5.3). Any thread may set a value; the audio
// thread collects the changed ones at the start of each block. Everything here is lock-free.
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>

namespace zed {

class ParamStore {
public:
    explicit ParamStore(int count)
        : count_(count), words_((count + 63) / 64),
          values_(std::make_unique<std::atomic<double>[]>(static_cast<std::size_t>(count))),
          dirty_(std::make_unique<std::atomic<std::uint64_t>[]>(static_cast<std::size_t>(words_))) {
        for (int i = 0; i < count_; ++i)
            values_[static_cast<std::size_t>(i)].store(0.0, std::memory_order_relaxed);
        for (int w = 0; w < words_; ++w)
            dirty_[static_cast<std::size_t>(w)].store(0, std::memory_order_relaxed);
    }

    int size() const noexcept { return count_; }

    // Plain (not normalized) value. Safe from any thread.
    void set(int index, double value) noexcept {
        values_[static_cast<std::size_t>(index)].store(value, std::memory_order_relaxed);
        dirty_[static_cast<std::size_t>(index >> 6)].fetch_or(std::uint64_t{1} << (index & 63),
                                                              std::memory_order_release);
    }
    double get(int index) const noexcept {
        return values_[static_cast<std::size_t>(index)].load(std::memory_order_relaxed);
    }

    // Audio thread: calls apply(index, value) for every value set since the last drain.
    template <typename Apply>
    void drain(Apply&& apply) noexcept {
        for (int w = 0; w < words_; ++w) {
            auto& word = dirty_[static_cast<std::size_t>(w)];
            if (word.load(std::memory_order_relaxed) == 0)
                continue;
            std::uint64_t bits = word.exchange(0, std::memory_order_acquire);
            while (bits != 0) {
                const int index = w * 64 + std::countr_zero(bits);
                bits &= bits - 1;
                apply(index, get(index));
            }
        }
    }

private:
    static_assert(std::atomic<double>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free);
    int count_, words_;
    std::unique_ptr<std::atomic<double>[]> values_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> dirty_;
};

} // namespace zed
