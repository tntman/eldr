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
// Below full resolution (Prepare's divisor 2, Frame::movingSsrResolution)
// mesh.frag only stores what each pixel reflects without a hit and its
// normals; ssr_trace.frag then traces one ray per 2 x 2 block (from its
// first pixel), passes 1 and 2 run over the blocks, and pass 3 takes each
// pixel's reflection from the blocks around it on its own surface (depth
// and normal), or what it reflects without a hit where none is. Without
// the denoiser (`denoise` false) the mirror rays go straight to pass 3.
// ponytail: always the block's first pixel; EEVEE moves it per frame and
// stretches the spatial kernel along the reflection.
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
    // the targets on first use and when `divisor` (1: every pixel traces in
    // mesh.frag, 2: every 2 x 2 block here) changes, which starts the
    // history over. `sceneLayout`: the scene set's (the trace reads it).
    void Prepare(uint32_t divisor, VkDescriptorSetLayout sceneLayout);
    uint32_t Divisor() const { return _divisor; }

    // What mesh.frag writes (GENERAL layout; 1 x 1 placeholders until
    // Prepare).
    VkImageView Rays() const { return _rays.view; }
    VkImageView Directions() const { return _directions.view; }
    VkImageView Normals() const { return _normals.view; }
    VkImageView Factor() const { return _factor.view; }

    // Before the shading pass: marks every pixel as without a ray.
    void Clear(VkCommandBuffer cmd);
    // After it: the trace below full resolution (with the scene's set
    // `scene`), then the three passes (`denoise` false, below full
    // resolution only: the last alone), adding the result to `color` (in
    // COLOR_ATTACHMENT_OPTIMAL). Matrices in row-vector order; `history`
    // false starts the history over. `dimension` in [0, 1): the sample
    // dimension's value turning the blue noise (kDimSsrDenoise); < 0:
    // gradient noise shifted by `noise`.
    void Resolve(VkCommandBuffer cmd, VkDescriptorSet scene, VkImageView color, const float invViewProj[16],
                 const float prevViewProj[16], const float eye[3], uint32_t noise, bool history, float dimension,
                 VkImageView splits, bool addToSplits, float indirect = 1.0f, bool denoise = true);
    // The 64 x 64 RGBA8 blue noise (Renderer's), before the first Prepare.
    void SetBlueNoise(VkImageView noise) { _blueNoise = noise; }

private:
    void _Destroy();
    void _Placeholders();

    Device &_dev;
    VkSampler _nearest = VK_NULL_HANDLE, _linear = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE, _traceLayout = VK_NULL_HANDLE;  // the trace's: the scene set
    VkPipeline _spatial = VK_NULL_HANDLE, _temporal = VK_NULL_HANDLE, _composite = VK_NULL_HANDLE;
    VkPipeline _trace = VK_NULL_HANDLE;  // made when first traced below full resolution
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _sets[2] = {};  // history[i] this frame's
    // GENERAL: mesh.frag's storage images. Below full resolution _rays
    // holds each pixel's reflection without a hit, and _directions (per
    // block) is the trace's, as _traced: the rays' radiance and pdf.
    Image _rays, _directions, _normals, _factor;
    Image _traced, _resolved, _history[2];  // per block below full resolution
    uint32_t _current = 0;
    VkImageView _depth = VK_NULL_HANDLE, _blueNoise = VK_NULL_HANDLE;
    uint32_t _width = 0, _height = 0, _divisor = 1;
    bool _fresh = true;  // the history's targets are new, or the last Resolve left them out
};

} // namespace vkr
