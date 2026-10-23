#pragma once

#include "device.h"

#include <memory>
#include <vector>

namespace vkr {

class Scene;

// One NanoVDB grid: the bytes of a single grid as NanoVDB lays it out
// (GridData first, as a nanovdb::GridHandle's buffer holds it). Float grids
// for density and temperature, float or Vec3f for colour and emission. Its
// map takes index space to the volume's object space. The Hydra layer may
// replace the bytes in place (bump `version`); volumes keep pointing at it.
struct VolumeGrid {
    std::vector<uint32_t> words;  // 32-bit words, as the GPU reads them
    uint64_t version = 0;         // bump after changing `words`
};

// A heterogeneous medium from up to four grids sampled in the density
// grid's index space: extinction (density, per world unit), single
// scattering albedo (color, else kDefaultAlbedo), blackbody emission
// (temperature) and emitted radiance per world unit (emission). Same
// threading rules as Mesh.
struct Volume {
    static constexpr float kDefaultAlbedo = 0.8f;
    std::shared_ptr<VolumeGrid> density, temperature, color, emission;
    // One world transform per instance, 16 floats each, USD row-vector order.
    std::vector<float> transforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool visible = true;
    uint32_t tag = 1;  // render tag bit, as Mesh::tag

private:
    friend class Scene;
    size_t _slot = 0;
};

// std430 mirror of Volume in shaders/volume.frag: one per drawn instance.
struct GpuVolume {
    float worldToIndex[3][4];   // density index = rows . (world, 1)
    float indexMin[4];          // density index-space box around the active voxels
    float indexMax[4];
    uint32_t grids[4];          // byte offsets in the grid pool: density, temperature, color, emission (kNone: none)
    uint32_t types[4];          // their PNANOVDB_GRID_TYPE_*
    float toGrid[3][3][4];      // density index -> temperature, color, emission index (rows)
    float params[4];            // x: temperature to Kelvin
};
static_assert(sizeof(GpuVolume) % 16 == 0, "std430 array stride of GpuVolume");

// Raymarches the scene's volumes over the shaded image: after the opaque
// and translucent passes, before depth of field and TAA, see volume.frag.
class VolumePass {
public:
    // `sceneLayout`: the renderer's set (frame data, lights, shadows,
    // clusters; volume bindings 63..65).
    VolumePass(Device &device, VkDescriptorSetLayout sceneLayout);
    ~VolumePass();
    VolumePass(const VolumePass &) = delete;
    VolumePass &operator=(const VolumePass &) = delete;

    struct Settings {
        float stepSize = 1.0f;      // density voxels per step
        uint32_t maxSteps = 256;    // per pixel; longer rays take longer steps
        uint32_t shadowSteps = 8;   // towards each distant light (half as many up to the dome)
        float density = 1.0f;       // extinction multiplier
    };

    // Blends the volumes over `color` (COLOR_ATTACHMENT_OPTIMAL, stays so);
    // `set` must hold the scene's depth (binding 63) in
    // DEPTH_READ_ONLY_OPTIMAL. Returns false (records nothing) without volumes.
    bool Record(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, VkImageView color,
                uint32_t width, uint32_t height, const Settings &settings);

private:
    Device &_dev;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
};

} // namespace vkr
