#include "aov.h"

#include "scene.h"

#include <algorithm>
#include <cstring>

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
};

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
    constexpr uint32_t components[kCount] = {3, 3, 3, 1, 2, 3, 3, 4, 4};
    return components[kind];
}

bool Aovs::Any(float *const targets[kCount])
{
    return std::any_of(targets, targets + kCount, [](const float *t) { return t != nullptr; });
}

bool Aovs::NeedsGeometry(float *const targets[kCount])
{
    return targets[kNormal] || targets[kNormalEye] || targets[kSt] || targets[kAlbedo] || targets[kEmission];
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
    VkDescriptorSetLayoutBinding bindings[7] = {};
    for (uint32_t i = 0; i < 7; ++i) {
        bindings[i] = {i, i < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                       VK_SHADER_STAGE_COMPUTE_BIT};
    }
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 7;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
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
    const VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &_resolve);
    vkDestroyShaderModule(d, m, nullptr);
    Check(r, "vkCreateComputePipelines");
}

Aovs::~Aovs()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    for (Image *img : {&_normalSt, &_albedo, &_emission}) {
        _dev.Destroy(*img);
    }
    _dev.Destroy(_params);
    _dev.Destroy(_output);
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
    VkResult r = VK_SUCCESS;
    for (uint32_t v = 0; v <= kVariants && r == VK_SUCCESS; ++v) {
        if ((v & ~kVariants) || ((v & 8) && !(v & 4))) {
            continue;  // procedurals come with a program
        }
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
        r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &_geometry[v]);
    }
    for (VkShaderModule m : modules) {
        vkDestroyShaderModule(d, m, nullptr);
    }
    Check(r, "vkCreateGraphicsPipelines");
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
    VkDeviceSize floats = 0;
    for (uint32_t k = 0; k < kCount; ++k) {
        _offsets[k] = floats;
        floats += targets[k] ? VkDeviceSize(Components(Kind(k))) * width * height : 0;
    }
    // Shader writes to host memory crawl (~30 ms for a 480 x 270 AOV):
    // they go to device memory and are copied over.
    const VkDeviceSize bytes = floats * sizeof(float);
    if (_output.size < bytes) {
        _dev.Destroy(_output);
        _output = _dev.CreateDeviceBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    _dev.Reserve(_readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo images[5] = {{_nearest, depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                             {_nearest, primIdView, ro},
                                             {_nearest, _normalSt.view, ro},
                                             {_nearest, _albedo.view, ro},
                                             {_nearest, _emission.view, ro}};
    const VkDescriptorBufferInfo buffers[2] = {{_params.buffer, 0, VK_WHOLE_SIZE}, {_output.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[7] = {};
    for (uint32_t i = 0; i < 7; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = _set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pImageInfo = i < 5 ? &images[i] : nullptr;
        writes[i].pBufferInfo = i < 5 ? nullptr : &buffers[i - 5];
    }
    vkUpdateDescriptorSets(_dev.device, 7, writes, 0, nullptr);

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
            const Push push{k, uint32_t(_offsets[k]), Components(Kind(k)), p.cryptomatteIds};
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

void Aovs::Read(float *const targets[kCount]) const
{
    for (uint32_t k = 0; k < kCount; ++k) {
        if (targets[k]) {
            std::memcpy(targets[k], static_cast<const float *>(_readback.mapped) + _offsets[k],
                        sizeof(float) * Components(Kind(k)) * _width * _height);
        }
    }
}

} // namespace vkr
