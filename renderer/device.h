// Standalone Vulkan layer: no Hydra/USD includes allowed under renderer/.
#pragma once

#include <cstdint>
#include <string>

namespace vkr {

struct DeviceInfo {
    bool supported = false;
    std::string name;
    uint32_t apiVersion = 0;
    std::string reasonWhyNot;
};

// Creates a throwaway instance and picks the best GPU that has what the
// renderer needs: Vulkan 1.3, bindless descriptor indexing, multi-draw
// indirect with count, dynamic rendering, synchronization2.
DeviceInfo ProbeDevice();

} // namespace vkr
