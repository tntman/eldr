#include "aov.h"

#include "scene.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <cstring>
#include <vector>

namespace vkr {

namespace {

const uint32_t kMeshVert[] =
#include "mesh.vert.inc"
;
const uint32_t kMeshGraphVert[] =
#include "mesh_graph.vert.inc"
;
const uint32_t kAovFrag[] =
#include "aov.frag.inc"
;
const uint32_t kAovGraphFrag[] =
#include "aov_graph.frag.inc"
;
const uint32_t kAovComp[] =
#include "aov.comp.inc"
;

constexpr VkFormat kGeometryFormats[3] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                          VK_FORMAT_R16G16B16A16_SFLOAT};
constexpr VkDeviceSize kMatrixBytes = 2 * 16 * sizeof(float);  // Params' view and invViewProj

struct Push {  // mirror of aov.comp's
    uint32_t kind, offset, components, cryptomatteIds;
    float weight;  // of this frame in the average
    uint32_t sumsOffset;
    float filmOffset[2], filmRadius;
    uint32_t film;
};

bool IsColour(uint32_t k)
{
    return k == Aovs::kAlbedo || k == Aovs::kEmission || k >= Aovs::kDiffuseAlbedo;
}

bool IsCrypto(uint32_t k)
{
    return k == Aovs::kCryptoObject || k == Aovs::kCryptoMaterial;
}

// A Cryptomatte pixel's film state in the sums: 6 ids (float bits), their 6
// weights, the pixel's total weight, the film's sample count (aov.comp).
constexpr uint32_t kCryptoStride = 14;

VkImageSubresourceRange Range(VkImageAspectFlags aspect)
{
    return {aspect, 0, 1, 0, 1};
}

} // namespace

uint32_t CryptomatteHash(const std::string &name)
{
    // MurmurHash3_x86_32 (Austin Appleby, public domain), seed 0.
    const auto *data = reinterpret_cast<const uint8_t *>(name.data());
    const size_t length = name.size();
    constexpr uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    auto rotl = [](uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    uint32_t h = 0;
    size_t i = 0;
    for (; i + 4 <= length; i += 4) {
        uint32_t k;
        std::memcpy(&k, data + i, 4);
        k = rotl(k * c1, 15) * c2;
        h = rotl(h ^ k, 13) * 5 + 0xe6546b64;
    }
    uint32_t k = 0;
    switch (length & 3) {
    case 3: k ^= uint32_t(data[i + 2]) << 16; [[fallthrough]];
    case 2: k ^= uint32_t(data[i + 1]) << 8; [[fallthrough]];
    case 1: k ^= data[i]; h ^= rotl(k * c1, 15) * c2;
    }
    h ^= uint32_t(length);
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    // Cryptomatte: no denormal, inf or NaN floats.
    const uint32_t exponent = (h >> 23) & 0xFF;
    return exponent == 0 || exponent == 0xFF ? h ^ (1u << 23) : h;
}

uint32_t Aovs::Components(Kind kind)
{
    constexpr uint32_t components[kDirectDiffuse] = {3, 3, 3, 1, 2, 3, 3, 12, 12, 3, 1, 3};
    return kind < kDirectDiffuse ? components[kind] : 3;  // the splits: rgb
}

bool Aovs::Any(float *const targets[kCount])
{
    return std::any_of(targets, targets + kCount, [](const float *t) { return t != nullptr; });
}

bool Aovs::NeedsSplits(float *const targets[kCount])
{
    return std::any_of(targets + kDirectDiffuse, targets + kCount, [](const float *t) { return t; });
}

uint32_t Aovs::SplitLayers(float *const targets[kCount])
{
    uint32_t groups = 0;
    for (uint32_t g = 0; g < kMaxLightGroups; ++g) {
        groups = targets[kLightGroup0 + g] ? g + 1 : groups;
    }
    return NeedsSplits(targets) ? kSplitLayers + groups : 4;
}

bool Aovs::NeedsGeometry(float *const targets[kCount])
{
    return targets[kNormal] || targets[kNormalEye] || targets[kSt] || targets[kAlbedo] || targets[kEmission] ||
           targets[kDiffuseAlbedo];
}

Aovs::Aovs(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(d, &sci, nullptr, &_nearest), "vkCreateSampler");

    // 0 depth, 1 primId, 2..4 the G-buffer; 5 Params, 6 the output.
    // 7: the light splits; 8: the film sums; 9: the film's weights.
    VkDescriptorSetLayoutBinding bindings[10] = {};
    for (uint32_t i = 0; i < 10; ++i) {
        bindings[i] = {i,
                       i < 5 || i == 7 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                       : i == 9        ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                       : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       1, VK_SHADER_STAGE_COMPUTE_BIT};
    }
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 10;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(d, &pci, nullptr, &_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = _pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &_setLayout;
    Check(vkAllocateDescriptorSets(d, &ai, &_set), "vkAllocateDescriptorSets");

    VkShaderModule m = CreateShaderModule(d, kAovComp, sizeof(kAovComp));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m,
                "main"};
    ci.layout = _layout;
    const VkResult r = vkCreateComputePipelines(d, _dev.pipelineCache, 1, &ci, nullptr, &_resolve);
    vkDestroyShaderModule(d, m, nullptr);
    Check(r, "vkCreateComputePipelines");
}

