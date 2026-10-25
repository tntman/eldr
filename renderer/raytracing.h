#pragma once

#include "device.h"
#include "scene.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vkr {

// std140 mirror of RayTracingData in shaders/gi.glsl.
// Traced shadows: the sun and up to 15 local lights, four per layer of the
// shadow image (stacked down it, `shadowSample` w rows apart).
constexpr uint32_t kMaxRtShadowLights = 16;
struct RayTracingData {
    uint32_t shadowLights[kMaxRtShadowLights];
    uint32_t params[4];  // frame, shadow rays per light, reflection rays, light shadowed at hits
    float giOrigin[4];
    uint32_t giCounts[4];
    float giParams[4];
    float giRotation[16];
    uint32_t skinned[4];  // the TLAS's first skinned instance, how many
    float shadowSample[4];  // xy: this sample's point on the light shapes (sample dimensions), w: layer stride
};

// Hardware ray tracing (Device::rayTracing): a BLAS per rigid opaque or
// cutout mesh, a TLAS over the instance pool, and the passes tracing them.
// Pascal has ray tracing pipelines (NVIDIA's compute fallback) but no ray
// queries, so fragment shaders can't trace: per pixel shadows and
// reflections are traced before shading into images mesh.frag reads
// (shaders/gi.glsl), next to DDGI probes for global illumination: a grid
// over the scene's bounds whose rays (rt_probes.rgen) are blended into
// per-probe irradiance and visibility (gi_update.comp).
class RayTracing {
public:
    // Set 0 of every pass here is the renderer's scene set (`sceneLayout`).
    RayTracing(Device &device, VkDescriptorSetLayout sceneLayout);
    ~RayTracing();
    RayTracing(const RayTracing &) = delete;
    RayTracing &operator=(const RayTracing &) = delete;

    // What to trace this frame.
    struct Work {
        bool shadows = false;      // traced shadows of shadowLights
        bool reflections = false;  // traced reflections
        uint32_t shadowLights[kMaxRtShadowLights] = {};  // per channel of the shadow image, kNone after the last
        float shadowSample[2] = {};  // the sample dimensions' point on the light shapes (frame.sampleInfo.x)
        uint32_t shadowSamples = 1, reflectionSamples = 1;          // rays per light / pixel
        uint32_t sun = kNone;  // the distant light shadowed at hits
        bool gi = false;            // global illumination probes
        uint32_t giProbes = 16;     // along the scene's longest side
        uint32_t giRays = 64;       // per probe and frame
        float giHysteresis = 0.97f; // of the probes' running average
        bool giBurst = false;       // fresh probes: all their settling updates in this frame
        // Skinned instances that block rays (Scene::LargestSkinned), in TLAS
        // room for skinnedCapacity of them.
        std::vector<Scene::SkinnedInstance> skinned;
        uint32_t skinnedCapacity = 0;
        // The instances moved without a new Scene::Gpu::version (motion
        // blur time samples): the TLAS is rebuilt.
        bool moving = false;
        // Displaced meshes' BLASes from their displaced vertices
        // (rt_displace.comp; Scene::Gpu::SlotGeometry::displaced) at this
        // displacement scale; 0: from their rest vertices.
        float displacement = 0.0f;
    };
    // Before recording, with the GPU idle: builds the BLASes the scene lacks
    // (in submits of their own), frees unused ones, sizes the TLAS and the
    // images for `width` x `height`, and writes this frame's uniforms.
    void Prepare(const Scene::Gpu &gpu, const Work &work, uint32_t width, uint32_t height);
    // Writes bindings 52..59 of the scene set (see shaders/rt.glsl, gi.glsl).
    void UpdateDescriptors(VkDescriptorSet set, const Scene::Gpu &gpu);
    // Records the TLAS build (when the scene changed) and the traces;
    // `pass` closes a GPU timestamp after each part.
    void Record(VkCommandBuffer cmd, VkDescriptorSet set, const Scene::Gpu &gpu,
                const std::function<void(const char *)> &pass);

    // Frees the acceleration structures (when ray tracing is switched off).
    void Release();

    // False while fresh GI probes settle (their first 1 / (1 - hysteresis)
    // frames, from switching GI on or a new probe grid).
    bool Converged() const;

