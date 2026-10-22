#pragma once

#include "device.h"
#include "scene.h"

namespace vkr {

class Renderer {
public:
    struct Frame {
        uint32_t width = 0;
        uint32_t height = 0;
        float viewProj[16];       // USD row-vector order, GL clip space
        float cameraPos[3];       // world space
        float clearColor[4] = {0, 0, 0, 0};
        float clearDepth = 1.0f;
    };

    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    explicit Renderer(Device &device);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // Draws `scene` and blocks until the result is copied to host memory,
    // bottom row first (GL/Hydra order).
    //   color: width*height RGBA16F (8 bytes per pixel)
    //   depth: width*height floats, window depth in [0, 1]; may be null
    void Render(const Scene &scene, const Frame &frame, void *color, float *depth);

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void _CreatePipeline();
    void _Resize(uint32_t width, uint32_t height);
    Image _CreateImage(VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void _Destroy(Image &image);

    void _UpdateDescriptors(const Scene::Gpu &gpu);

    Device &_dev;
    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
    VkFence _fence = VK_NULL_HANDLE;
    Image _color, _depth;
    Buffer _readback;
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
