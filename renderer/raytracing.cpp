#include "raytracing.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_set>

namespace vkr {

namespace {

const uint32_t kPixelsRgen[] =
#include "rt_pixels.rgen.inc"
;
const uint32_t kMiss[] =
#include "rt.rmiss.inc"
;
const uint32_t kShadowMiss[] =
#include "rt_shadow.rmiss.inc"
;
const uint32_t kClosestHit[] =
#include "rt.rchit.inc"
;
const uint32_t kAnyHit[] =
#include "rt.rahit.inc"
;
const uint32_t kTlasComp[] =
#include "tlas.comp.inc"
;
const uint32_t kSkinComp[] =
#include "rt_skin.comp.inc"
;
const uint32_t kProbesRgen[] =
#include "rt_probes.rgen.inc"
;
const uint32_t kGiUpdateComp[] =
#include "gi_update.comp.inc"
;

// GI probes: at most this many (the grid coarsens until it fits), atlas
// tiles per row, rays per probe (gi_update.comp's kMaxRays).
constexpr uint32_t kMaxProbes = 16384, kProbesPerRow = 128, kMaxProbeRays = 512;
constexpr int kIrradianceTexels = 6, kVisibilityTexels = 14;  // tile interiors, as gi.glsl

// std140 mirror of RayTracingData in shaders/gi.glsl.
struct RayTracingData {
    uint32_t shadowLights[4];
    uint32_t params[4];  // frame, shadow rays per light, reflection rays, light shadowed at hits
    float giOrigin[4];
    uint32_t giCounts[4];
    float giParams[4];
    float giRotation[16];
    uint32_t skinned[4];  // the TLAS's first skinned instance, how many
};

// RtSlot::flags (shaders/rt.glsl, tlas.comp).
constexpr uint32_t kSlotEmitter = 1, kSlotCutout = 2;

// BLASes are placed in 16 MB blocks at the required 256-byte alignment.
constexpr VkDeviceSize kUnit = 256;
constexpr VkDeviceSize kBlockBytes = VkDeviceSize(16) << 20;
// Scratch memory of one batch of BLAS builds.
constexpr VkDeviceSize kScratchBudget = VkDeviceSize(128) << 20;

VkDeviceSize AlignUp(VkDeviceSize x, VkDeviceSize a)
{
    return (x + a - 1) / a * a;
}

void MemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

// The extensions' entry points (the loader exports only core functions).
struct RayTracing::Functions {
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure;
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAddress;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild;
    PFN_vkCreateRayTracingPipelinesKHR createPipelines;
    PFN_vkGetRayTracingShaderGroupHandlesKHR getGroupHandles;
    PFN_vkCmdTraceRaysKHR cmdTraceRays;
};

RayTracing::RayTracing(Device &device, VkDescriptorSetLayout sceneLayout)
    : _dev(device)
    , _fn(std::make_unique<Functions>())
{
    auto load = [&](auto &fn, const char *name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(vkGetDeviceProcAddr(_dev.device, name));
        if (!fn) {
            throw std::runtime_error(std::string(name) + " missing");
        }
    };
    load(_fn->createAccelerationStructure, "vkCreateAccelerationStructureKHR");
    load(_fn->destroyAccelerationStructure, "vkDestroyAccelerationStructureKHR");
    load(_fn->getBuildSizes, "vkGetAccelerationStructureBuildSizesKHR");
    load(_fn->getAddress, "vkGetAccelerationStructureDeviceAddressKHR");
    load(_fn->cmdBuild, "vkCmdBuildAccelerationStructuresKHR");
    load(_fn->createPipelines, "vkCreateRayTracingPipelinesKHR");
    load(_fn->getGroupHandles, "vkGetRayTracingShaderGroupHandlesKHR");
    load(_fn->cmdTraceRays, "vkCmdTraceRaysKHR");

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    _props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    _props.pNext = &asProps;
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &_props};
    vkGetPhysicalDeviceProperties2(_dev.physical, &props);
    _props.pNext = nullptr;
    _scratchAlignment = std::max<VkDeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment, 1);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_sampler), "vkCreateSampler");
    _dev.Reserve(_uniforms, sizeof(RayTracingData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::memset(_uniforms.mapped, 0, sizeof(RayTracingData));
    _dev.Reserve(_slots, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    _dev.Reserve(_skinnedTable, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    _CreatePipelines(sceneLayout);
    // Placeholders until a frame needs the real sizes: every descriptor
    // stays valid.
    _CreateImages(1, 1);
    _giIrradiance = _CreateAtlas(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8);
    _giVisibility = _CreateAtlas(VK_FORMAT_R32G32_SFLOAT, 16, 16);
}

RayTracing::~RayTracing()
{
    vkDeviceWaitIdle(_dev.device);
    Release();
    for (Image *img : {&_shadowImage, &_reflectionImage, &_giIrradiance, &_giVisibility}) {
        _dev.Destroy(*img);
    }
    _dev.Destroy(_uniforms);
    _dev.Destroy(_slots);
    _dev.Destroy(_sbt);
    _dev.Destroy(_giRays);
    _dev.Destroy(_skinnedTable);
    for (VkPipeline p : {_pipeline, _tlasPipeline, _skinPipeline, _giUpdate[0], _giUpdate[1]}) {
        vkDestroyPipeline(_dev.device, p, nullptr);
    }
    vkDestroyPipelineLayout(_dev.device, _layout, nullptr);
    vkDestroyDescriptorPool(_dev.device, _pool, nullptr);
    vkDestroyDescriptorSetLayout(_dev.device, _setLayout, nullptr);
    vkDestroySampler(_dev.device, _sampler, nullptr);
}

void RayTracing::Release()
{
    for (auto &[version, blas] : _blas) {
        _fn->destroyAccelerationStructure(_dev.device, blas.as, nullptr);
    }
    _blas.clear();
    for (auto &[instance, s] : _skinned) {
        _fn->destroyAccelerationStructure(_dev.device, s.blas.as, nullptr);
    }
    _skinned.clear();
    _skinnedOrder.clear();
    _skinnedRanges = RangeAllocator();
    _dev.Destroy(_skinnedVertices);
    for (Block &b : _blocks) {
        _dev.Destroy(b.buffer);
    }
    _blocks.clear();
    _fn->destroyAccelerationStructure(_dev.device, _tlas, nullptr);
    _tlas = VK_NULL_HANDLE;
    _tlasCapacity = 0;
    _tlasDirty = true;
    _slotWords.clear();
    for (Buffer *b : {&_scratch, &_instances, &_tlasBuffer}) {
        _dev.Destroy(*b);
    }
    _work = Work();
}

bool RayTracing::Converged() const
{
    return !_work.gi || _giFrames >= std::min(uint32_t(1.0f / (1.0f - _work.giHysteresis)), 100u);
}

void RayTracing::_CreatePipelines(VkDescriptorSetLayout sceneLayout)
{
    const VkDevice device = _dev.device;
    // Set 1, what only these passes write: the TLAS's instances, the probes'
    // rays and atlases, the skinned instances' posed vertices and table
    // (partially bound: written once they exist).
    const VkShaderStageFlags compute = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSetLayoutBinding bindings[6] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, compute},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, compute | VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, compute},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, compute},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, compute | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, compute | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
    };
    VkDescriptorBindingFlags partial[6];
    std::fill(partial, partial + 6, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 6;
    flags.pBindingFlags = partial;
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags};
    slci.bindingCount = 6;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(device, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(device, &dpci, nullptr, &_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = _pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &_setLayout;
    Check(vkAllocateDescriptorSets(device, &dsai, &_set), "vkAllocateDescriptorSets");

    const VkDescriptorSetLayout layouts[2] = {sceneLayout, _setLayout};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 2;
    lci.pSetLayouts = layouts;
    Check(vkCreatePipelineLayout(device, &lci, nullptr, &_layout), "vkCreatePipelineLayout");

    VkResult r = VK_SUCCESS;
    for (auto [code, bytes, pipeline] : {std::tuple{kTlasComp, sizeof(kTlasComp), &_tlasPipeline},
                                         std::tuple{kSkinComp, sizeof(kSkinComp), &_skinPipeline}}) {
        VkShaderModule module = CreateShaderModule(device, code, bytes);
        VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                     module, "main"};
        cci.layout = _layout;
        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cci, nullptr, pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        Check(r, "vkCreateComputePipelines");
    }
    VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                 VK_NULL_HANDLE, "main"};
    cci.layout = _layout;
    // The probe update, specialized for each atlas's tile interiors.
    VkShaderModule giModule = CreateShaderModule(device, kGiUpdateComp, sizeof(kGiUpdateComp));
    cci.stage.module = giModule;
    const VkSpecializationMapEntry entry{0, 0, sizeof(int32_t)};
    for (int i = 0; i < 2 && r == VK_SUCCESS; ++i) {
        const int32_t texels = i == 0 ? kIrradianceTexels : kVisibilityTexels;
        const VkSpecializationInfo spec{1, &entry, sizeof(texels), &texels};
        cci.stage.pSpecializationInfo = &spec;
        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cci, nullptr, &_giUpdate[i]);
    }
    vkDestroyShaderModule(device, giModule, nullptr);
    Check(r, "vkCreateComputePipelines");

    // Shader groups: the ray generation shaders, misses (closest surface,
    // shadow), hit groups (closest surface: closest hit + cutout any hit;
    // shadow: the any hit only). Rays pick the shadow ones by SBT offset 1.
    struct Stage {
        const uint32_t *code;
        size_t bytes;
        VkShaderStageFlagBits stage;
    };
    const Stage stages[] = {
        {kPixelsRgen, sizeof(kPixelsRgen), VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {kMiss, sizeof(kMiss), VK_SHADER_STAGE_MISS_BIT_KHR},
        {kShadowMiss, sizeof(kShadowMiss), VK_SHADER_STAGE_MISS_BIT_KHR},
        {kClosestHit, sizeof(kClosestHit), VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR},
        {kAnyHit, sizeof(kAnyHit), VK_SHADER_STAGE_ANY_HIT_BIT_KHR},
        {kProbesRgen, sizeof(kProbesRgen), VK_SHADER_STAGE_RAYGEN_BIT_KHR},
    };
    constexpr uint32_t kStages = sizeof(stages) / sizeof(stages[0]);
    VkPipelineShaderStageCreateInfo stageInfos[kStages] = {};
    for (uint32_t i = 0; i < kStages; ++i) {
        stageInfos[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, stages[i].stage,
                         CreateShaderModule(device, stages[i].code, stages[i].bytes), "main"};
    }
    auto group = [](VkRayTracingShaderGroupTypeKHR type, uint32_t general, uint32_t closest, uint32_t any) {
        VkRayTracingShaderGroupCreateInfoKHR g{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        g.type = type;
        g.generalShader = general;
        g.closestHitShader = closest;
        g.anyHitShader = any;
        g.intersectionShader = VK_SHADER_UNUSED_KHR;
        return g;
    };
    constexpr uint32_t kUnused = VK_SHADER_UNUSED_KHR;
    const VkRayTracingShaderGroupCreateInfoKHR groups[] = {
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0, kUnused, kUnused),  // pixels
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 1, kUnused, kUnused),  // miss
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 2, kUnused, kUnused),  // shadow miss
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, kUnused, 3, 4),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR, kUnused, kUnused, 4),
        group(VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 5, kUnused, kUnused),  // probes
    };
    constexpr uint32_t kGroups = sizeof(groups) / sizeof(groups[0]);
    VkRayTracingPipelineCreateInfoKHR rci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    rci.stageCount = kStages;
    rci.pStages = stageInfos;
    rci.groupCount = kGroups;
    rci.pGroups = groups;
    rci.maxPipelineRayRecursionDepth = 1;  // only ray generation shaders trace
    rci.layout = _layout;
    r = _fn->createPipelines(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rci, nullptr, &_pipeline);
    for (const VkPipelineShaderStageCreateInfo &s : stageInfos) {
        vkDestroyShaderModule(device, s.module, nullptr);
    }
    Check(r, "vkCreateRayTracingPipelinesKHR");

    // Shader binding table: each region starts on the base alignment; the
    // raygen regions are one record each (pixels, probes), miss and hit
    // regions two records each.
    const uint32_t handleSize = _props.shaderGroupHandleSize;
    const VkDeviceSize stride = AlignUp(handleSize, _props.shaderGroupHandleAlignment);
    const VkDeviceSize base = _props.shaderGroupBaseAlignment;
    std::vector<uint8_t> handles(size_t(kGroups) * handleSize);
    Check(_fn->getGroupHandles(device, _pipeline, 0, kGroups, handles.size(), handles.data()),
          "vkGetRayTracingShaderGroupHandlesKHR");
    const VkDeviceSize raygenOffset = 0, probesOffset = AlignUp(stride, base),
                       missOffset = probesOffset + AlignUp(stride, base),
                       hitOffset = missOffset + AlignUp(2 * stride, base);
    const VkDeviceSize bytes = hitOffset + 2 * stride;
    _dev.Reserve(_sbt, bytes + base,
                 VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    const VkDeviceAddress address = AlignUp(_Address(_sbt.buffer), base);
    auto *table = static_cast<uint8_t *>(_sbt.mapped) + (address - _Address(_sbt.buffer));
    std::memcpy(table + raygenOffset, &handles[0], handleSize);
    std::memcpy(table + probesOffset, &handles[size_t(5) * handleSize], handleSize);
    for (uint32_t i = 0; i < 2; ++i) {
        std::memcpy(table + missOffset + i * stride, &handles[size_t(1 + i) * handleSize], handleSize);
        std::memcpy(table + hitOffset + i * stride, &handles[size_t(3 + i) * handleSize], handleSize);
    }
    _raygen = {address + raygenOffset, stride, stride};
    _raygenProbes = {address + probesOffset, stride, stride};
    _miss = {address + missOffset, stride, 2 * stride};
    _hit = {address + hitOffset, stride, 2 * stride};
}