Aovs::~Aovs()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    for (Image *img : {&_normalSt, &_albedo, &_emission, &_splits, &_prevSplits}) {
        _dev.Destroy(*img);
    }
    _dev.Destroy(_params);
    _dev.Destroy(_output);
    _dev.Destroy(_sums);
    _dev.Destroy(_readback);
    vkDestroyPipeline(d, _resolve, nullptr);
    for (VkPipeline p : _geometry) {
        vkDestroyPipeline(d, p, nullptr);
    }
    vkDestroyDescriptorPool(d, _pool, nullptr);
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroySampler(d, _nearest, nullptr);
}

// As the renderer's shading pipelines (variant bits: kWideSkin,
// kDualQuaternion, GRAPH, procedurals), into the G-buffer, depth tested
// read-only.
const VkPipeline *Aovs::Pipelines(VkPipelineLayout layout)
{
    if (_geometry[0]) {
        return _geometry;
    }
    const VkDevice d = _dev.device;
    const VkShaderModule modules[4] = {CreateShaderModule(d, kMeshVert, sizeof(kMeshVert)),
                                       CreateShaderModule(d, kAovFrag, sizeof(kAovFrag)),
                                       CreateShaderModule(d, kMeshGraphVert, sizeof(kMeshGraphVert)),
                                       CreateShaderModule(d, kAovGraphFrag, sizeof(kAovGraphFrag))};
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {3, 0, VK_FORMAT_R16G16B16A16_SFLOAT, offsetof(Vertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 4;
    input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachments[3] = {};
    for (VkPipelineColorBlendAttachmentState &a : attachments) {
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 3;
    blend.pAttachments = attachments;
    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 3;
    rendering.pColorAttachmentFormats = kGeometryFormats;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    const VkSpecializationMapEntry entries[3] = {
        {0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}, {10, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
    const VkSpecializationMapEntry proceduralEntry{8, 0, sizeof(VkBool32)};
    const VkBool32 procedural = VK_TRUE;
    const VkSpecializationInfo proceduralSpec{1, &proceduralEntry, sizeof(procedural), &procedural};
    // Each variant on a thread of its own (drivers compile one pipeline on
    // one core): seconds with a cold driver cache otherwise.
    std::atomic<VkResult> r{VK_SUCCESS};
    std::vector<std::thread> threads;
    for (uint32_t v = 0; v <= kVariants; ++v) {
        if ((v & ~kVariants) || ((v & 8) && !(v & 4))) {
            continue;  // procedurals come with a program
        }
        threads.emplace_back([&, v] {
            const VkBool32 flags[3] = {(v & 1) ? VK_TRUE : VK_FALSE, (v & 2) ? VK_TRUE : VK_FALSE,
                                       (v & 32) ? VK_TRUE : VK_FALSE};
            const VkSpecializationInfo spec{3, entries, sizeof(flags), flags};
            const int graph = v & 4 ? 2 : 0;
            const VkPipelineShaderStageCreateInfo stages[2] = {
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                 modules[graph], "main", &spec},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                 modules[graph + 1], "main", v & 8 ? &proceduralSpec : nullptr},
            };
            VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
            ci.stageCount = 2;
            ci.pStages = stages;
            ci.pVertexInputState = &input;
            ci.pInputAssemblyState = &assembly;
            ci.pViewportState = &viewport;
            ci.pRasterizationState = &raster;
            ci.pMultisampleState = &multisample;
            ci.pDepthStencilState = &depth;
            ci.pColorBlendState = &blend;
            ci.pDynamicState = &dynamic;
            ci.layout = layout;
            const VkResult result = vkCreateGraphicsPipelines(d, _dev.pipelineCache, 1, &ci, nullptr, &_geometry[v]);
            if (result != VK_SUCCESS) {
                r = result;
            }
        });
    }
    for (std::thread &t : threads) {
        t.join();
    }
    for (VkShaderModule m : modules) {
        vkDestroyShaderModule(d, m, nullptr);
    }
    Check(r.load(), "vkCreateGraphicsPipelines");
    return _geometry;
}

// The GPU is idle while Render records.
void Aovs::_Resize(uint32_t width, uint32_t height)
{
    Image *images[3] = {&_normalSt, &_albedo, &_emission};
    for (int i = 0; i < 3; ++i) {
        _dev.Destroy(*images[i]);
        *images[i] = _dev.CreateImage(kGeometryFormats[i], width, height, 1, 1,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    }
    _width = width;
    _height = height;
}

void Aovs::Record(VkCommandBuffer cmd, const Params &p, uint32_t width, uint32_t height, VkImage depth,
                  VkImageView depthView, VkImage primId, VkImageView primIdView, float *const targets[kCount],
                  const std::function<void()> &drawGeometry)
{
    if (width != _width || height != _height) {
        _Resize(width, height);
    }
    const VkDeviceSize cryptoBytes = VkDeviceSize(p.cryptomatteIds) * 2 * sizeof(uint32_t);
    _dev.Reserve(_params, kMatrixBytes + std::max<VkDeviceSize>(cryptoBytes, 4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto *params = static_cast<uint8_t *>(_params.mapped);
    std::memcpy(params, p.view, sizeof(p.view));
    std::memcpy(params + sizeof(p.view), p.invViewProj, sizeof(p.invViewProj));
    if (cryptoBytes > 0) {
        std::memcpy(params + kMatrixBytes, p.cryptomatte, cryptoBytes);
    }
    VkDeviceSize floats = 0, sumFloats = 0;
    for (uint32_t k = 0; k < kCount; ++k) {
        _offsets[k] = floats;
        _sumsOffsets[k] = sumFloats;
        _components[k] = p.components[k] ? std::min<uint32_t>(p.components[k], IsCrypto(k) ? 12 : 4) : Components(Kind(k));
        floats += targets[k] ? VkDeviceSize(_components[k]) * width * height : 0;
        sumFloats += targets[k] && IsColour(k) ? VkDeviceSize(_components[k] + 2) * width * height : 0;
        sumFloats += targets[k] && IsCrypto(k) && p.film && p.cryptoSoft ? VkDeviceSize(kCryptoStride) * width * height : 0;
    }
    // Shader writes to host memory crawl (~30 ms for a 480 x 270 AOV):
    // they go to device memory and are copied over.
    const VkDeviceSize bytes = floats * sizeof(float);
    if (_output.size < bytes) {
        _dev.Destroy(_output);
        _output = _dev.CreateDeviceBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    const VkDeviceSize sumBytes = std::max<VkDeviceSize>(sumFloats * sizeof(float), 16);
    if (_sums.size < sumBytes) {
        _dev.Destroy(_sums);
        _sums = _dev.CreateDeviceBuffer(sumBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    _dev.Reserve(_readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo images[10] = {{_nearest, depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                             {_nearest, primIdView, ro},
                                             {_nearest, _normalSt.view, ro},
                                             {_nearest, _albedo.view, ro},
                                             {_nearest, _emission.view, ro},
                                             {},
                                             {},
                                             {_nearest, _splits.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {},
                                             {VK_NULL_HANDLE, p.filmWeights, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo buffers[4] = {{_params.buffer, 0, VK_WHOLE_SIZE}, {_output.buffer, 0, VK_WHOLE_SIZE}, {}, {_sums.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[10] = {};
    for (uint32_t i = 0; i < 10; ++i) {
        const bool image = i < 5 || i == 7 || i == 9;
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = _set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 9 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                 : image ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pImageInfo = image ? &images[i] : nullptr;
        writes[i].pBufferInfo = image ? nullptr : &buffers[i - 5];
    }
    vkUpdateDescriptorSets(_dev.device, 10, writes, 0, nullptr);

    // The average over still frames (as TAA's): this frame's weight; reset
    // when the camera, the scene, the size or the AOVs asked for change.
    // ponytail: material and light edits don't reset it; with no new reset
    // the running average turns into a moving one (1 / maxFrames).
    std::vector<uint32_t> layout(_offsets, _offsets + kCount);
    layout.insert(layout.end(), _components, _components + kCount);
    const bool same = _frames > 0 && std::equal(p.viewProj, p.viewProj + 16, _viewProj) && p.version == _version &&
                      width == _accumWidth && height == _accumHeight && layout == _accumLayout && _output.buffer == _accumBuffer &&
                      _sums.buffer == _accumSums;
    _frames = same ? std::min(_frames + 1, std::max(p.maxFrames, 1u)) : 1;
    std::copy(p.viewProj, p.viewProj + 16, _viewProj);
    _version = p.version;
    _accumWidth = width;
    _accumHeight = height;
    _accumLayout = std::move(layout);
    _accumBuffer = _output.buffer;
    _accumSums = _sums.buffer;
    // Film samples: the colour AOVs sum filtered samples; the others (surface
    // data, positions, ids) are the sample nearest the pixel centre so far.
    const bool reset = _frames == 1;
    const float weight = p.film ? (reset || p.filmNearer ? 1.0f : 0.0f) : 1.0f / float(_frames);
    // The shading pass stored the light splits; earlier resolves wrote the output.
    VkMemoryBarrier2 stored{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    stored.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    stored.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    stored.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    stored.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo storedDep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    storedDep.memoryBarrierCount = 1;
    storedDep.pMemoryBarriers = &stored;
    vkCmdPipelineBarrier2(cmd, &storedDep);

    // Depth: read-only attachment and sampled; primId sampled.
    constexpr VkPipelineStageFlags2 kTests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    ImageBarrier(cmd, depth, Range(VK_IMAGE_ASPECT_DEPTH_BIT), VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                 kTests | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, primId, Range(VK_IMAGE_ASPECT_COLOR_BIT), VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ro);
    const bool geometry = drawGeometry && NeedsGeometry(targets);
    for (const Image *img : {&_normalSt, &_albedo, &_emission}) {
        ImageBarrier(cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                     geometry ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     geometry ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, geometry ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : ro);
    }
    if (geometry) {
        VkRenderingAttachmentInfo colors[3] = {};
        const VkImageView views[3] = {_normalSt.view, _albedo.view, _emission.view};
        for (int i = 0; i < 3; ++i) {
            colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colors[i].imageView = views[i];
            colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAtt.imageView = depthView;
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {width, height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 3;
        ri.pColorAttachments = colors;
        ri.pDepthAttachment = &depthAtt;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp{0, 0, float(width), float(height), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &ri.renderArea);
        drawGeometry();
        vkCmdEndRendering(cmd);
        for (const Image *img : {&_normalSt, &_albedo, &_emission}) {
            ImageBarrier(cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, ro);
        }
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _resolve);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
    for (uint32_t k = 0; k < kCount; ++k) {
        if (targets[k]) {
            const bool filtered = p.film && IsColour(k);
            // Cryptomatte without the film's coverage keeps the nearest sample's id
            // (weight 0: this one isn't), else the latest frame's.
            const bool crypto = IsCrypto(k) && p.film && p.cryptoSoft;
            const Push push{k, uint32_t(_offsets[k]), _components[k], p.cryptomatteIds,
                            filtered ? 1.0f : IsCrypto(k) ? (p.film ? weight : 1.0f) : weight,
                            uint32_t(_sumsOffsets[k]), {p.filmOffset[0], p.filmOffset[1]}, p.filmRadius,
                            (p.film && (!IsCrypto(k) || crypto)) ? (reset ? 2u : 1u) : 0u};
            vkCmdPushConstants(cmd, _layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
        }
    }
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
    const VkBufferCopy all{0, 0, bytes};
    vkCmdCopyBuffer(cmd, _output.buffer, _readback.buffer, 1, &all);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
    ImageBarrier(cmd, depth, Range(VK_IMAGE_ASPECT_DEPTH_BIT), kTests | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(cmd, primId, Range(VK_IMAGE_ASPECT_COLOR_BIT), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, ro,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

bool Aovs::EnsureSplits(uint32_t width, uint32_t height, uint32_t layers)
{
    if (_splits.image && width == _splitsWidth && height == _splitsHeight && layers == _splitsLayers) {
        return false;
    }
    _dev.Destroy(_splits);
    _dev.Destroy(_prevSplits);
    _splits = _dev.CreateImage(VK_FORMAT_R16G16B16A16_SFLOAT, width, height, layers, 1,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    _prevSplits = _dev.CreateImage(VK_FORMAT_R16G16B16A16_SFLOAT, width, height, 4, 1,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    _splitsWidth = width;
    _splitsHeight = height;
    _splitsLayers = layers;
    _dev.Submit([&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, _splits.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        ImageBarrier(cmd, _prevSplits.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 4}, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    return true;
}

void Aovs::ClearSplits(VkCommandBuffer cmd, bool keepPrevious)
{
    // Last frame's are the first 4 layers (SSR's direct and indirect).
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, _splitsLayers},
        first{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 4};
    if (keepPrevious) {
        // Last frame's writers (shading, SSR, subsurface, AOV reads) are
        // done; this frame's shading samples the copy.
        ImageBarrier(cmd, _splits.image, all, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL);
        ImageBarrier(cmd, _prevSplits.image, first, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 4};
        copy.extent = {_splitsWidth, _splitsHeight, 1};
        vkCmdCopyImage(cmd, _splits.image, VK_IMAGE_LAYOUT_GENERAL, _prevSplits.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ImageBarrier(cmd, _prevSplits.image, first, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    ImageBarrier(cmd, _splits.image, all,
                 keepPrevious ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_GENERAL);
    const VkClearColorValue zero{};
    vkCmdClearColorImage(cmd, _splits.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    ImageBarrier(cmd, _splits.image, all, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_GENERAL);
}

void Aovs::Read(float *const targets[kCount]) const
{
    for (uint32_t k = 0; k < kCount; ++k) {
        if (targets[k]) {
            std::memcpy(targets[k], static_cast<const float *>(_readback.mapped) + _offsets[k],
                        sizeof(float) * _components[k] * _width * _height);
        }
    }
}

} // namespace vkr
