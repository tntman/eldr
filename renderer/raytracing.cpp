#include "raytracing.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
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

// std140 mirror of RayTracingData in shaders/gi.glsl.
struct RayTracingData {
    uint32_t shadowLights[4];
    uint32_t params[4];  // frame, shadow rays per light, reflection rays, light shadowed at hits
    float giOrigin[4];
    uint32_t giCounts[4];
    float giParams[4];
    float giRotation[16];
};

// RtSlot::flags (shaders/rt.glsl, tlas.comp).
constexpr uint32_t kSlotCutout = 2;

// BLASes are placed in 64 MB blocks at the required 256-byte alignment.
constexpr VkDeviceSize kUnit = 256;
constexpr VkDeviceSize kBlockBytes = VkDeviceSize(64) << 20;
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
    _CreatePipelines(sceneLayout);
    // Placeholders until a frame needs the real sizes: every descriptor
    // stays valid.
    _CreateImages(1, 1);
    _giIrradiance = _CreateAtlas(VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8);
    _giVisibility = _CreateAtlas(VK_FORMAT_R16G16_SFLOAT, 16, 16);
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
    for (VkPipeline p : {_pipeline, _tlasPipeline}) {
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
}

void RayTracing::_CreatePipelines(VkDescriptorSetLayout sceneLayout)
{
    const VkDevice device = _dev.device;
    // Set 1, what only these passes write: the TLAS's instances, the probes'
    // rays and atlases (partially bound: written once they exist).
    const VkShaderStageFlags compute = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, compute},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, compute | VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, compute},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, compute},
    };
    const VkDescriptorBindingFlags partial[4] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 4;
    flags.pBindingFlags = partial;
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags};
    slci.bindingCount = 4;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(device, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
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

    VkShaderModule tlasModule = CreateShaderModule(device, kTlasComp, sizeof(kTlasComp));
    VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                 tlasModule, "main"};
    cci.layout = _layout;
    VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cci, nullptr, &_tlasPipeline);
    vkDestroyShaderModule(device, tlasModule, nullptr);
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
    // raygen region is one record (picked per trace), miss and hit regions
    // two records each.
    const uint32_t handleSize = _props.shaderGroupHandleSize;
    const VkDeviceSize stride = AlignUp(handleSize, _props.shaderGroupHandleAlignment);
    const VkDeviceSize base = _props.shaderGroupBaseAlignment;
    std::vector<uint8_t> handles(size_t(kGroups) * handleSize);
    Check(_fn->getGroupHandles(device, _pipeline, 0, kGroups, handles.size(), handles.data()),
          "vkGetRayTracingShaderGroupHandlesKHR");
    const VkDeviceSize raygenOffset = 0, missOffset = AlignUp(stride, base),
                       hitOffset = missOffset + AlignUp(2 * stride, base);
    const VkDeviceSize bytes = hitOffset + 2 * stride;
    _dev.Reserve(_sbt, bytes + base,
                 VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    const VkDeviceAddress address = AlignUp(_Address(_sbt.buffer), base);
    auto *table = static_cast<uint8_t *>(_sbt.mapped) + (address - _Address(_sbt.buffer));
    std::memcpy(table + raygenOffset, &handles[0], handleSize);
    for (uint32_t i = 0; i < 2; ++i) {
        std::memcpy(table + missOffset + i * stride, &handles[size_t(1 + i) * handleSize], handleSize);
        std::memcpy(table + hitOffset + i * stride, &handles[size_t(3 + i) * handleSize], handleSize);
    }
    _raygen = {address + raygenOffset, stride, stride};
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

// One BLAS per traced slot geometry, built in batches (one submit each) as
// soon as a mesh's geometry is new; unused ones are freed.
// ponytail: no compaction (BLASes take about twice the memory they could),
// and empty blocks are kept until Release.
void RayTracing::_BuildBlases(const Scene::Gpu &gpu)
{
    std::unordered_set<uint64_t> used;
    std::vector<uint32_t> missing;
    for (uint32_t s = 0; s < gpu.slotGeometry.size(); ++s) {
        const Scene::Gpu::SlotGeometry &g = gpu.slotGeometry[s];
        if (g.version != 0 && g.indexCount >= 3 && used.insert(g.version).second && !_blas.count(g.version)) {
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
            VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geometry.flags = 0;  // opaque or not per instance (tlas.comp), as the material says
            VkAccelerationStructureGeometryTrianglesDataKHR &t = geometry.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress = vertices + VkDeviceAddress(g.vertexOffset) * sizeof(Vertex);
            t.vertexStride = sizeof(Vertex);
            t.maxVertex = g.vertexCount - 1;
            t.indexType = VK_INDEX_TYPE_UINT32;
            t.indexData.deviceAddress = indices + VkDeviceAddress(g.firstIndex) * sizeof(uint32_t);
            geometries.push_back(geometry);

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

            Blas blas;
            blas.units = uint32_t((sizes.accelerationStructureSize + kUnit - 1) / kUnit);
            bool placed = false;
            for (uint32_t b = 0; b < _blocks.size() && !placed; ++b) {
                placed = _blocks[b].ranges.Allocate(blas.units, &blas.offset);
                blas.block = b;
            }
            if (!placed) {
                Block block;
                const VkDeviceSize bytes = std::max(kBlockBytes, VkDeviceSize(blas.units) * kUnit);
                block.buffer = _dev.CreateDeviceBuffer(bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
                block.ranges.Grow(uint32_t(bytes / kUnit));
                block.ranges.Allocate(blas.units, &blas.offset);
                blas.block = uint32_t(_blocks.size());
                _blocks.push_back(std::move(block));
            }
            VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ci.buffer = _blocks[blas.block].buffer.buffer;
            ci.offset = VkDeviceSize(blas.offset) * kUnit;
            ci.size = sizes.accelerationStructureSize;
            ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            Check(_fn->createAccelerationStructure(_dev.device, &ci, nullptr, &blas.as),
                  "vkCreateAccelerationStructureKHR");
            VkAccelerationStructureDeviceAddressInfoKHR ai{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            ai.accelerationStructure = blas.as;
            blas.address = _fn->getAddress(_dev.device, &ai);
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
        words[s * 4 + 3] = g.cutout ? kSlotCutout : 0;
    }
    if (words != _slotWords) {
        _dev.Reserve(_slots, words.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(_slots.mapped, words.data(), words.size() * sizeof(uint32_t));
        _slotWords = std::move(words);
        _tlasDirty = true;
    }
    _tlasDirty = _tlasDirty || gpu.version != _tlasVersion;

    const uint32_t capacity = std::max(gpu.instanceCapacity, 1u);
    if (capacity <= _tlasCapacity) {
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
    _ReserveScratch(sizes.buildScratchSize);
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

void RayTracing::Prepare(const Scene::Gpu &gpu, const Work &work, uint32_t width, uint32_t height)
{
    _work = work;
    _BuildBlases(gpu);
    _PrepareTlas(gpu);
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
    if (_work.shadows || _work.reflections) {
        // Last frame's shading pass is done reading the images.
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
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
    VkDeviceSize bytes = _scratch.size + _instances.size + _tlasBuffer.size;
    for (const Block &b : _blocks) {
        bytes += b.buffer.size;
    }
    return bytes;
}

} // namespace vkr
