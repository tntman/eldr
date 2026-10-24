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

    // (Re)creates the targets; `color`, `depth` and `velocity` (motion
    // vectors, see mesh.frag) must stay valid until the next Resize. History
    // starts empty.
    void Resize(uint32_t width, uint32_t height, VkImageView color, VkImageView depth, VkImageView velocity);

    // Expects color and velocity in SHADER_READ_ONLY_OPTIMAL, depth in
    // DEPTH_READ_ONLY_OPTIMAL, and leaves Output() in SHADER_READ_ONLY_OPTIMAL. `alpha` is the current frame's blend weight
    // (1 ignores history); `reprojection` maps current jittered NDC to the
    // previous frame's clip space (row-vector order).
    void Record(VkCommandBuffer cmd, const float reprojection[16], float alpha);

    // Film accumulation instead (shaders/film.comp), same layouts: adds
    // this sample, its pixels' samples `offset` pixels from their centres,
    // through a Gaussian of `radius` pixels to the fp32 sums and resolves
    // their average. `mode` 0 adds, 1 starts over, 2 starts over from the
    // history (the TAA image) at `seed` samples' weight.
    void RecordFilm(VkCommandBuffer cmd, const float offset[2], float radius, uint32_t mode, float seed);

    VkImageView Output() const { return _resolved.view; }
    VkImage OutputImage() const { return _resolved.image; }
    // Last frame's resolve, in SHADER_READ_ONLY_OPTIMAL between frames.
    VkImageView History() const { return _history.view; }

private:
    void _Destroy();
    void _CopyToHistory(VkCommandBuffer cmd, VkImageLayout layout, VkPipelineStageFlags2 stage,
                        VkAccessFlags2 access);

    Device &_dev;
    VkSampler _nearest = VK_NULL_HANDLE, _linear = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    Image _resolved, _history;
    // Film: compute pipeline, set and its fp32 sums (colour * weight; weight), GENERAL.
    VkDescriptorSetLayout _filmSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout _filmLayout = VK_NULL_HANDLE;
    VkPipeline _film = VK_NULL_HANDLE;
    VkDescriptorSet _filmSet = VK_NULL_HANDLE;
    Image _sums, _weights;
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
