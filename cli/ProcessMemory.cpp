// The one place the command-line tool asks the operating system about its own memory (the soak's
// growth check). Other systems report 0, and the check is then skipped.
#include "ProcessMemory.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace cli {

double privateMegabytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
#endif
    return 0.0;
}

} // namespace cli
