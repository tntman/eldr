#pragma once

#include "device.h"

namespace vkr {

// Translucent surfaces: weighted blended order-independent transparency
// (McGuire & Bavoil 2013). The Renderer draws translucent meshes into
// Accum() (premultiplied colour and coverage, each times a depth weight,
// summed) and Revealage() (the product of 1 - coverage), then Resolve
// composites their weighted average over the opaque colour.
// Chosen over per-pixel sorting (linked lists or k-buffers): one extra
// 18 bytes per pixel and a full-screen pass, no atomics, no per-pixel
// list memory that grows with depth complexity and no sort, which on a
// GTX 1080 Ti costs more than the whole translucent pass whenever a few
// layers overlap. Its weights only approximate the order: the nearer of
// two layers dominates, a distant one behind it still shows a little.
class Transparency {
public:
    static constexpr VkFormat kAccumFormat = VK_FORMAT_R32G32B32A32_SFLOAT;  // HDR highlights times weights
    static constexpr VkFormat kRevealageFormat = VK_FORMAT_R16_SFLOAT;

    explicit Transparency(Device &device);
    ~Transparency();
    Transparency(const Transparency &) = delete;
    Transparency &operator=(const Transparency &) = delete;

    void Resize(uint32_t width, uint32_t height);

    // Accum and Revealage to COLOR_ATTACHMENT_OPTIMAL (the accumulation
    // pass clears them: 0 and 1).
    void BeginAccumulation(VkCommandBuffer cmd);
    // After the accumulation pass: blends the average over `color` (in
    // COLOR_ATTACHMENT_OPTIMAL, premultiplied by coverage).
    void Resolve(VkCommandBuffer cmd, VkImageView color);

    const Image &Accum() const { return _accum; }
    const Image &Revealage() const { return _revealage; }

    // Refraction: a copy of the opaque colour with a mip chain that
    // translucent surfaces sample through themselves, blurrier the
    // rougher they are. Made on first use at the current size (the GPU is
    // idle there), then waiting in SHADER_READ_ONLY_OPTIMAL.
    VkImageView EnsureBackground();
    uint32_t BackgroundLevels() const { return _backgroundLevels; }
    // `color` (premultiplied, in COLOR_ATTACHMENT_OPTIMAL after the opaque
    // pass) into the background and its mips; `color` stays a colour
    // attachment.
    void RecordBackground(VkCommandBuffer cmd, VkImage color);

private:
    void _Destroy();

    Device &_dev;
    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _resolve = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    Image _accum, _revealage;
    Image _background;
    uint32_t _backgroundLevels = 0;
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
