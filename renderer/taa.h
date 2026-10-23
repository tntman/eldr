#pragma once

#include "device.h"

namespace vkr {

// Temporal anti-aliasing resolve. Owns the resolved frame (input to the
// post chain) and the history it keeps between frames.
class TemporalAA {
public:
    explicit TemporalAA(Device &device);
    ~TemporalAA();
    TemporalAA(const TemporalAA &) = delete;
    TemporalAA &operator=(const TemporalAA &) = delete;

    // (Re)creates the targets; `color` and `depth` must stay valid until
    // the next Resize. History starts empty.
    void Resize(uint32_t width, uint32_t height, VkImageView color, VkImageView depth);

    // Expects color in SHADER_READ_ONLY_OPTIMAL, depth in
    // DEPTH_READ_ONLY_OPTIMAL, and leaves Output() in SHADER_READ_ONLY_OPTIMAL. `alpha` is the current frame's blend weight
    // (1 ignores history); `reprojection` maps current jittered NDC to the
    // previous frame's clip space (row-vector order).
    void Record(VkCommandBuffer cmd, const float reprojection[16], float alpha);

    VkImageView Output() const { return _resolved.view; }
    // Last frame's resolve, in SHADER_READ_ONLY_OPTIMAL between frames.
    VkImageView History() const { return _history.view; }

private:
    void _Destroy();

    Device &_dev;
    VkSampler _nearest = VK_NULL_HANDLE, _linear = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    Image _resolved, _history;
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
