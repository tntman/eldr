#pragma once

#include "device.h"

#include <array>
#include <functional>
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
    // The Hydra layer's record of what `words` came from (a file's path,
    // grid and time; the grid object), so the same again isn't converted.
    std::string source;
    std::weak_ptr<const void> origin;
};

// A volume's material: coefficients per unit of the density field (as
// MaterialX's anisotropic_vdf and Arnold's standard_volume give them), the
// fields that play each role, and how temperature turns into blackbody
// emission. The defaults are what a volume without a material renders as.
// The Hydra layer may update it in place, as Material.
struct VolumeShading {
    // Without a material, Karma's: white scattering, no absorption, isotropic
    // (measured: Karma CPU renders such a volume as anisotropic_vdf(0, density, 0)).
    float scattering[3] = {1.0f, 1.0f, 1.0f};  // sigma_s per unit of density
    float absorption[3] = {0.0f, 0.0f, 0.0f};  // sigma_a per unit of density
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
    // Functions of one float field each (a material's remaps and ramps),
    // baked into lookup tables over the field's value range; the masks say
    // which multiply scattering, absorption and emission (bit k: factors[k];
    // kDensity: the density field itself, as the coefficients above are per
    // unit of it). See Scene::_CommitVolumes.
    struct Factor {
        std::string field;
        std::function<std::array<float, 3>(float)> value;
    };
    static constexpr uint32_t kMaxFactors = 3, kDensity = 8;
    std::vector<Factor> factors;
    uint32_t scatteringFactors = kDensity, absorptionFactors = kDensity, emissionFactors = kDensity;
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
    // Karma's Volume Limit on this volume (primvars:eldr:object:volumelimit,
    // else karma:object:volumelimit): above 0 the frame scatters more than
    // once, as Settings::volumeLimit (the largest of the frame's).
    float volumeLimit = 0.0f;
    // Velocity blur (Karma's Volume Velocity Blur, its Lookup Offset mode):
    // in motion blur time samples the fields are read at p - vel * t, t
    // the sample's offset from this frame in frames. Object units per frame
    // per unit of the velocity field (vel, else v: a Vec3f grid, or three
    // float grids vel.x/.y/.z or vel_x/_y/_z); 0: none.
    float velocityScale = 0.0f;
    uint32_t tag = 1;  // render tag bit, as Mesh::tag
    std::vector<uint32_t> linkCategories;  // light linking, as Mesh::linkCategories

private:
    friend class Scene;
    size_t _slot = 0;
};

