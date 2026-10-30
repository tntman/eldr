// Standalone Vulkan layer: no Hydra/USD includes allowed under renderer/.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

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

// Where Eldr caches on disk (pipelines, compressed textures):
// HDVK_PIPELINE_CACHE_DIR, else LOCALAPPDATA's Eldr, else the temp folder's.
std::filesystem::path CacheFolder();

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
    // `texture`: counted as texture memory in Memory(). `next`: chained to
    // VkMemoryAllocateInfo (dedicated, exported).
    VkDeviceMemory Allocate(VkMemoryRequirements req, VkMemoryPropertyFlags props,
                            VkMemoryPropertyFlags preferred = 0, bool texture = false, const void *next = nullptr);
    void Free(VkDeviceMemory memory);  // memory from Allocate, or null

    // Bytes of all memory from Allocate: device-local (graphics memory) and
    // the rest (system memory the GPU reads); textures are part of the
    // device-local bytes; the most device-local bytes held at once. With
    // VK_EXT_memory_budget, the driver's view of the device-local heaps:
    // their size, what this process uses and what it may use (0 without).
    struct MemoryStats {
        VkDeviceSize deviceLocal = 0, host = 0, textures = 0, peakDeviceLocal = 0;
        VkDeviceSize heapSize = 0, heapUsage = 0, heapBudget = 0;
    };
    MemoryStats Memory() const;
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

    // Waits for a frame the renderer left in flight (Frame::deferReadback),
    // if any: Submit, Destroy, Scene::Commit and Texture call it before
    // touching or freeing what that frame may use. Set by the Renderer.
    void WaitFrame() const
    {
        if (waitFrame) {
            waitFrame();
        }
    }
    std::function<void()> waitFrame;
    // Scene::Commit's news: the scene has glass (refraction) or caustic
    // casters and lights, so their ray tracing pipelines can start compiling
    // before a frame traces with them. Set by the Renderer.
    std::function<void(bool refraction, bool caustics)> sceneTraces;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    bool memoryBudget = false;  // VK_EXT_memory_budget is enabled
    // GPU sharing: memory and semaphores export as Windows handles
    // (VK_KHR_external_memory_win32, VK_KHR_external_semaphore_win32).
    bool externalWin32 = false;
    // Minimum-reduction samplers of R32_SFLOAT (samplerFilterMinmax): soft
    // sun shadows bound their blocker search with them.
    bool samplerMinmax = false;
    // VK_KHR_ray_tracing_pipeline and VK_KHR_acceleration_structure are
    // enabled (with bufferDeviceAddress; every allocation can back a
    // device address): see RayTracing. HDVK_NO_RAY_TRACING=1 leaves them
    // off, as on GPUs without them.
    bool rayTracing = false;
    // What buffers acceleration structures are built from (and ray tracing
    // shaders read) add to their usage; 0 without rayTracing.
    VkBufferUsageFlags rayTracingInputUsage = 0;

    // Compiled pipelines kept across runs: the first call reads the file
    // for this driver (pipelineCacheUUID) and these shaders (`shaderHash`)
    // from %LOCALAPPDATA%\Eldr (HDVK_PIPELINE_CACHE_DIR overrides it) into
    // pipelineCache, later ones do nothing. SavePipelineCache writes it
    // back (any thread; also when the Device goes). pipelineCacheBytes:
    // what the file held.
    // ponytail: files of older shaders stay; delete the folder to reclaim.
    void LoadPipelineCache(uint64_t shaderHash);
    void SavePipelineCache();
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    size_t pipelineCacheBytes = 0;

private:
    void _Release();

    std::string _pipelineCachePath;
    std::mutex _pipelineCacheMutex;  // one writer of the file at a time
    size_t _savedCacheBytes = 0;     // the file's size as loaded or last written: equal size, nothing new to write

    VkCommandPool _submitPool = VK_NULL_HANDLE;
    VkFence _submitFence = VK_NULL_HANDLE;

    struct Allocation {
        VkDeviceSize size;
        bool deviceLocal, texture;
    };
    mutable std::mutex _memoryMutex;
    std::unordered_map<VkDeviceMemory, Allocation> _allocations;
    MemoryStats _memory;
};

} // namespace vkr
