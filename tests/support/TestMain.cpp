// Test runner with the audio-thread guard switched on (plan 5.3, 6.1).
//
// This file replaces the global allocation functions for the test program. Every allocation or
// free is passed to the guard, which records a violation when the calling thread is inside the
// engine's audio callback. After each test case the count must be zero; otherwise the run fails,
// whatever the test itself asserted. Tests tagged [guard-selftest] provoke violations on purpose.
#include "zederia/rt/AudioThreadGuard.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

void* allocate(std::size_t size) noexcept {
    zed::rt::checkAudioThread("allocation");
    return std::malloc(size != 0 ? size : 1);
}

void* allocateAligned(std::size_t size, std::size_t alignment) noexcept {
    zed::rt::checkAudioThread("allocation");
#if defined(_MSC_VER)
    return _aligned_malloc(size != 0 ? size : 1, alignment);
#else
    const std::size_t rounded = (size + alignment - 1) / alignment * alignment;
    return std::aligned_alloc(alignment, rounded != 0 ? rounded : alignment);
#endif
}

void release(void* p) noexcept {
    if (p == nullptr)
        return;
    zed::rt::checkAudioThread("free");
    std::free(p);
}

void releaseAligned(void* p) noexcept {
    if (p == nullptr)
        return;
    zed::rt::checkAudioThread("free");
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

int leakedViolations = 0;

class GuardListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo&) override { zed::rt::resetViolations(); }

    void testCaseEnded(const Catch::TestCaseStats& stats) override {
        const int count = zed::rt::violationCount();
        if (count == 0)
            return;
        bool selfTest = false;
        for (const auto& tag : stats.testInfo->tags)
            selfTest = selfTest || tag.original == "guard-selftest";
        if (!selfTest) {
            ++leakedViolations;
            std::fprintf(stderr, "\nAUDIO-THREAD GUARD: test \"%s\" made %d forbidden call(s) inside the audio callback (last: %s)\n",
                         stats.testInfo->name.c_str(), count, zed::rt::lastViolation());
        }
        zed::rt::resetViolations();
    }
};

CATCH_REGISTER_LISTENER(GuardListener)

} // namespace

void* operator new(std::size_t size) {
    if (void* p = allocate(size))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    if (void* p = allocate(size))
        return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (void* p = allocateAligned(size, static_cast<std::size_t>(alignment)))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    if (void* p = allocateAligned(size, static_cast<std::size_t>(alignment)))
        return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }

int main(int argc, char* argv[]) {
    const int result = Catch::Session().run(argc, argv);
    if (leakedViolations != 0) {
        std::fprintf(stderr, "\nFAILED: the audio-thread guard reported violations in %d test case(s)\n", leakedViolations);
        return result != 0 ? result : 1;
    }
    return result;
}
