// The audio-thread guard (plan 5.3). The engine marks the audio callback with AudioThreadScope.
// While a thread is inside that scope, any allocation, free or lock is a violation: tests install
// allocation hooks that call checkAudioThread(), and core code takes locks only through rt::Mutex,
// which calls it as well. A test fails when the violation count is not zero.
#pragma once

#include <mutex>

namespace zed::rt {

class AudioThreadScope {
public:
    AudioThreadScope() noexcept;
    ~AudioThreadScope();
    AudioThreadScope(const AudioThreadScope&) = delete;
    AudioThreadScope& operator=(const AudioThreadScope&) = delete;
};

bool onAudioThread() noexcept;

// Records a violation if the calling thread is inside the audio callback. `what` must be a literal.
void checkAudioThread(const char* what) noexcept;
int violationCount() noexcept;
const char* lastViolation() noexcept; // "" when there is none
void resetViolations() noexcept;

// The only mutex core code may use: it reports when it is taken on the audio thread.
class Mutex {
public:
    void lock() {
        checkAudioThread("lock");
        mutex_.lock();
    }
    bool try_lock() {
        checkAudioThread("try_lock");
        return mutex_.try_lock();
    }
    void unlock() { mutex_.unlock(); }

private:
    std::mutex mutex_;
};

} // namespace zed::rt
