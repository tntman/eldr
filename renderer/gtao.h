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
    void Record(VkCommandBuffer cmd, const float invViewProj[16], const float eye[3], float radius,
                uint32_t noise, uint32_t slices);

    // Output() = 1 (unoccluded) everywhere, same final layout as Record.
    void Clear(VkCommandBuffer cmd);

    // SSGI instead of AO from now on: Output() is then RGBA16F, visibility
    // (r, read as AO) and gathered irradiance / pi (gba), traced at
    // 1 / `scale` resolution (1 or 2). With the GPU idle, before Output()
    // is bound.
    void SetSsgi(bool on, uint32_t scale);
    // Like Record, gathering light from `history` (last frame's resolved
    // colour, seen by `prevViewProj`; unusable: visibility only) with
    // `steps` steps per side, its radiance clamped to `maxRadiance` (fireflies).
    void RecordSsgi(VkCommandBuffer cmd, const float invViewProj[16], const float prevViewProj[16],
                    const float eye[3], float radius, uint32_t noise, uint32_t slices, uint32_t steps,
                    VkImageView history, bool historyValid, float maxRadiance);

    VkImageView Output() const { return _ssgiOn ? _ssgi.view : _ao.view; }

private:
    void _Destroy();
    void _CreateSsgi();
    void _Draw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, const Image &target, uint32_t width,
               uint32_t height, const void *params);

    Device &_dev;
    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _gtaoPipeline = VK_NULL_HANDLE, _denoisePipeline = VK_NULL_HANDLE;
    VkPipeline _ssgiPipeline = VK_NULL_HANDLE, _ssgiDenoisePipeline = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE, _ssgiSet = VK_NULL_HANDLE;
    Image _raw;  // RG16F: visibility, distance to the camera (0 = background)
    Image _ao;   // R8: denoised visibility
    // SSGI: RGBA16F raw (light, visibility) at 1 / _scale, denoised and
    // upsampled (visibility, light).
    Image _ssgiRaw, _ssgi;
    bool _ssgiOn = false;
    uint32_t _scale = 1;
    VkImageView _depth = VK_NULL_HANDLE;
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
