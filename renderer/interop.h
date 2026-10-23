#pragma once

#include "device.h"

#include <memory>

namespace vkr {

// GPU sharing: a finished frame handed to another API on the same GPU
// (OpenGL through GL_EXT_memory_object_win32 and GL_EXT_semaphore_win32)
// instead of read back to the CPU. Owns a colour (RGBA16F) and a depth (D32)
// image in exportable memory and two exportable semaphores: Copy() copies
// the renderer's frame into the images and signals `ready`; the other API
// waits on that, reads them, signals `released` and says so (Released()),
// which the next Copy waits for before it writes them again. Windows only.
class SharedFrame {
public:
    // Null when the device can't export memory and semaphores.
    static std::unique_ptr<SharedFrame> Create(Device &device);
    ~SharedFrame();
    SharedFrame(const SharedFrame &) = delete;
    SharedFrame &operator=(const SharedFrame &) = delete;

    // Copies `color` and `depth` (the frame's, TRANSFER_SRC_OPTIMAL) into
    // the shared images, which it remakes at a new size, and leaves those
    // in TRANSFER_SRC_OPTIMAL. Returns once the GPU is done (the renderer
    // may then overwrite its images).
    void Copy(VkImage color, VkImage depth, uint32_t width, uint32_t height);
    // The other API signalled `released` after reading the images.
    void Released() { _released = true; }

    // Windows HANDLEs (owned here) and sizes of the images' memory; they
    // change with Generation() (a new size).
    struct Handles {
        void *color = nullptr, *depth = nullptr;
        VkDeviceSize colorBytes = 0, depthBytes = 0;
        void *ready = nullptr, *released = nullptr;  // the semaphores
    };
    const Handles &GetHandles() const { return _handles; }
    uint64_t Generation() const { return _generation; }
    // The device's and driver's UUIDs: the other API must run on the same.
    const uint8_t *DeviceUuid() const { return _deviceUuid; }
    const uint8_t *DriverUuid() const { return _driverUuid; }

private:
    explicit SharedFrame(Device &device);
    void _Remake(uint32_t width, uint32_t height);
    void _DestroyImages();
    void _WaitReleased();

    Device &_dev;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
    VkFence _fence = VK_NULL_HANDLE;
    VkSemaphore _ready = VK_NULL_HANDLE, _releasedSemaphore = VK_NULL_HANDLE;
    VkImage _color = VK_NULL_HANDLE, _depth = VK_NULL_HANDLE;
    VkDeviceMemory _colorMemory = VK_NULL_HANDLE, _depthMemory = VK_NULL_HANDLE;
    Handles _handles;
    bool _released = false;  // `released` is signalled and not yet waited on
    uint32_t _width = 0, _height = 0;
    uint64_t _generation = 0;
    uint8_t _deviceUuid[VK_UUID_SIZE] = {}, _driverUuid[VK_UUID_SIZE] = {};
};

} // namespace vkr
