// Standalone Vulkan layer: no Hydra/USD includes allowed under renderer/.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>

namespace vkr {

// Throws std::runtime_error naming `what` unless r == VK_SUCCESS.
void Check(VkResult r, const char *what);

VkShaderModule CreateShaderModule(VkDevice device, const uint32_t *code, size_t bytes);

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageSubresourceRange range,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                  VkImageLayout oldLayout, VkImageLayout newLayout);

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

// Device-local image with one view over all of it.
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

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

    // Memory with `props`; a type that also has `preferred` wins if there is one.
    VkDeviceMemory Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags props,
                            VkMemoryPropertyFlags preferred = 0);
    // Grows `b` to hold at least `bytes`, discarding its contents. GPU must be idle.
    // Host-visible, coherent and mapped. Pass HOST_CACHED as `preferred` for
    // buffers the CPU reads: uncached (write-combined) memory reads crawl.
    void Reserve(Buffer &b, VkDeviceSize bytes, VkBufferUsageFlags usage,
                 VkMemoryPropertyFlags preferred = 0);
    // Device-local, unmapped; fill it through a staging copy.
    Buffer CreateDeviceBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage);
    void Destroy(Buffer &b);

    Image CreateImage(VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                      uint32_t mips, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                      VkImageViewType viewType, VkImageCreateFlags flags = 0);
    void Destroy(Image &image);

    // Records one command buffer with `record`, submits it and waits.
    void Submit(const std::function<void(VkCommandBuffer)> &record);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

private:
    void _Release();

    VkCommandPool _submitPool = VK_NULL_HANDLE;
    VkFence _submitFence = VK_NULL_HANDLE;
};

} // namespace vkr
