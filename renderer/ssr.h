#pragma once

#include "device.h"

namespace vkr {

// Denoised glossy screen-space reflections (Frame::ssrDenoise; Stachowiak
// 2015, "Stochastic Screen-Space Reflections", as in EEVEE). mesh.frag's
// opaque pixels trace one GGX-sampled ray each and store its radiance
// with its pdf, its direction and hit distance, the normal and the
// reflection's weight (Rays() .. Factor(), scene set bindings 67..70)
// instead of adding the reflection. Then, before the translucency:
//   1. spatial: each pixel reuses its neighbours' rays, weighted by its
//      own BRDF over their pdf (the ratio estimator);
//   2. temporal: the history reprojected through the reflected point
//      (parallax of the reflection, not of the surface), clamped to the
//      neighbourhood's statistics in YCoCg, with the luma's second moment
//      for the variance;
//   3. a variance-guided bilateral filter, its result times the weight
//      added to the colour (and, with `addToSplits`, to the indirect glossy
// layer of the light splits `splits`, Aovs::SplitsView).
// ponytail: full resolution, one ray per pixel; EEVEE traces at half or
// quarter resolution and stretches the spatial kernel along the reflection.
class SsrDenoise {
public:
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    explicit SsrDenoise(Device &device);
    ~SsrDenoise();
    SsrDenoise(const SsrDenoise &) = delete;
    SsrDenoise &operator=(const SsrDenoise &) = delete;

    // Drops the targets; `depth` (the prepass's) must stay valid until the
    // next Resize. The trace images stay 1 x 1 until Prepare.
    void Resize(uint32_t width, uint32_t height, VkImageView depth);
    // Before the frame's descriptors are written, with the GPU idle: makes
    // the targets on first use.
    void Prepare();

    // What mesh.frag writes (GENERAL layout; 1 x 1 placeholders until
    // Prepare).
    VkImageView Rays() const { return _rays.view; }
    VkImageView Directions() const { return _directions.view; }
    VkImageView Normals() const { return _normals.view; }
    VkImageView Factor() const { return _factor.view; }

    // Before the shading pass: marks every pixel as without a ray.
    void Clear(VkCommandBuffer cmd);
    // After it: the three passes, adding the result to `color` (in
    // COLOR_ATTACHMENT_OPTIMAL). Matrices in row-vector order; `history`
    // false starts the history over. `dimension` in [0, 1): the sample
    // dimension's value turning the blue noise (kDimSsrDenoise); < 0:
    // gradient noise shifted by `noise`.
    void Resolve(VkCommandBuffer cmd, VkImageView color, const float invViewProj[16], const float prevViewProj[16],
                 const float eye[3], uint32_t noise, bool history, float dimension,
                 VkImageView splits, bool addToSplits);
    // The 64 x 64 RGBA8 blue noise (Renderer's), before the first Prepare.
    void SetBlueNoise(VkImageView noise) { _blueNoise = noise; }

private:
    void _Destroy();
    void _Placeholders();

    Device &_dev;
    VkSampler _nearest = VK_NULL_HANDLE, _linear = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _spatial = VK_NULL_HANDLE, _temporal = VK_NULL_HANDLE, _composite = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _sets[2] = {};  // history[i] this frame's
    Image _rays, _directions, _normals, _factor;  // GENERAL: mesh.frag's storage images
    Image _resolved, _history[2];
    uint32_t _current = 0;
    VkImageView _depth = VK_NULL_HANDLE, _blueNoise = VK_NULL_HANDLE;
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
