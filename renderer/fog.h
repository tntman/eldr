#pragma once

#include "device.h"

namespace vkr {

// Volumetric fog (Wronski 2014, Hillaire 2015): a froxel grid over the view
// frustum (shaders/fog.glsl). fog_inject.comp evaluates the medium and the
// light it scatters towards the camera per froxel (the sun with its
// cascades, local lights from the clusters with their atlas tiles, the
// dome's average), at a depth jittered per frame and blended with last
// frame's grid reprojected; fog_integrate.comp accumulates it front to back
// into the volume the shading pass and the sky read (in-scattered light,
// transmittance).
class Fog {
public:
    explicit Fog(Device &device);
    ~Fog();
    Fog(const Fog &) = delete;
    Fog &operator=(const Fog &) = delete;

    // Compute pipelines in the renderer's layout (set 0: bindings 47 and 48
    // are the fog's, see Descriptors).
    void CreatePipelines(VkPipelineLayout layout);

    // (Re)creates the volumes for a grid of x * y * z froxels; returns
    // whether last frame's grid is usable as history (same size, recorded
    // last frame). Before Descriptors, once per frame with fog on.
    bool Resize(uint32_t x, uint32_t y, uint32_t z);

    // Binding 47: this frame's injected grid, last frame's, the integrated
    // volume (storage images, GENERAL); binding 48: the integrated volume
    // sampled with `sampler`. Without fog: 1^3 volumes of no fog.
    void Descriptors(VkDescriptorImageInfo storage[3], VkDescriptorImageInfo *volume, VkSampler sampler) const;

    // Inject and integrate; the shadow maps must be readable by compute.
    // Leaves the volume readable by fragment shaders.
    void Record(VkCommandBuffer cmd, VkPipelineLayout layout, VkDescriptorSet set);

    // Next frame's history is invalid (fog was off this frame).
    void Skip() { _recorded = false; }

    // Where in its slice this frame samples the medium (0..1, a golden
    // ratio sequence over the frames).
    float Jitter() const;

private:
    void _Destroy();
    Image _Create(uint32_t x, uint32_t y, uint32_t z);

    Device &_dev;
    VkPipeline _inject = VK_NULL_HANDLE, _integrate = VK_NULL_HANDLE;
    Image _injected[2];  // RGBA16F: in-scattering, extinction (ping-pong: this frame's, history)
    Image _volume;       // RGBA16F: in-scattered light, transmittance from the camera
    uint32_t _x = 0, _y = 0, _z = 0;
    uint32_t _current = 0;     // _injected[_current] is written this frame
    uint32_t _frames = 0;      // Records so far (Jitter)
    bool _recorded = false;    // the last frame recorded into _injected[_current ^ 1]
};

} // namespace vkr