// std430 mirror of Volume in shaders/volume.frag: one per drawn instance.
struct GpuVolume {
    float worldToIndex[3][4];   // density index = rows . (world, 1)
    float indexMin[4];          // density index-space box around the active voxels; w: light link mask (bits)
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
    uint32_t factorMasks[4];    // VolumeShading's masks (scattering, absorption, emission); w: roles read
                                // (1 temperature as blackbody, 2 colour tint, 4 emission field)
    uint32_t lutOffsets[4];     // factors' tables in the grid pool (32-bit words; kNone: none); w: the extinction's (rgb, mean)
    uint32_t lutSlots[4];       // the grid (0..3, as grids) each factor reads
    float lutLow[4];            // table i = (value - low) * scale, 0..kVolumeLut - 1
    float lutScale[4];
    uint32_t velocity[4];       // velocity grids: xyz the Vec3f grid's (x) or the three float grids'; w: 0 none,
                                // 1 a Vec3f grid, 3 three float grids
    float toVelocity[3][4];     // density index -> velocity index (rows)
    float advect[3][4];         // velocity value -> density index offset per frame (rows, w 0)
};
constexpr uint32_t kVolumeLut = 1024;  // entries (vec4) per factor table
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
        float anisotropy = 0.0f;    // Henyey-Greenstein g of volumes without a material (Karma's: isotropic)
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
        // Multiple scattering of distant lights and the dome, approximated
        // by octaves of weaker, less attenuated and less directional single
        // scattering (Wrenninge 2015), calibrated against Karma's Volume
        // Limit: volumeLimit bounces (0: single scattering, as Karma's
        // default); multipleScattering alone stands for kDefaultVolumeLimit.
        bool multipleScattering = false;
        float volumeLimit = 0.0f;
        static constexpr float kDefaultVolumeLimit = 16.0f;
        // Local lights' shadows (atlas tap, other volumes) every this many
        // march steps; the steps between scale the unshadowed light by the
        // last tap's shadowed fraction. 1: every step.
        uint32_t localShadowStride = 2;
        // Shadow grids: each volume's optical depth towards the dome, each
        // distant light and (with mutualShadows) each local light's other
        // volumes, at this many points per axis over its box, built once
        // per change (EnsureShadowGrid) and read trilinearly by the march
        // instead of a shadow ray per step and light. 0: march every ray.
        // Time samples of moving volumes (velocity blur) march.
        uint32_t shadowGrid = 64;
    };

    // Before the frame's descriptors: sizes the shadow grid (Settings::
    // shadowGrid) for `volumeCount` volumes and `distant` + `local` lights
    // (the GPU must be idle when it changes); `version` stands for what the
    // grid holds (the volumes, lights, dome; Scene::Gpu's stamps): the next
    // Record rebuilds it when that or the settings changed. `tables`: as
    // Record's `factors`.
    void EnsureShadowGrid(uint32_t volumeCount, uint32_t distant, uint32_t local, bool dome, const Settings &settings,
                          bool tables, uint64_t version);
    VkImageView ShadowGrid() const { return _grid.view; }  // bindings 93 (sampled) and 94 (storage), GENERAL

    // Which part of each ray Record marches: all of it; the part behind the
    // farthest translucent layer (TranslucentLayers()), drawn before the
    // translucency pass, which dims and refracts it; the part in front of
    // it, after that pass, each stretch between layers seen through the
    // layers in front of it; or (kThrough, under traced refraction, whose
    // glass replaces what's behind it) all of it after that pass, so.
    enum Part : uint32_t { kAll, kBehind, kInFront, kThrough };
    static constexpr uint32_t kMaxLayers = 4;

    // The camera's translucent layers, nearest first, peeled by the
    // Renderer (_RenderTranslucentDepth) for `width` x `height` frames:
    // `layers` (1 to kMaxLayers) layers of the colour each lets through and
    // its distance (rgb, a; a < 0: none), bound as binding 77; per layer
    // views to render into; the peel's depth attachment, and the last
    // layer's depth (binding 68) each peel reads. Made on first use and on
    // changes (the GPU must be idle); null views without them yet.
    static constexpr VkFormat kLayerFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    void EnsureTranslucentLayers(uint32_t width, uint32_t height, uint32_t layers);
    VkImageView TranslucentDepth() const { return _translucentDepth.view; }
    const Image &TranslucentDepthImage() const { return _translucentDepth; }
    const Image &PeelDepth() const { return _peelDepth; }
    VkImageView TranslucentLayers() const { return _layers.view; }
    const Image &TranslucentLayerImage() const { return _layers; }
    VkImageView TranslucentLayer(uint32_t layer) const { return _layerViews[layer]; }
    uint32_t TranslucentLayerCount() const { return _layerCount; }

    // The half resolution target for a `width` x `height` frame (made on
    // first use and on resizes; the GPU must be idle), bound as binding 66;
    // a null view without one yet.
    VkImageView EnsureHalf(uint32_t width, uint32_t height);
    VkImageView Half() const { return _half.view; }
    VkSampler HalfSampler() const { return _halfSampler; }  // nearest: Half() holds integers

    // Blends the volumes over `color` (COLOR_ATTACHMENT_OPTIMAL, stays so);
    // `set` must hold the scene's depth (binding 63) in
    // DEPTH_READ_ONLY_OPTIMAL and, at half resolution, Half() (66). Returns
    // false (records nothing) without volumes.
    // `factors`: a volume reads factor tables (VolumeShading::factors, or a
    // coefficient not per unit of density); without, a leaner shader runs.
    bool Record(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, VkImageView color,
                uint32_t width, uint32_t height, const Settings &settings, Part part = kAll, bool factors = true);

    // Inside rendering into one layer of Transparency's sun tint map (just
    // cleared, `mapSize` texels square, viewport set) for frame cascade
    // `cascade`: multiplies the colour by the volumes' transmittance along
    // the light and writes the depth of their optical depth's mean, before
    // the translucent casters (which keep the nearer depth). Binds `set`
    // with its own layout: rebind the renderer's after. shadowLink: the
    // light's shadow link bit (volumes outside it cast none; -1: all do).
    void RecordShadow(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, uint32_t cascade,
                      uint32_t mapSize, const Settings &settings, int32_t shadowLink = -1);
    // The same for a local light's atlas tile (index `tile` of the
    // renderer's ShadowTiles): rendering into the tint atlas with the
    // viewport and scissor on its rectangle (x, y, size texels).
    void RecordTileShadow(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, uint32_t tile, uint32_t x,
                          uint32_t y, uint32_t size, const Settings &settings, int32_t shadowLink = -1);

    // Frame::lightShapesVisible: the shapes of lights [first, first + count)
    // of the renderer's light buffer over `color` (as Record, with the
    // scene's depth at binding 63).
    void RecordLightShapes(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t first, uint32_t count,
                           VkImageView color, uint32_t width, uint32_t height);

private:
    Device &_dev;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    void _EnsurePipelines();
    // Full resolution, blended over the image, and half resolution; [1]:
    // with factor tables (volume.frag's kFactors).
    VkPipeline _pipeline[2] = {}, _halfPipeline[2] = {};
    VkPipeline _upsamplePipeline = VK_NULL_HANDLE;
    VkPipeline _shadowPipeline = VK_NULL_HANDLE;  // volume_shadow.frag into the tint map
    VkPipeline _shapePipeline = VK_NULL_HANDLE;   // light_shapes.frag over the image
    Image _half;
    VkSampler _halfSampler = VK_NULL_HANDLE;
    uint32_t _halfWidth = 0, _halfHeight = 0;
    // Shadow grid (volume_grid.comp): gridSize^3 cells per slot, gridLights
    // slots per volume, stacked along z; built for _gridWanted, holds _gridBuilt.
    Image _grid;
    VkPipelineLayout _gridLayout = VK_NULL_HANDLE;
    VkPipeline _gridPipeline = VK_NULL_HANDLE;
    uint32_t _gridSize = 0, _gridLights = 0, _gridCount = 0, _gridMutual = 0, _gridTables = 0;
    float _gridDensity = 0.0f;
    uint32_t _gridExtent[3] = {};
    uint64_t _gridWanted = 0, _gridBuilt = 0;
    void _BuildShadowGrid(VkCommandBuffer cmd, VkDescriptorSet set);
    Image _translucentDepth, _peelDepth, _layers;
    VkImageView _layerViews[kMaxLayers] = {};
    uint32_t _depthWidth = 0, _depthHeight = 0, _layerCount = 0;
    void _DestroyLayers();
};

} // namespace vkr
