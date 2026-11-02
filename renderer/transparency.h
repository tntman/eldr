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
    static constexpr VkFormat kTintFormat = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kTintDepthFormat = VK_FORMAT_D32_SFLOAT;

    explicit Transparency(Device &device);
    ~Transparency();
    Transparency(const Transparency &) = delete;
    Transparency &operator=(const Transparency &) = delete;

    void Resize(uint32_t width, uint32_t height);
    // Accum and Revealage exist only while `on` (frames that accumulate):
    // made or freed here, per frame before recording (the GPU is idle).
    void EnsureAccumulation(bool on);

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
    // idle there), then waiting in SHADER_READ_ONLY_OPTIMAL. Called every
    // frame that refracts, before recording.
    VkImageView EnsureBackground();
    void DropBackground();  // frames without refraction, before recording
    uint32_t BackgroundLevels() const { return _backgroundLevels; }
    // `color` (premultiplied, in COLOR_ATTACHMENT_OPTIMAL after the opaque
    // pass) into the background and its mips; `color` stays a colour
    // attachment. With `splits` (the light splits, GENERAL, stored by the
    // shading pass) the light path intensities `direct` and `indirect` are
    // taken out again (background_unscale.comp).
    void RecordBackground(VkCommandBuffer cmd, VkImage color, VkImageView splits = VK_NULL_HANDLE,
                          float direct = 1.0f, float indirect = 1.0f);

    // Translucent shadows: per sun cascade (layers) and for the local
    // lights' atlas, the colour translucent casters let through
    // (multiplied together) and the depth of the nearest one; receivers
    // behind it take the colour. (Re)made at these sizes, cleared to white
    // and depth 1 (no caster) and left in SHADER_READ_ONLY_OPTIMAL; 1 x 1
    // until used. Returns true when it made them (the GPU is idle there).
    struct TintMap {
        Image color, depth;
        VkImageView colorLayers[4] = {}, depthLayers[4] = {};  // the sun's, per cascade
        uint32_t size = 0;
    };
    bool EnsureTintMaps(uint32_t sunSize, uint32_t cascades, uint32_t atlasSize);
    // Recording into them: BeginTint clears `layers` of `map` and makes
    // them attachments, EndTint makes them readable again.
    void BeginTint(VkCommandBuffer cmd, const TintMap &map, uint32_t layers);
    void EndTint(VkCommandBuffer cmd, const TintMap &map, uint32_t layers);
    const TintMap &SunTint() const { return _sunTint; }
    const TintMap &AtlasTint() const { return _atlasTint; }
    VkSampler NearestSampler() const { return _sampler; }

private:
    void _Destroy();
    void _Unscale(VkCommandBuffer cmd, VkImageView splits, float direct, float indirect);

    Device &_dev;
    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _resolve = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    Image _accum, _revealage;
    Image _background;
    VkImageView _backgroundLevel0 = VK_NULL_HANDLE;  // storage view for the unscale
    uint32_t _backgroundLevels = 0;
    // background_unscale.comp, made on first use.
    VkDescriptorSetLayout _unscaleSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout _unscaleLayout = VK_NULL_HANDLE;
    VkPipeline _unscale = VK_NULL_HANDLE;
    VkDescriptorPool _unscalePool = VK_NULL_HANDLE;
    VkDescriptorSet _unscaleSet = VK_NULL_HANDLE;
    VkImageView _unscaleViews[2] = {};  // what _unscaleSet holds
    TintMap _sunTint, _atlasTint;
    uint32_t _tintCascades = 0;
    void _DestroyTint(TintMap &map);
    static void _ClearTint(VkCommandBuffer cmd, const TintMap &map, uint32_t layers, VkImageLayout from);
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
