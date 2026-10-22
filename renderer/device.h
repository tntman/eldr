// Standalone Vulkan layer: no Hydra/USD includes allowed under renderer/.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace vkr {

// Throws std::runtime_error naming `what` unless r == VK_SUCCESS.
void Check(VkResult r, const char *what);

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

// Host-visible, coherent, persistently mapped buffer.
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
};

class Device {
public:
    Device();  // throws std::runtime_error if no suitable GPU
    ~Device();
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    VkDeviceMemory Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags props);
    // Grows `b` to hold at least `bytes`, discarding its contents. GPU must be idle.
    void Reserve(Buffer &b, VkDeviceSize bytes, VkBufferUsageFlags usage);
    void Destroy(Buffer &b);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

private:
    void _Release();
};

} // namespace vkr
