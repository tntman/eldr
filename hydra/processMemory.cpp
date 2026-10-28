#include "processMemory.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2  // K32GetProcessMemoryInfo, in kernel32
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <cstring>
#endif

HdVkProcessMemory HdVkGetProcessMemory()
{
    HdVkProcessMemory m;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS c{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c))) {
        m.resident = c.WorkingSetSize;
        m.peak = c.PeakWorkingSetSize;
    }
#else
    if (FILE *f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            unsigned long kb = 0;
            if (std::sscanf(line, "VmRSS: %lu", &kb) == 1) {
                m.resident = size_t(kb) * 1024;
            } else if (std::sscanf(line, "VmHWM: %lu", &kb) == 1) {
                m.peak = size_t(kb) * 1024;
            }
        }
        std::fclose(f);
    }
#endif
    return m;
}
