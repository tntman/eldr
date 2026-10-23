#include "interop.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

#include <cstring>

namespace vkr {

namespace {

VkImageSubresourceRange Range(VkImageAspectFlags aspect)
{
    return {aspect, 0, 1, 0, 1};
}

} // namespace

std::unique_ptr<SharedFrame> SharedFrame::Create(Device &device)
{
#ifdef _WIN32
    if (device.externalWin32) {
        return std::unique_ptr<SharedFrame>(new SharedFrame(device));
    }
#endif
    (void)device;
    return nullptr;
}

#ifdef _WIN32

SharedFrame::SharedFrame(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    vkGetPhysicalDeviceProperties2(_dev.physical, &props);
    std::memcpy(_deviceUuid, id.deviceUUID, VK_UUID_SIZE);
    std::memcpy(_driverUuid, id.driverUUID, VK_UUID_SIZE);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = _dev.queueFamily;
    Check(vkCreateCommandPool(d, &pci, nullptr, &_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = _pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(d, &ai, &_cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(vkCreateFence(d, &fci, nullptr, &_fence), "vkCreateFence");

    auto getHandle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        vkGetDeviceProcAddr(d, "vkGetSemaphoreWin32HandleKHR"));
    for (auto [semaphore, handle] : {std::pair{&_ready, &_handles.ready}, {&_releasedSemaphore, &_handles.released}}) {
        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exportInfo};
        Check(vkCreateSemaphore(d, &sci, nullptr, semaphore), "vkCreateSemaphore");
        VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        gi.semaphore = *semaphore;
        gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE h = nullptr;
        Check(getHandle ? getHandle(d, &gi, &h) : VK_ERROR_EXTENSION_NOT_PRESENT, "vkGetSemaphoreWin32HandleKHR");
        *handle = h;
    }
}

SharedFrame::~SharedFrame()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _DestroyImages();
    for (void *h : {_handles.ready, _handles.released}) {
        if (h) {
            CloseHandle(h);
        }
    }
    vkDestroySemaphore(d, _ready, nullptr);
    vkDestroySemaphore(d, _releasedSemaphore, nullptr);
    vkDestroyFence(d, _fence, nullptr);
    vkDestroyCommandPool(d, _pool, nullptr);
}

void SharedFrame::_DestroyImages()
{
    for (void *h : {_handles.color, _handles.depth}) {
        if (h) {
            CloseHandle(h);
        }
    }
    _handles.color = _handles.depth = nullptr;
    vkDestroyImage(_dev.device, _color, nullptr);
    vkDestroyImage(_dev.device, _depth, nullptr);
    _dev.Free(_colorMemory);
    _dev.Free(_depthMemory);
    _color = _depth = VK_NULL_HANDLE;
    _colorMemory = _depthMemory = VK_NULL_HANDLE;
}

// Exportable images, each in a dedicated allocation (as GL imports them).
void SharedFrame::_Remake(uint32_t width, uint32_t height)
{
    // GL may still read the old images: wait for its `released`.
    if (_released) {
        _WaitReleased();
    }
    _DestroyImages();
    const VkDevice d = _dev.device;
    auto getHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        vkGetDeviceProcAddr(d, "vkGetMemoryWin32HandleKHR"));
    struct Shared {
        VkImage *image;
        VkDeviceMemory *memory;
        void **handle;
        VkDeviceSize *bytes;
        VkFormat format;
        VkImageUsageFlags usage;
    };
    const Shared shared[2] = {
        {&_color, &_colorMemory, &_handles.color, &_handles.colorBytes, VK_FORMAT_R16G16B16A16_SFLOAT,
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
        {&_depth, &_depthMemory, &_handles.depth, &_handles.depthBytes, VK_FORMAT_D32_SFLOAT,
         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
    };
    for (const Shared &s : shared) {
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = s.format;
        ci.extent = {width, height, 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        // GL textures can do anything; so the images may.
        ci.usage = s.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT;
        Check(vkCreateImage(d, &ci, nullptr, s.image), "vkCreateImage");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, *s.image, &req);
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = *s.image;
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated};
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        *s.memory = _dev.Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, &exportInfo);
        Check(vkBindImageMemory(d, *s.image, *s.memory, 0), "vkBindImageMemory");
        VkMemoryGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        gi.memory = *s.memory;
        gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE h = nullptr;
        Check(getHandle ? getHandle(d, &gi, &h) : VK_ERROR_EXTENSION_NOT_PRESENT, "vkGetMemoryWin32HandleKHR");
        *s.handle = h;
        *s.bytes = req.size;
    }
    _width = width;
    _height = height;
    ++_generation;
}

// Waits for `released` (GL done reading the images), on the CPU.
void SharedFrame::_WaitReleased()
{
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = _releasedSemaphore;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    Check(vkQueueSubmit2(_dev.queue, 1, &si, _fence), "vkQueueSubmit2");
    Check(vkWaitForFences(_dev.device, 1, &_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    Check(vkResetFences(_dev.device, 1, &_fence), "vkResetFences");
    _released = false;
}

void SharedFrame::Copy(VkImage color, VkImage depth, uint32_t width, uint32_t height)
{
    if (width != _width || height != _height) {
        _Remake(width, height);
    }
    const VkDevice d = _dev.device;
    Check(vkResetCommandBuffer(_cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(_cmd, &bi), "vkBeginCommandBuffer");
    const VkImage sources[2] = {color, depth}, targets[2] = {_color, _depth};
    const VkImageAspectFlags aspects[2] = {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT};
    for (int i = 0; i < 2; ++i) {
        ImageBarrier(_cmd, targets[i], Range(aspects[i]), VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {aspects[i], 0, 0, 1};
        copy.extent = {width, height, 1};
        vkCmdCopyImage(_cmd, sources[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets[i],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ImageBarrier(_cmd, targets[i], Range(aspects[i]), VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    Check(vkEndCommandBuffer(_cmd), "vkEndCommandBuffer");
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = _releasedSemaphore;
    wait.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = _ready;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = _cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = _released ? 1 : 0;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    Check(vkQueueSubmit2(_dev.queue, 1, &si, _fence), "vkQueueSubmit2");
    Check(vkWaitForFences(d, 1, &_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    Check(vkResetFences(d, 1, &_fence), "vkResetFences");
    _released = false;
}

#else

SharedFrame::SharedFrame(Device &device) : _dev(device) {}
SharedFrame::~SharedFrame() = default;
void SharedFrame::Copy(VkImage, VkImage, uint32_t, uint32_t) {}

#endif

} // namespace vkr
