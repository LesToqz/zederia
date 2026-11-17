// Private parts of the processor, shared by its source files.
#pragma once

#include "PluginProcessor.h"

#include <array>
#include <chrono>

// Builds the workshop's edited tables off the message thread (plan 5.8: audio within 50 ms of an
// edit). Only the newest request per oscillator is built; a newer request, or restore() taking the
// oscillator, makes an older build stale and it is dropped instead of handed to the engine.
class ZederiaProcessor::TableBuilder final : public juce::Thread {
public:
    explicit TableBuilder(ZederiaProcessor& owner);
    ~TableBuilder() override;

    void request(int osc, std::shared_ptr<const zed::Wavetable> table);
    // restore() is about to set this oscillator's table itself: anything queued or in flight is stale.
    void cancel(int osc);
    bool busy() const;
    // Hands a built table to the engine unless a newer request or a cancel came meanwhile.
    void run() override;

private:
    struct Request {
        std::shared_ptr<const zed::Wavetable> table;
        std::chrono::steady_clock::time_point when{};
        std::uint64_t generation = 0;
    };
    ZederiaProcessor& owner_;
    juce::CriticalSection lock_;
    std::array<Request, zed::kNumOscs> pending_{};
    std::array<std::uint64_t, zed::kNumOscs> generation_{};
    int inFlight_ = 0;
    juce::WaitableEvent wake_;
};
