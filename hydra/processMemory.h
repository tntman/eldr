#pragma once

#include <cstddef>

// The process's resident memory now and at its peak, in bytes (0 where the
// platform doesn't say). For the load line in husk's log.
struct HdVkProcessMemory {
    size_t resident = 0, peak = 0;
};
HdVkProcessMemory HdVkGetProcessMemory();
