#pragma once

#include "device.h"

#include <limits>
#include <map>
#include <memory>
#include <string>
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

// A volume's material: coefficients per unit of the density field (as
// MaterialX's anisotropic_vdf and Arnold's standard_volume give them), the
// fields that play each role, and how temperature turns into blackbody
// emission. The defaults are what a volume without a material renders as.
// The Hydra layer may update it in place, as Material.
struct VolumeShading {
    float scattering[3] = {0.8f, 0.8f, 0.8f};  // sigma_s per unit of density
    float absorption[3] = {0.2f, 0.2f, 0.2f};  // sigma_a per unit of density
    float densityScale = 1.0f;                  // times the density field
    // Henyey-Greenstein g; NaN: the renderer's setting (no material).
    float anisotropy = std::numeric_limits<float>::quiet_NaN();
    float emission[3] = {0, 0, 0};  // radiance per unit length, per unit of density
    float emissionScale = 1.0f;     // times the emission field (radiance per unit length)
    float emissionTint[3] = {1, 1, 1};  // times the emission field too
    // Blackbody emission from the temperature field: Kelvin per unit of the
    // field (0: guess: Kelvin above 100, else normalized as Houdini's pyro,
    // 1 = 5000 K), and the radiance per unit length of a luminance-1
    // blackbody colour, times (K / 1500)^4 when `physical` (else as
    // MaterialX's blackbody node: luminance 1 at any temperature).
    float kelvinScale = 0.0f;
    float blackbodyIntensity = 1.0f;
    bool physical = true;
    // The volume's fields (Volume::fields) by role; empty: none.
    std::string densityField = "density", temperatureField = "temperature", colorField = "Cd",
                emissionField = "emission";
    // Read from a volume material. Without one the roles other than density
    // are only read with VolumePass::Settings::defaultFields.
    bool material = false;
};

// A heterogeneous medium: the density field's extinction, the colour
// field's tint of the scattering, blackbody emission from temperature and
// the emission field's radiance, all sampled in the density grid's index
// space. Same threading rules as Mesh.
struct Volume {
    // By the volume's field binding names (UsdVol: field:density ...); the
    // shading picks them by role. Without its density field, the first one
    // is the density.
    std::map<std::string, std::shared_ptr<VolumeGrid>> fields;
    std::shared_ptr<const VolumeShading> shading;  // null: VolumeShading's defaults
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
    float params[4];            // x: temperature to Kelvin, y: blackbody intensity, z: 1 = physical
    float scattering[4];        // rgb: sigma_s per unit of density field; w: Henyey-Greenstein g
    float absorption[4];        // rgb: sigma_a per unit of density field
    float medium[4];            // x: extinction (mean sigma_t) per unit of density field; y: 1 = scattering.w
                                // is the material's g (else the setting); z: 1 = no volume material
    float emission[4];          // rgb: radiance per unit length per unit of density field
    float fieldEmission[4];     // rgb: times the emission field (scale times tint)
};
static_assert(sizeof(GpuVolume) % 16 == 0, "std430 array stride of GpuVolume");

// Raymarches the scene's volumes over the shaded image: after the opaque
// and translucent passes, before depth of field and TAA, see volume.frag.
// At half resolution it marches a quarter of the pixels into its own
// target, then upsamples that over the image, weighting the four nearest
// by how close their depth is to the pixel's.
class VolumePass {
public:
    // `sceneLayout`: the renderer's set (frame data, lights, shadows,
    // clusters; volume bindings 63..66).
    VolumePass(Device &device, VkDescriptorSetLayout sceneLayout);
    ~VolumePass();
    VolumePass(const VolumePass &) = delete;
    VolumePass &operator=(const VolumePass &) = delete;