    // Graphics memory of the acceleration structures (with their scratch
    // and instances).
    VkDeviceSize AccelerationStructureBytes() const;

private:
    struct Functions;
    struct Blas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        uint32_t block = 0, offset = 0, units = 0;  // in _blocks, kUnit bytes each
        VkDeviceAddress address = 0;
        float displacement = 0.0f;  // the Work::displacement it was built with (displaced meshes)
    };
    // A displaced BLAS Record builds: from the slot's rest vertices displaced
    // into _displacedVertices at firstVertex.
    struct DisplacedBuild {
        VkAccelerationStructureKHR as;
        uint32_t slot, firstVertex;
        VkDeviceSize scratch;  // offset in the scratch buffer
    };
    struct Block {
        Buffer buffer;
        RangeAllocator ranges;
    };
    // Skinned instances (rt_skin.comp poses their vertices): a BLAS each,
    // built once and refit every frame.
    struct Skinned {
        Blas blas;
        uint64_t version = 0;  // of its mesh's geometry
        uint32_t slot = 0, firstVertex = 0, vertexCount = 0, flags = 0;
        VkDeviceSize buildScratch = 0, updateScratch = 0;
        bool built = false;
    };
    Blas _CreateBlas(VkDeviceSize bytes);
    VkAccelerationStructureGeometryKHR _Triangles(VkDeviceAddress vertices, VkDeviceSize stride, uint32_t vertexCount,
                                                  VkDeviceAddress indices) const;
    void _BuildBlases(const Scene::Gpu &gpu);
    void _DestroyBlas(Blas &blas);
    VkDeviceSize _PrepareSkinned(const Scene::Gpu &gpu);
    void _PrepareDisplaced(const Scene::Gpu &gpu, const std::vector<uint32_t> &slots);
    void _DestroySkinned(Skinned &s);
    VkAccelerationStructureBuildGeometryInfoKHR _SkinnedBuildInfo(const Skinned &s, const Scene::Gpu &gpu,
                                                                  VkAccelerationStructureGeometryKHR *geometry) const;
    void _PrepareTlas(const Scene::Gpu &gpu);
    VkAccelerationStructureBuildGeometryInfoKHR _TlasBuildInfo(VkAccelerationStructureGeometryKHR *geometry) const;
    VkDeviceAddress _Address(VkBuffer buffer) const;
    void _ReserveScratch(VkDeviceSize bytes);
    Image _CreateAtlas(VkFormat format, uint32_t width, uint32_t height);  // storage + sampled, GENERAL, zero
    void _CreateImages(uint32_t width, uint32_t height, uint32_t shadowLayers);
    void _CreatePipelines(VkDescriptorSetLayout sceneLayout);
    void _FitProbes(const Scene::Gpu &gpu);

    Device &_dev;
    std::unique_ptr<Functions> _fn;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR _props{};
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;  // set 1: buffers and images only passes here write
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;  // ray tracing: shaders/rt*
    VkPipeline _tlasPipeline = VK_NULL_HANDLE;
    VkPipeline _skinPipeline = VK_NULL_HANDLE;  // rt_skin.comp
    VkPipeline _displacePipeline = VK_NULL_HANDLE;  // rt_displace.comp
    Buffer _sbt;  // shader binding table: raygen, miss and hit regions
    VkStridedDeviceAddressRegionKHR _raygen{}, _raygenProbes{}, _miss{}, _hit{}, _callable{};

    // BLASes by Scene::Gpu::SlotGeometry::version, sub-allocated from blocks.
    std::unordered_map<uint64_t, Blas> _blas;
    std::vector<Block> _blocks;
    Buffer _scratch;  // BLAS batches and the TLAS build
    VkDeviceSize _scratchAlignment = 1;
    // The TLAS over every instance slot, rebuilt when the scene or its
    // BLASes change.
    Buffer _slots;  // RtSlot per mesh slot
    Buffer _instances, _tlasBuffer;
    VkAccelerationStructureKHR _tlas = VK_NULL_HANDLE;
    uint32_t _tlasCapacity = 0;
    VkDeviceSize _tlasScratch = 0;  // its build's scratch
    bool _tlasDirty = true;
    uint64_t _tlasVersion = 0;  // Scene::Gpu::version it was built from
    std::vector<uint32_t> _slotWords;  // what _slots holds
    // Skinned instances by instance slot, this frame's in table order.
    std::unordered_map<uint32_t, Skinned> _skinned;
    std::vector<uint32_t> _skinnedOrder;
    std::vector<VkDeviceSize> _skinnedScratch;  // offsets of their builds' scratch
    uint32_t _skinnedMostVertices = 0, _skinnedLast = 0;  // this frame; last frame's count
    Buffer _skinnedVertices;    // posed position and normal per vertex
    RangeAllocator _skinnedRanges;
    Buffer _skinnedTable;       // RtSkinned per entry (rt.glsl)
    // Displaced BLASes to build this frame, their displaced vertices
    // (3 floats each) and table (RtDisplaced, rt_displace.comp).
    std::vector<DisplacedBuild> _displacedBuilds;
    uint32_t _displacedMostVertices = 0;
    Buffer _displacedVertices, _displacedTable;

    Work _work;
    uint32_t _frame = 0;
    Buffer _uniforms;  // RayTracingData
    Image _shadowImage, _reflectionImage;
    uint32_t _width = 0, _height = 0, _shadowLayers = 0;
    Image _giIrradiance, _giVisibility;
    VkSampler _sampler = VK_NULL_HANDLE;
    VkPipeline _giUpdate[2] = {};  // gi_update.comp for irradiance, visibility
    Buffer _giRays;                // this frame's probe rays
    // The probe grid; a new one starts the probes over.
    float _giOrigin[3] = {}, _giSpacing = 0;
    uint32_t _giCounts[3] = {}, _giPerRow = 1, _giAlong = 0;
    uint32_t _giFrames = 0;  // since the probes started over
    std::vector<RayTracingData> _giBurst;  // uniforms of the extra updates this frame
};

} // namespace vkr
