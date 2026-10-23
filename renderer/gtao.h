#pragma once

#include "device.h"

namespace vkr {

// Ground-truth ambient occlusion (Jimenez 2016, after Intel's XeGTAO) from
// the depth prepass, normals reconstructed from depth, followed by an
// edge-aware denoise. TAA accumulates the per-frame noise.
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
    // units; `noise` picks this frame's sampling pattern.
    void Record(VkCommandBuffer cmd, const float invViewProj[16], const float eye[3], float radius,
                uint32_t noise);

    // Output() = 1 (unoccluded) everywhere, same final layout as Record.
    void Clear(VkCommandBuffer cmd);

    VkImageView Output() const { return _ao.view; }

private:
    void _Destroy();

    Device &_dev;
    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _gtaoPipeline = VK_NULL_HANDLE, _denoisePipeline = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    Image _raw;  // RG16F: visibility, distance to the camera (0 = background)
    Image _ao;   // R8: denoised visibility
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