    struct Settings {
        float stepSize = 1.0f;      // density voxels per step
        uint32_t maxSteps = 256;    // per pixel; longer rays take longer steps
        uint32_t shadowSteps = 8;   // towards each distant light (half as many up to the dome)
        float density = 1.0f;       // extinction multiplier (on the material's)
        float emission = 1.0f;      // emission multiplier (on the material's)
        float anisotropy = 0.2f;    // Henyey-Greenstein g of volumes whose material has none
        uint32_t resolution = 1;    // 1: every pixel, 2: half resolution, upsampled
        // Under transparency: with translucent meshes in view (and the depth
        // prepass), the part of each ray behind the nearest translucent
        // surface is marched before the translucency pass, which then dims
        // and refracts it, and the part in front after it.
        bool underTransparency = true;
        // Shadows: the sun's light through the volumes onto surfaces
        // (RecordShadow into Transparency's sun tint map).
        bool shadows = true;
        // Volumes shadow each other: the light towards each step of a
        // volume's march also crosses the other volumes (sun and local lights).
        bool mutualShadows = true;
        // Volumes without a volume material read their temperature, Cd
        // (colour) and emission fields with VolumeShading's defaults; off:
        // only density, as Karma renders them.
        bool defaultFields = false;
    };

    // Which part of each ray Record marches: all of it, the part behind
    // the nearest translucent surface (TranslucentDepth()), or in front.
    enum Part : uint32_t { kAll, kBehind, kInFront };

    // A depth target for the nearest translucent surfaces, `width` x
    // `height` (made on first use and on resizes; the GPU must be idle),
    // bound as binding 67 for kBehind and kInFront; null without one yet.
    const Image &EnsureTranslucentDepth(uint32_t width, uint32_t height);
    VkImageView TranslucentDepth() const { return _translucentDepth.view; }

    // The half resolution target for a `width` x `height` frame (made on
    // first use and on resizes; the GPU must be idle), bound as binding 66;
    // a null view without one yet.
    VkImageView EnsureHalf(uint32_t width, uint32_t height);
    VkImageView Half() const { return _half.view; }

    // Blends the volumes over `color` (COLOR_ATTACHMENT_OPTIMAL, stays so);
    // `set` must hold the scene's depth (binding 63) in
    // DEPTH_READ_ONLY_OPTIMAL and, at half resolution, Half() (66). Returns
    // false (records nothing) without volumes.
    bool Record(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, VkImageView color,
                uint32_t width, uint32_t height, const Settings &settings, Part part = kAll);

    // Inside rendering into one layer of Transparency's sun tint map (just
    // cleared, `mapSize` texels square, viewport set) for frame cascade
    // `cascade`: multiplies the colour by the volumes' transmittance along
    // the light and writes the depth of their optical depth's mean, before
    // the translucent casters (which keep the nearer depth). Binds `set`
    // with its own layout: rebind the renderer's after.
    void RecordShadow(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, uint32_t cascade,
                      uint32_t mapSize, const Settings &settings);
    // The same for a local light's atlas tile (index `tile` of the
    // renderer's ShadowTiles): rendering into the tint atlas with the
    // viewport and scissor on its rectangle (x, y, size texels).
    void RecordTileShadow(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, uint32_t tile, uint32_t x,
                          uint32_t y, uint32_t size, const Settings &settings);

    // Frame::lightShapesVisible: the shapes of lights [first, first + count)
    // of the renderer's light buffer over `color` (as Record, with the
    // scene's depth at binding 63).
    void RecordLightShapes(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t first, uint32_t count,
                           VkImageView color, uint32_t width, uint32_t height);

private:
    Device &_dev;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;   // full resolution, blended over the image
    VkPipeline _halfPipeline = VK_NULL_HANDLE, _upsamplePipeline = VK_NULL_HANDLE;
    VkPipeline _shadowPipeline = VK_NULL_HANDLE;  // volume_shadow.frag into the tint map
    VkPipeline _shapePipeline = VK_NULL_HANDLE;   // light_shapes.frag over the image
    Image _half;
    uint32_t _halfWidth = 0, _halfHeight = 0;
    Image _translucentDepth;
    uint32_t _depthWidth = 0, _depthHeight = 0;
};

} // namespace vkr