VkDeviceAddress RayTracing::_Address(VkBuffer buffer) const
{
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(_dev.device, &info);
}

void RayTracing::_ReserveScratch(VkDeviceSize bytes)
{
    bytes += _scratchAlignment;  // room to align the start
    if (_scratch.buffer && _scratch.size >= bytes) {
        return;
    }
    _dev.Destroy(_scratch);
    _scratch = _dev.CreateDeviceBuffer(std::max(bytes, _scratch.size + _scratch.size / 2),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
}

Image RayTracing::_CreateAtlas(VkFormat format, uint32_t width, uint32_t height)
{
    Image img = _dev.CreateImage(format, width, height, 1, 1,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _dev.Submit([&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ImageBarrier(cmd, img.image, range, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        const VkClearColorValue zero{};
        vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        ImageBarrier(cmd, img.image, range, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    });
    return img;
}

// The per-pixel results, in GENERAL layout for good (written by ray
// generation, read by mesh.frag).
void RayTracing::_CreateImages(uint32_t width, uint32_t height)
{
    _dev.Destroy(_shadowImage);
    _dev.Destroy(_reflectionImage);
    _shadowImage = _CreateAtlas(VK_FORMAT_R8G8B8A8_UNORM, width, height);
    _reflectionImage = _CreateAtlas(VK_FORMAT_R16G16B16A16_SFLOAT, width, height);
    _width = width;
    _height = height;
}

void RayTracing::_DestroyBlas(Blas &blas)
{
    _fn->destroyAccelerationStructure(_dev.device, blas.as, nullptr);
    _blocks[blas.block].ranges.Free(blas.offset, blas.units);
    blas = Blas();
}

// A BLAS's storage in a block (a new block when none has room) and the
// acceleration structure over it.
RayTracing::Blas RayTracing::_CreateBlas(VkDeviceSize bytes)
{
    Blas blas;
    blas.units = uint32_t((bytes + kUnit - 1) / kUnit);
    bool placed = false;
    for (uint32_t b = 0; b < _blocks.size() && !placed; ++b) {
        placed = _blocks[b].ranges.Allocate(blas.units, &blas.offset);
        blas.block = b;
    }
    if (!placed) {
        Block block;
        const VkDeviceSize blockBytes = std::max(kBlockBytes, VkDeviceSize(blas.units) * kUnit);
        block.buffer = _dev.CreateDeviceBuffer(blockBytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        block.ranges.Grow(uint32_t(blockBytes / kUnit));
        block.ranges.Allocate(blas.units, &blas.offset);
        blas.block = uint32_t(_blocks.size());
        _blocks.push_back(std::move(block));
    }
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = _blocks[blas.block].buffer.buffer;
    ci.offset = VkDeviceSize(blas.offset) * kUnit;
    ci.size = bytes;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    Check(_fn->createAccelerationStructure(_dev.device, &ci, nullptr, &blas.as), "vkCreateAccelerationStructureKHR");
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = blas.as;
    blas.address = _fn->getAddress(_dev.device, &ai);
    return blas;
}

// Indexed triangles with float positions first in each vertex; opaque or
// not per instance (tlas.comp), as the material says.
VkAccelerationStructureGeometryKHR RayTracing::_Triangles(VkDeviceAddress vertices, VkDeviceSize stride,
                                                          uint32_t vertexCount, VkDeviceAddress indices) const
{
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    VkAccelerationStructureGeometryTrianglesDataKHR &t = geometry.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    t.vertexData.deviceAddress = vertices;
    t.vertexStride = stride;
    t.maxVertex = vertexCount - 1;
    t.indexType = VK_INDEX_TYPE_UINT32;
    t.indexData.deviceAddress = indices;
    return geometry;
}

// One BLAS per traced slot geometry, built in batches (one submit each) as
// soon as a mesh's geometry is new; unused ones are freed. Skinned meshes
// get theirs per instance (_PrepareSkinned).
// ponytail: no compaction (BLASes take about twice the memory they could),
// and empty blocks are kept until Release.
void RayTracing::_BuildBlases(const Scene::Gpu &gpu)
{
    std::unordered_set<uint64_t> used;
    std::vector<uint32_t> missing;
    for (uint32_t s = 0; s < gpu.slotGeometry.size(); ++s) {
        const Scene::Gpu::SlotGeometry &g = gpu.slotGeometry[s];
        if (g.version != 0 && !g.skinned && g.indexCount >= 3 && used.insert(g.version).second &&
            !_blas.count(g.version)) {
            missing.push_back(s);
        }
    }
    for (auto it = _blas.begin(); it != _blas.end();) {
        if (used.count(it->first)) {
            ++it;
            continue;
        }
        _DestroyBlas(it->second);
        it = _blas.erase(it);
        _tlasDirty = true;
    }
    if (missing.empty()) {
        return;
    }
    _tlasDirty = true;
    const VkDeviceAddress vertices = _Address(gpu.vertices), indices = _Address(gpu.indices);
    for (size_t next = 0; next < missing.size();) {
        const size_t batch = std::min<size_t>(missing.size() - next, 4096);
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<VkDeviceSize> scratchOffsets;
        geometries.reserve(batch);  // infos point into it
        VkDeviceSize scratch = 0;
        while (infos.size() < batch && (infos.empty() || scratch < kScratchBudget)) {
            const Scene::Gpu::SlotGeometry &g = gpu.slotGeometry[missing[next++]];
            geometries.push_back(_Triangles(vertices + VkDeviceAddress(g.vertexOffset) * sizeof(Vertex), sizeof(Vertex),
                                            g.vertexCount, indices + VkDeviceAddress(g.firstIndex) * sizeof(uint32_t)));

            VkAccelerationStructureBuildGeometryInfoKHR info{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.geometryCount = 1;
            info.pGeometries = &geometries.back();
            const uint32_t triangles = g.indexCount / 3;
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            _fn->getBuildSizes(_dev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &triangles,
                               &sizes);

            const Blas blas = _CreateBlas(sizes.accelerationStructureSize);
            _blas[g.version] = blas;

            info.dstAccelerationStructure = blas.as;
            infos.push_back(info);
            ranges.push_back({triangles, 0, 0, 0});
            scratchOffsets.push_back(scratch);
            scratch += AlignUp(sizes.buildScratchSize, _scratchAlignment);
        }
        _ReserveScratch(scratch);
        const VkDeviceAddress scratchBase = AlignUp(_Address(_scratch.buffer), _scratchAlignment);
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangeInfos;
        for (size_t i = 0; i < infos.size(); ++i) {
            infos[i].scratchData.deviceAddress = scratchBase + scratchOffsets[i];
            rangeInfos.push_back(&ranges[i]);
        }
        _dev.Submit([&](VkCommandBuffer cmd) {
            _fn->cmdBuild(cmd, uint32_t(infos.size()), infos.data(), rangeInfos.data());
        });
    }
    _dev.Destroy(_scratch);  // the TLAS needs far less
}

void RayTracing::_DestroySkinned(Skinned &s)
{
    _DestroyBlas(s.blas);
    _skinnedRanges.Free(s.firstVertex, s.vertexCount);
    s = Skinned();
}

// A skinned instance's triangles over its posed vertices (6 floats each).
VkAccelerationStructureBuildGeometryInfoKHR RayTracing::_SkinnedBuildInfo(
    const Skinned &s, const Scene::Gpu &gpu, VkAccelerationStructureGeometryKHR *geometry) const
{
    const Scene::Gpu::SlotGeometry &g = gpu.slotGeometry[s.slot];
    *geometry = _Triangles(_Address(_skinnedVertices.buffer) + VkDeviceAddress(s.firstVertex) * 6 * sizeof(float),
                           6 * sizeof(float), s.vertexCount,
                           _Address(gpu.indices) + VkDeviceAddress(g.firstIndex) * sizeof(uint32_t));
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                 VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    info.mode = s.built ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.srcAccelerationStructure = s.built ? s.blas.as : VK_NULL_HANDLE;
    info.dstAccelerationStructure = s.blas.as;
    info.geometryCount = 1;
    info.pGeometries = geometry;
    return info;
}

// The skinned instances this frame traces (Work::skinned): a BLAS and room
// for posed vertices each, kept while an instance stays picked and its
// mesh's geometry the same (then only refit), built anew otherwise. Writes
// their table; returns the scratch their builds and refits need.
VkDeviceSize RayTracing::_PrepareSkinned(const Scene::Gpu &gpu)
{
    const size_t count = std::min<size_t>(_work.skinned.size(), _work.skinnedCapacity);
    std::unordered_set<uint32_t> picked;
    for (size_t i = 0; i < count; ++i) {
        picked.insert(_work.skinned[i].instance);
    }
    for (auto it = _skinned.begin(); it != _skinned.end();) {
        if (picked.count(it->first)) {
            ++it;
            continue;
        }
        _DestroySkinned(it->second);
        it = _skinned.erase(it);
    }
    _skinnedOrder.clear();
    for (size_t i = 0; i < count; ++i) {
        const Scene::SkinnedInstance &pick = _work.skinned[i];
        const Scene::Gpu::SlotGeometry &g = gpu.slotGeometry[pick.slot];
        if (g.version == 0 || g.indexCount < 3) {
            continue;
        }
        Skinned &s = _skinned[pick.instance];
        if (s.blas.as && (s.version != g.version || s.slot != pick.slot)) {
            _DestroySkinned(s);
        }
        if (!s.blas.as) {
            // Room for its posed vertices (all get rebuilt where it grows:
            // their vertices move to a new buffer).
            while (!_skinnedRanges.Allocate(g.vertexCount, &s.firstVertex)) {
                const uint32_t old = _skinnedRanges.Capacity();
                const uint32_t capacity = std::max(old * 2, old + g.vertexCount);
                _dev.Destroy(_skinnedVertices);
                _skinnedVertices = _dev.CreateDeviceBuffer(VkDeviceSize(capacity) * 6 * sizeof(float),
                                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | _dev.rayTracingInputUsage);
                _skinnedRanges.Grow(capacity);
                for (auto &[instance, other] : _skinned) {
                    other.built = false;
                }
            }
            s.slot = pick.slot;
            s.version = g.version;
            s.vertexCount = g.vertexCount;
            s.flags = pick.flags;
            s.built = false;
            VkAccelerationStructureGeometryKHR geometry;
            const VkAccelerationStructureBuildGeometryInfoKHR info = _SkinnedBuildInfo(s, gpu, &geometry);
            const uint32_t triangles = g.indexCount / 3;
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            _fn->getBuildSizes(_dev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &triangles, &sizes);
            s.blas = _CreateBlas(sizes.accelerationStructureSize);
            s.buildScratch = sizes.buildScratchSize;
            s.updateScratch = sizes.updateScratchSize;
        }
        _skinnedOrder.push_back(pick.instance);
    }
    // Once all have their room (growing rebuilds them all).
    _skinnedScratch.clear();
    _skinnedMostVertices = 0;
    std::vector<uint32_t> table;
    VkDeviceSize scratch = 0;
    for (uint32_t instance : _skinnedOrder) {
        const Skinned &s = _skinned.at(instance);
        _skinnedScratch.push_back(scratch);
        scratch += AlignUp(s.built ? s.updateScratch : s.buildScratch, _scratchAlignment);
        _skinnedMostVertices = std::max(_skinnedMostVertices, s.vertexCount);
        table.insert(table.end(), {uint32_t(s.blas.address), uint32_t(s.blas.address >> 32), s.slot, s.firstVertex,
                                   instance, s.vertexCount, s.flags, 0u});
    }
    if (!table.empty()) {
        _dev.Reserve(_skinnedTable, table.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(_skinnedTable.mapped, table.data(), table.size() * sizeof(uint32_t));
    }
    // Posed every frame: the TLAS follows them.
    _tlasDirty = _tlasDirty || !_skinnedOrder.empty() || _skinnedLast > 0;
    _skinnedLast = uint32_t(_skinnedOrder.size());

    const VkDescriptorBufferInfo infos[2] = {
        {_skinnedVertices.buffer ? _skinnedVertices.buffer : _skinnedTable.buffer, 0, VK_WHOLE_SIZE},
        {_skinnedTable.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = _set;
        writes[i].dstBinding = 4 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(_dev.device, 2, writes, 0, nullptr);
    return scratch;
}

// The slot table and the TLAS's storage for every instance slot; the
// build itself is recorded by Record when something changed.
void RayTracing::_PrepareTlas(const Scene::Gpu &gpu)
{
    std::vector<uint32_t> words(std::max<size_t>(gpu.slotGeometry.size(), 1) * 4, 0);
    for (size_t s = 0; s < gpu.slotGeometry.size(); ++s) {
        const Scene::Gpu::SlotGeometry &g = gpu.slotGeometry[s];
        const auto it = g.version ? _blas.find(g.version) : _blas.end();
        const VkDeviceAddress address = it != _blas.end() ? it->second.address : 0;
        words[s * 4 + 0] = uint32_t(address);
        words[s * 4 + 1] = uint32_t(address >> 32);
        words[s * 4 + 2] = g.firstIndex;
        words[s * 4 + 3] = (g.cutout ? kSlotCutout : 0) | (g.emitter ? kSlotEmitter : 0);
    }
    if (words != _slotWords) {
        _dev.Reserve(_slots, words.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(_slots.mapped, words.data(), words.size() * sizeof(uint32_t));
        _slotWords = std::move(words);
        _tlasDirty = true;
    }
    _tlasDirty = _tlasDirty || gpu.version != _tlasVersion;

    const uint32_t capacity = std::max(gpu.instanceCapacity, 1u) + _work.skinnedCapacity;
    if (capacity <= _tlasCapacity) {
        _ReserveScratch(_tlasScratch);
        return;
    }
    _fn->destroyAccelerationStructure(_dev.device, _tlas, nullptr);
    _dev.Destroy(_instances);
    _dev.Destroy(_tlasBuffer);
    _instances = _dev.CreateDeviceBuffer(VkDeviceSize(capacity) * sizeof(VkAccelerationStructureInstanceKHR),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    VkAccelerationStructureGeometryKHR geometry;
    VkAccelerationStructureBuildGeometryInfoKHR info = _TlasBuildInfo(&geometry);
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    _fn->getBuildSizes(_dev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &capacity, &sizes);
    _tlasBuffer = _dev.CreateDeviceBuffer(sizes.accelerationStructureSize,
                                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = _tlasBuffer.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    Check(_fn->createAccelerationStructure(_dev.device, &ci, nullptr, &_tlas), "vkCreateAccelerationStructureKHR");
    _tlasScratch = sizes.buildScratchSize;
    _ReserveScratch(_tlasScratch);
    _tlasCapacity = capacity;
    _tlasDirty = true;

    const VkDescriptorBufferInfo instances{_instances.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = _set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &instances;
    vkUpdateDescriptorSets(_dev.device, 1, &w, 0, nullptr);
}

VkAccelerationStructureBuildGeometryInfoKHR RayTracing::_TlasBuildInfo(VkAccelerationStructureGeometryKHR *geometry) const
{
    *geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry->geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry->geometry.instances.data.deviceAddress = _instances.buffer ? _Address(_instances.buffer) : 0;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = geometry;
    info.dstAccelerationStructure = _tlas;
    return info;
}

// A grid over the scene's bounds, Work::giProbes along the longest side:
// the spacing snapped to powers of 2^(1/4) and the probes to multiples of
// it (half a step off, so they don't sit on floors at round heights).
// Coarser until it fits kMaxProbes. A new grid (or GI switched back on)
// starts the probes over, so the grid stays while the scene (an animated
// one too) stays inside it and its spacing within a factor sqrt 2 of the
// fitting one.
void RayTracing::_FitProbes(const Scene::Gpu &gpu)
{
    float extent[3], longest = 0;
    for (int k = 0; k < 3; ++k) {
        extent[k] = gpu.hasBounds ? std::max(gpu.boundsMax[k] - gpu.boundsMin[k], 0.0f) : 1.0f;
        longest = std::max(longest, extent[k]);
    }
    const uint32_t along = std::clamp(_work.giProbes, 2u, 64u);
    float spacing = std::exp2(std::ceil(std::log2(std::max(longest, 1e-6f) / float(along - 1)) * 4.0f) / 4.0f);
    float origin[3];
    uint32_t counts[3];
    for (;;) {
        uint64_t total = 1;
        for (int k = 0; k < 3; ++k) {
            const float lo = gpu.hasBounds ? gpu.boundsMin[k] : -0.5f;
            origin[k] = (std::floor(lo / spacing - 0.5f) + 0.5f) * spacing;
            counts[k] = uint32_t(std::floor((lo + extent[k] - origin[k]) / spacing)) + 2;
            total *= counts[k];
        }
        if (total <= kMaxProbes) {
            break;
        }
        spacing *= 1.18920712f;
    }
    bool keep = _giFrames > 0 && along == _giAlong && spacing < 1.42f * _giSpacing && _giSpacing < 1.42f * spacing;
    for (int k = 0; k < 3 && keep && gpu.hasBounds; ++k) {
        keep = gpu.boundsMin[k] >= _giOrigin[k] &&
               gpu.boundsMax[k] <= _giOrigin[k] + float(_giCounts[k] - 1) * _giSpacing;
    }
    if (keep) {
        return;
    }
    const uint32_t probes = counts[0] * counts[1] * counts[2];
    const uint32_t perRow = std::min(probes, kProbesPerRow), rows = (probes + perRow - 1) / perRow;
    _dev.Destroy(_giIrradiance);
    _dev.Destroy(_giVisibility);
    _giIrradiance = _CreateAtlas(VK_FORMAT_R16G16B16A16_SFLOAT, perRow * (kIrradianceTexels + 2),
                                 rows * (kIrradianceTexels + 2));
    _giVisibility = _CreateAtlas(VK_FORMAT_R32G32_SFLOAT, perRow * (kVisibilityTexels + 2),
                                 rows * (kVisibilityTexels + 2));
    std::copy(origin, origin + 3, _giOrigin);
    std::copy(counts, counts + 3, _giCounts);
    _giSpacing = spacing;
    _giPerRow = perRow;
    _giAlong = along;
    _giFrames = 0;
}

void RayTracing::Prepare(const Scene::Gpu &gpu, const Work &work, uint32_t width, uint32_t height)
{
    if (work.gi && !_work.gi) {
        _giFrames = 0;  // switched on: start over
    }
    _work = work;
    _work.giRays = std::clamp(work.giRays, 16u, kMaxProbeRays);
    _BuildBlases(gpu);
    const VkDeviceSize skinnedScratch = _PrepareSkinned(gpu);
    _PrepareTlas(gpu);
    _ReserveScratch(skinnedScratch);  // the skinned BLASes' builds share it with the TLAS's, one after the other
    if ((work.shadows || work.reflections) && (width != _width || height != _height)) {
        _CreateImages(width, height);
    }
    RayTracingData &d = *static_cast<RayTracingData *>(_uniforms.mapped);
    d = RayTracingData{};
    std::copy(work.shadowLights, work.shadowLights + 4, d.shadowLights);
    d.params[0] = _frame++;
    d.params[1] = std::clamp(work.shadowSamples, 1u, 16u);
    d.params[2] = std::clamp(work.reflectionSamples, 1u, 16u);
    d.params[3] = work.sun;
    d.skinned[0] = gpu.instanceCapacity;
    d.skinned[1] = uint32_t(_skinnedOrder.size());
    if (!work.gi) {
        return;
    }
    _FitProbes(gpu);
    const uint32_t probes = _giCounts[0] * _giCounts[1] * _giCounts[2];
    const VkDeviceSize rayBytes = VkDeviceSize(probes) * _work.giRays * 4 * sizeof(float);
    if (_giRays.size < rayBytes) {
        _dev.Destroy(_giRays);
        _giRays = _dev.CreateDeviceBuffer(rayBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    std::copy(_giOrigin, _giOrigin + 3, d.giOrigin);
    d.giOrigin[3] = _giSpacing;
    std::copy(_giCounts, _giCounts + 3, d.giCounts);
    d.giCounts[3] = _giPerRow;
    // While the probes are fresh, a plain average of the frames so far.
    const float hysteresis = std::clamp(work.giHysteresis, 0.0f, 0.99f);
    d.giParams[0] = std::min(hysteresis, float(_giFrames) / float(_giFrames + 1));
    d.giParams[1] = float(_work.giRays);
    d.giParams[2] = 2.0f * _giSpacing;
    // A random rotation of the ray directions (a random unit quaternion,
    // Shoemake), different every frame.
    uint32_t seed = _frame * 747796405u + 2891336453u;
    auto random = [&]() {
        seed = seed * 747796405u + 2891336453u;
        return float((seed >> 8) & 0xFFFFFF) / 16777216.0f;
    };
    const float u0 = random(), u1 = 6.2831853f * random(), u2 = 6.2831853f * random();
    const float x = std::sqrt(1 - u0) * std::sin(u1), y = std::sqrt(1 - u0) * std::cos(u1),
                z = std::sqrt(u0) * std::sin(u2), w = std::sqrt(u0) * std::cos(u2);
    const float m[16] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y), 0,
                         2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x), 0,
                         2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y), 0,
                         0, 0, 0, 1};
    std::copy(m, m + 16, d.giRotation);
    ++_giFrames;

    const VkDescriptorBufferInfo rays{_giRays.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo atlases[2] = {{VK_NULL_HANDLE, _giIrradiance.view, VK_IMAGE_LAYOUT_GENERAL},
                                              {VK_NULL_HANDLE, _giVisibility.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = _set;
        writes[i].dstBinding = 1 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pBufferInfo = i == 0 ? &rays : nullptr;
        writes[i].pImageInfo = i == 0 ? nullptr : &atlases[i - 1];
    }
    vkUpdateDescriptorSets(_dev.device, 3, writes, 0, nullptr);
}

void RayTracing::UpdateDescriptors(VkDescriptorSet set, const Scene::Gpu &gpu)
{
    VkWriteDescriptorSetAccelerationStructureKHR tlas{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    tlas.accelerationStructureCount = 1;
    tlas.pAccelerationStructures = &_tlas;
    const VkDescriptorImageInfo shadow{VK_NULL_HANDLE, _shadowImage.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo reflection{VK_NULL_HANDLE, _reflectionImage.view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo vertices{gpu.vertices, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo indices{gpu.indices, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo slots{_slots.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo atlases[2] = {{_sampler, _giIrradiance.view, VK_IMAGE_LAYOUT_GENERAL},
                                              {_sampler, _giVisibility.view, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo uniforms{_uniforms.buffer, 0, sizeof(RayTracingData)};
    std::vector<VkWriteDescriptorSet> writes;
    auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo *buffer,
                     const VkDescriptorImageInfo *image, uint32_t count = 1) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = count;
        w.descriptorType = type;
        w.pBufferInfo = buffer;
        w.pImageInfo = image;
        writes.push_back(w);
    };
    if (_tlas) {
        write(52, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr);
        writes.back().pNext = &tlas;
    }
    write(53, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, &shadow);
    write(54, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, &reflection);
    if (gpu.vertices && gpu.indices) {
        write(55, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &vertices, nullptr);
        write(56, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &indices, nullptr);
    }
    write(57, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &slots, nullptr);
    write(58, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, atlases, 2);
    write(59, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uniforms, nullptr);
    vkUpdateDescriptorSets(_dev.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
}

void RayTracing::Record(VkCommandBuffer cmd, VkDescriptorSet set, const Scene::Gpu &gpu,
                        const std::function<void(const char *)> &pass)
{
    const VkDescriptorSet sets[2] = {set, _set};
    if (!_skinnedOrder.empty()) {
        // Pose the skinned instances (skin.comp built this frame's joints
        // before culling), then build or refit their BLASes.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _skinPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 2, sets, 0, nullptr);
        vkCmdDispatch(cmd, (_skinnedMostVertices + 63) / 64, uint32_t(_skinnedOrder.size()), 1);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_READ_BIT);
        std::vector<VkAccelerationStructureGeometryKHR> geometries(_skinnedOrder.size());
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        const VkDeviceAddress scratch = AlignUp(_Address(_scratch.buffer), _scratchAlignment);
        for (size_t i = 0; i < _skinnedOrder.size(); ++i) {
            const Skinned &s = _skinned.at(_skinnedOrder[i]);
            infos.push_back(_SkinnedBuildInfo(s, gpu, &geometries[i]));
            infos.back().scratchData.deviceAddress = scratch + _skinnedScratch[i];
            ranges.push_back({gpu.slotGeometry[s.slot].indexCount / 3, 0, 0, 0});
        }
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangeInfos;
        for (const VkAccelerationStructureBuildRangeInfoKHR &r : ranges) {
            rangeInfos.push_back(&r);
        }
        _fn->cmdBuild(cmd, uint32_t(infos.size()), infos.data(), rangeInfos.data());
        // The TLAS build reads them and reuses the scratch.
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
        for (uint32_t instance : _skinnedOrder) {
            _skinned.at(instance).built = true;
        }
    }
    if (_tlasDirty) {
        // The instances (tlas.comp), then the TLAS over them.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _tlasPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 2, sets, 0, nullptr);
        vkCmdDispatch(cmd, (_tlasCapacity + 63) / 64, 1, 1);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT);
        VkAccelerationStructureGeometryKHR geometry;
        VkAccelerationStructureBuildGeometryInfoKHR info = _TlasBuildInfo(&geometry);
        info.scratchData.deviceAddress = AlignUp(_Address(_scratch.buffer), _scratchAlignment);
        const VkAccelerationStructureBuildRangeInfoKHR range{_tlasCapacity, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
        _fn->cmdBuild(cmd, 1, &info, &ranges);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        _tlasDirty = false;
        _tlasVersion = gpu.version;
        pass("accelerationStructures");
    }
    if (_work.gi) {
        // Probe rays (reading last frame's probes for further bounces), then
        // blended into the probes, before shading and reflections read them.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _layout, 0, 2, sets, 0, nullptr);
        const uint32_t probes = _giCounts[0] * _giCounts[1] * _giCounts[2];
        _fn->cmdTraceRays(cmd, &_raygenProbes, &_miss, &_hit, &_callable, _work.giRays, probes, 1);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 2, sets, 0, nullptr);
        for (VkPipeline p : _giUpdate) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdDispatch(cmd, probes, 1, 1);
        }
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        pass("giProbes");
    }
    if (_work.shadows || _work.reflections) {
        // Last frame's shading pass is done reading the images; this
        // frame's prepass depth is written.
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _layout, 0, 2, sets, 0, nullptr);
        _fn->cmdTraceRays(cmd, &_raygen, &_miss, &_hit, &_callable, _width, _height, 1);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        pass("rayTracing");
    }
}

VkDeviceSize RayTracing::AccelerationStructureBytes() const
{
    VkDeviceSize bytes = _scratch.size + _instances.size + _tlasBuffer.size + _skinnedVertices.size;
    for (const Block &b : _blocks) {
        bytes += b.buffer.size;
    }
    return bytes;
}

} // namespace vkr
