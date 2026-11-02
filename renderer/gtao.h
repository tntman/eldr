#pragma once

#include "device.h"

namespace vkr {

// Ground-truth ambient occlusion (Jimenez 2016, after Intel's XeGTAO) from
// the depth prepass, normals reconstructed from depth, followed by an
// edge-aware denoise. TAA accumulates the per-frame noise. Or, with SSGI
// on, screen-space global illumination on the same horizon scan: the
// light of what hides each arc of the hemisphere, from last frame's
// resolved image, and the visibility that's left (shaders/gtao.frag).
class Gtao {
public:
    explicit Gtao(Device &device);
    ~Gtao();
    Gtao(const Gtao &) = delete;
    Gtao &operator=(const Gtao &) = delete;

    // (Re)creates the targets; `depth` must stay valid until the next Resize.
    void Resize(uint32_t width, uint32_t height, VkImageView depth);

    // Expects depth in DEPTH_READ_ONLY_OPTIMAL and leaves Output() in
    // SHADER_READ_ONLY_OPTIMAL. `invViewProj` maps the depth buffer's
    // (jittered) NDC to world space, row-vector order; `radius` is in world
    // units; `noise` picks this frame's sampling pattern; `slices` is the
    // number of directions per pixel.
    // `noise`: in [0, 1) this sample's offset of the blue noise
    // (SetBlueNoise; Renderer's sample dimensions), from 1 an index
    // shifting interleaved gradient noise (+1).
    void Record(VkCommandBuffer cmd, const float invViewProj[16], const float eye[3], float radius,
                float noise, uint32_t slices);
    // The 64 x 64 RGBA8 blue noise (Renderer's), before the first Resize.
    void SetBlueNoise(VkImageView noise) { _blueNoise = noise; }

    // Output() = 1 (unoccluded) everywhere, same final layout as Record.
    void Clear(VkCommandBuffer cmd);

    // SSGI instead of AO from now on: Output() is then RGBA16F, visibility
    // (r, read as AO) and gathered irradiance / pi (gba), traced at
    // 1 / `scale` resolution (1 or 2). Once per frame, with the GPU idle,
    // before Output() is bound: with SSGI on it also moves on to this
    // frame's output (last frame's is its history).
    void SetSsgi(bool on, uint32_t scale);
    // Like Record, gathering light from `history` (last frame's resolved
    // colour, seen by `prevViewProj`; unusable: visibility only) with
    // `steps` steps per side, its radiance clamped to `maxRadiance` (fireflies).
    // While the camera is `still`, the result accumulates over the frames
    // (up to 16), so its per-frame noise averages out before TAA, whose
    // neighbourhood clipping would keep the denoised blotches. `thickness`:
    // the thickness estimator; `objectThickness` > 0: the visibility
    // bitmask, objects that thick behind their front (world units).
    // `backface` >= 0: hits on back faces take that share of their light
    // (Frame::screenBackface); < 0: the bitmask takes none, the horizon all.
    // `tile`: below full resolution, which pixel of each tile traces, in
    // Morton order (0: the first, as without Frame::ssgiTileOrder).
    // `jitter` in [0, 1): the bitmask's sector jitter's sample dimension
    // (kDimSsgiJitter); < 0: from the steps' noise.
    void RecordSsgi(VkCommandBuffer cmd, const float invViewProj[16], const float prevViewProj[16],
                    const float eye[3], float radius, float noise, uint32_t slices, uint32_t steps,
                    VkImageView history, bool historyValid, float maxRadiance, bool still, bool thickness,
                    float objectThickness, float backface, uint32_t tile, float jitter);

    VkImageView Output() const { return _ssgiOn ? _ssgi[_current].view : _ao.view; }

private:
    void _Destroy();
    void _CreateSsgi();
    void _CreateAo();
    void _Draw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, const Image &target, uint32_t width,
               uint32_t height, const void *params);

    Device &_dev;
    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _gtaoPipeline = VK_NULL_HANDLE, _denoisePipeline = VK_NULL_HANDLE;
    VkPipeline _ssgiPipeline = VK_NULL_HANDLE, _ssgiDenoisePipeline = VK_NULL_HANDLE;
    VkPipeline _ssgiBitmaskPipeline = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE, _ssgiSet = VK_NULL_HANDLE;
    Image _raw;  // RG16F: visibility, distance to the camera (0 = background)
    Image _ao;   // R8: denoised visibility
    // SSGI: RGBA16F raw (light, visibility) at 1 / _scale, denoised and
    // upsampled (visibility, light).
    Image _ssgiRaw, _ssgi[2];       // _ssgi[_current] this frame's, the other last frame's
    uint32_t _current = 0, _accumulated = 0;  // frames in _ssgi's running average
    bool _ssgiOn = false;
    uint32_t _scale = 1;
    VkImageView _depth = VK_NULL_HANDLE, _blueNoise = VK_NULL_HANDLE;
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
