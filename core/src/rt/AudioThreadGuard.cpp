#include "zederia/rt/AudioThreadGuard.h"

#include <atomic>

namespace zed::rt {
namespace {
thread_local int depth = 0;
std::atomic<int> violations{0};
std::atomic<const char*> last{""};
} // namespace

AudioThreadScope::AudioThreadScope() noexcept { ++depth; }
AudioThreadScope::~AudioThreadScope() { --depth; }

bool onAudioThread() noexcept { return depth > 0; }

void checkAudioThread(const char* what) noexcept {
    if (depth > 0) {
        violations.fetch_add(1, std::memory_order_relaxed);
        last.store(what, std::memory_order_relaxed);
    }
}

int violationCount() noexcept { return violations.load(std::memory_order_relaxed); }
const char* lastViolation() noexcept { return last.load(std::memory_order_relaxed); }

void resetViolations() noexcept {
    violations.store(0, std::memory_order_relaxed);
    last.store("", std::memory_order_relaxed);
}

} // namespace zed::rt
