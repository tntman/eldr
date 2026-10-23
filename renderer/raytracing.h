#pragma once

#include "device.h"
#include "scene.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace vkr {

// Hardware ray tracing (Device::rayTracing): a BLAS per rigid opaque or
// cutout mesh, a TLAS over the instance pool, and the passes tracing them.
// Pascal has ray tracing pipelines (NVIDIA's compute fallback) but no ray
// queries, so fragment shaders can't trace: per pixel shadows and
// reflections are traced before shading into images mesh.frag reads
// (shaders/gi.glsl), next to DDGI probes for global illumination.
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
        uint32_t shadowLights[4] = {kNone, kNone, kNone, kNone};  // per channel of the shadow image
        uint32_t shadowSamples = 1, reflectionSamples = 1;          // rays per light / pixel
        uint32_t sun = kNone;  // the distant light shadowed at hits
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

    // Graphics memory of the acceleration structures (with their scratch
    // and instances).
    VkDeviceSize AccelerationStructureBytes() const;

private:
    struct Functions;
    struct Blas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        uint32_t block = 0, offset = 0, units = 0;  // in _blocks, kUnit bytes each
        VkDeviceAddress address = 0;
    };
    struct Block {
        Buffer buffer;
        RangeAllocator ranges;
    };
    void _BuildBlases(const Scene::Gpu &gpu);
    void _DestroyBlas(Blas &blas);
    void _PrepareTlas(const Scene::Gpu &gpu);
    VkAccelerationStructureBuildGeometryInfoKHR _TlasBuildInfo(VkAccelerationStructureGeometryKHR *geometry) const;
    VkDeviceAddress _Address(VkBuffer buffer) const;
    void _ReserveScratch(VkDeviceSize bytes);
    Image _CreateAtlas(VkFormat format, uint32_t width, uint32_t height);  // storage + sampled, GENERAL, zero
    void _CreateImages(uint32_t width, uint32_t height);
    void _CreatePipelines(VkDescriptorSetLayout sceneLayout);

    Device &_dev;
    std::unique_ptr<Functions> _fn;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR _props{};
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;  // set 1: buffers and images only passes here write
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;  // ray tracing: shaders/rt*
    VkPipeline _tlasPipeline = VK_NULL_HANDLE;
    Buffer _sbt;  // shader binding table: raygen, miss and hit regions
    VkStridedDeviceAddressRegionKHR _raygen{}, _miss{}, _hit{}, _callable{};

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
    bool _tlasDirty = true;
    uint64_t _tlasVersion = 0;  // Scene::Gpu::version it was built from
    std::vector<uint32_t> _slotWords;  // what _slots holds

    Work _work;
    uint32_t _frame = 0;
    Buffer _uniforms;  // RayTracingData
    Image _shadowImage, _reflectionImage;
    uint32_t _width = 0, _height = 0;
    Image _giIrradiance, _giVisibility;
    VkSampler _sampler = VK_NULL_HANDLE;
};

} // namespace vkr
