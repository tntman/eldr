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

    // (Re)creates the targets at the frame's size; history starts empty.
    // Then SetInput.
    void Resize(uint32_t width, uint32_t height);
    // This frame's `color`, `depth` and `velocity` (motion vectors, see
    // mesh.frag), `width` x `height`: the frame's size, or smaller to
    // upscale (TAAU; the film needs the frame's); they must stay valid
    // until the next SetInput. The history stays.
    void SetInput(uint32_t width, uint32_t height, VkImageView color, VkImageView depth, VkImageView velocity);

    // Expects color and velocity in SHADER_READ_ONLY_OPTIMAL, depth in
    // DEPTH_READ_ONLY_OPTIMAL, and leaves Output() in SHADER_READ_ONLY_OPTIMAL. `alpha` is the current frame's blend weight
    // (1 ignores history); `reprojection` maps current jittered NDC to the
    // previous frame's clip space (row-vector order); upscaling, unjittered
    // NDC, and `offset` is where this frame's samples are from their input
    // pixels' centres (input pixels).
    void Record(VkCommandBuffer cmd, const float reprojection[16], float alpha, const float offset[2]);

    // Film accumulation instead (shaders/film.comp), same layouts: adds
    // this sample, its pixels' samples `offset` pixels from their centres,
    // through a Gaussian of `radius` pixels to the fp32 sums and resolves
    // their average. `mode` 0 adds, 1 starts over, 2 starts over from the
    // history (the TAA image) at `seed` samples' weight. Adaptive with a
    // `threshold` > 0: pixels whose mean's standard error (of the luma,
    // times `exposure`, compressed) fell under it after `minSamples` take
    // no more samples; ActivePixels() counts the others.
    void RecordFilm(VkCommandBuffer cmd, const float offset[2], float radius, uint32_t mode, float seed,
                    float threshold = 0.0f, uint32_t minSamples = 0, float exposure = 1.0f);
    // After the frame's fence: pixels the last adaptive film still sampled.
    uint32_t ActivePixels() const { return *static_cast<const uint32_t *>(_active.mapped); }

    VkImageView Output() const { return _resolved.view; }
    VkImage OutputImage() const { return _resolved.image; }
    // Last frame's resolve, in SHADER_READ_ONLY_OPTIMAL between frames.
    // Its view has mips; they hold the resolve's averages over 2^m pixels
    // only while SetHistoryMips is on (SSR's footprint-filtered hits),
    // else stale data (samplers with maxLod 0 never see them).
    VkImageView History() const { return _history.view; }
    // Whether the next Record/RecordFilm builds the history's mips.
    void SetHistoryMips(bool on) { _mipsOn = on; }

private:
    void _Destroy();
    void _CopyToHistory(VkCommandBuffer cmd, VkImageLayout layout, VkPipelineStageFlags2 stage,
                        VkAccessFlags2 access);

    Device &_dev;
    VkSampler _nearest = VK_NULL_HANDLE, _linear = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkPipeline _upscale = VK_NULL_HANDLE;  // taa.frag built with UPSCALE
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    Image _resolved, _history;
    uint32_t _historyMips = 1;
    bool _mipsOn = false;
    // Film: compute pipeline, set and its fp32 sums (colour * weight; weight), GENERAL.
    VkDescriptorSetLayout _filmSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout _filmLayout = VK_NULL_HANDLE;
    VkPipeline _film = VK_NULL_HANDLE;
    VkDescriptorSet _filmSet = VK_NULL_HANDLE;
    Image _sums, _weights;
    Buffer _active;  // film.comp's count of pixels still sampling (host-visible)
    uint32_t _width = 0, _height = 0;
    uint32_t _inWidth = 0, _inHeight = 0;  // SetInput's
};

} // namespace vkr
