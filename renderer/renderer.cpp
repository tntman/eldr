#include "renderer.h"

#include "ibl.h"
#include "vkmath.h"

#include <cstring>
#include <vector>

namespace vkr {

// std140 mirror of FrameData in shaders/frame.glsl.
struct FrameData {
    float viewProj[16];
    float invViewProj[16];
    float cameraPos[4];
    float cameraForward[4];
    float viewport[4];
    uint32_t counts[4];  // lightCount, shadowLight, envLevels, envLatlong
    float cascadeViewProj[4][16];
    float cascadeSplits[4];
    float cascadeTexel[4];
    float envSh[9][4];
    float envWorldToLocal[16];
    float envRadiance[4];
};

namespace {

const uint32_t kMeshVert[] =
#include "mesh.vert.inc"
;
const uint32_t kMeshFrag[] =
#include "mesh.frag.inc"
;
const uint32_t kSkyVert[] =
#include "sky.vert.inc"
;
const uint32_t kSkyFrag[] =
#include "sky.frag.inc"
;

constexpr uint32_t kBrdfLutSize = 64;

VkShaderModule CreateShader(VkDevice device, const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(device, &ci, nullptr, &module), "vkCreateShaderModule");
    return module;
}

VkImageSubresourceRange Range(VkImageAspectFlags aspect, uint32_t layers = 1)
{
    return {aspect, 0, 1, 0, layers};
}

Mat4 ToMat4(const float *m)
{
    Mat4 r;
    std::memcpy(r.m, m, sizeof(r.m));
    return r;
}

} // namespace

Renderer::Renderer(Device &device)
    : _dev(device)
{
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = _dev.queueFamily;
    Check(vkCreateCommandPool(_dev.device, &pci, nullptr, &_pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = _pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(_dev.device, &ai, &_cmd), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(vkCreateFence(_dev.device, &fci, nullptr, &_fence), "vkCreateFence");

    _dev.Reserve(_frameData, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    _CreateDescriptors();
    _CreatePipelines();
    _CreateConstantImages();
}

Renderer::~Renderer()
{
    vkDeviceWaitIdle(_dev.device);
    _Destroy(_color);
    _Destroy(_depth);
    _Destroy(_shadowMap);
    for (VkImageView v : _shadowLayers) {
        vkDestroyImageView(_dev.device, v, nullptr);
    }
    _Destroy(_brdfLut);
    _Destroy(_blackCube);
    _dev.Destroy(_readback);
    _dev.Destroy(_frameData);
    for (VkPipeline p : {_pipeline, _shadowPipeline, _skyPipeline}) {
        vkDestroyPipeline(_dev.device, p, nullptr);
    }
    vkDestroyPipelineLayout(_dev.device, _layout, nullptr);
    vkDestroyDescriptorPool(_dev.device, _descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(_dev.device, _setLayout, nullptr);
    for (VkSampler s : {_sampler, _clampSampler, _shadowSampler}) {
        vkDestroySampler(_dev.device, s, nullptr);
    }
    vkDestroyFence(_dev.device, _fence, nullptr);
    vkDestroyCommandPool(_dev.device, _pool, nullptr);
}

void Renderer::_CreateDescriptors()
{
    // ponytail: one material sampler (trilinear, 16x aniso, repeat) for every
    // texture; UsdUVTexture wrapS/wrapT are ignored.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = VK_TRUE;
    sci.maxAnisotropy = 16.0f;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_sampler), "vkCreateSampler");

    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxAnisotropy = 1.0f;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_clampSampler), "vkCreateSampler");

    // Outside the cascade reads as depth 1, i.e. lit.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    sci.maxLod = 0.0f;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_shadowSampler), "vkCreateSampler");

    constexpr VkShaderStageFlags kFragment = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Binding 0 is the bindless array, partially bound and indexed by material data.
    const VkDescriptorSetLayoutBinding bindings[7] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures, kFragment},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kFragment},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
    };
    const VkDescriptorBindingFlags bindingFlags[7] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 7;
    flags.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags};
    slci.bindingCount = 7;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(_dev.device, &slci, nullptr, &_setLayout),
          "vkCreateDescriptorSetLayout");

    const VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + 3},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(_dev.device, &dpci, nullptr, &_descriptorPool),
          "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = _descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &_setLayout;
    Check(vkAllocateDescriptorSets(_dev.device, &dsai, &_set), "vkAllocateDescriptorSets");
}

void Renderer::_CreatePipelines()
{
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};  // mat4 viewProj
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(_dev.device, &lci, nullptr, &_layout), "vkCreatePipelineLayout");

    const VkDevice device = _dev.device;
    VkShaderModule meshVert = CreateShader(device, kMeshVert, sizeof(kMeshVert));
    VkShaderModule meshFrag = CreateShader(device, kMeshFrag, sizeof(kMeshFrag));
    VkShaderModule skyVert = CreateShader(device, kSkyVert, sizeof(kSkyVert));
    VkShaderModule skyFrag = CreateShader(device, kSkyFrag, sizeof(kSkyFrag));
    auto stage = [](VkShaderStageFlagBits s, VkShaderModule m) {
        return VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                               nullptr, 0, s, m, "main"};
    };

    const VkVertexInputBindingDescription bindings[2] = {
        {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    const VkVertexInputAttributeDescription attributes[9] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
        {7, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Instance, color)},
        {8, 1, VK_FORMAT_R32_UINT, offsetof(Instance, material)},
    };
    VkPipelineVertexInputStateCreateInfo meshInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    meshInput.vertexBindingDescriptionCount = 2;
    meshInput.pVertexBindingDescriptions = bindings;
    meshInput.vertexAttributeDescriptionCount = 9;
    meshInput.pVertexAttributeDescriptions = attributes;
    VkPipelineVertexInputStateCreateInfo noInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;  // ponytail: all double-sided; honor doubleSided later
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkDynamicState dynamicStates[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                       VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &kColorFormat;
    rendering.depthAttachmentFormat = kDepthFormat;

    VkPipelineShaderStageCreateInfo stages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, meshVert),
                                                 stage(VK_SHADER_STAGE_FRAGMENT_BIT, meshFrag)};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &meshInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = _layout;
    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &_pipeline);

    // Shadow: same vertex stage, depth only, slope-scaled bias.
    if (r == VK_SUCCESS) {
        VkPipelineRasterizationStateCreateInfo shadowRaster = raster;
        shadowRaster.depthBiasEnable = VK_TRUE;
        VkPipelineRenderingCreateInfo shadowRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        shadowRendering.depthAttachmentFormat = kDepthFormat;
        VkPipelineColorBlendStateCreateInfo noBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkPipelineDynamicStateCreateInfo shadowDynamic = dynamic;
        shadowDynamic.dynamicStateCount = 3;
        VkGraphicsPipelineCreateInfo sci = ci;
        sci.pNext = &shadowRendering;
        sci.stageCount = 1;
        sci.pRasterizationState = &shadowRaster;
        sci.pColorBlendState = &noBlend;
        sci.pDynamicState = &shadowDynamic;
        r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &sci, nullptr, &_shadowPipeline);
    }

    // Sky: full-screen triangle at depth 1, drawn after geometry.
    if (r == VK_SUCCESS) {
        VkPipelineShaderStageCreateInfo skyStages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, skyVert),
                                                        stage(VK_SHADER_STAGE_FRAGMENT_BIT, skyFrag)};
        VkPipelineDepthStencilStateCreateInfo skyDepth = depth;
        skyDepth.depthWriteEnable = VK_FALSE;
        skyDepth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkGraphicsPipelineCreateInfo kci = ci;
        kci.pStages = skyStages;
        kci.pVertexInputState = &noInput;
        kci.pDepthStencilState = &skyDepth;
        r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &kci, nullptr, &_skyPipeline);
    }

    for (VkShaderModule m : {meshVert, meshFrag, skyVert, skyFrag}) {
        vkDestroyShaderModule(device, m, nullptr);
    }
    Check(r, "vkCreateGraphicsPipelines");
}

Renderer::Image Renderer::_CreateImage(VkFormat format, uint32_t width, uint32_t height,
                                       uint32_t layers, VkImageUsageFlags usage,
                                       VkImageAspectFlags aspect, VkImageViewType viewType,
                                       VkImageCreateFlags flags)
{
    Image img;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    Check(vkCreateImage(_dev.device, &ci, nullptr, &img.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(_dev.device, img.image, &req);
    img.memory = _dev.Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindImageMemory(_dev.device, img.image, img.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = viewType;
    vci.format = format;
    vci.subresourceRange = Range(aspect, layers);
    Check(vkCreateImageView(_dev.device, &vci, nullptr, &img.view), "vkCreateImageView");
    return img;
}

void Renderer::_Destroy(Image &img)
{
    vkDestroyImageView(_dev.device, img.view, nullptr);
    vkDestroyImage(_dev.device, img.image, nullptr);
    vkFreeMemory(_dev.device, img.memory, nullptr);
    img = Image();
}

// Shadow cascades, BRDF LUT and the no-dome placeholder never change size.
void Renderer::_CreateConstantImages()
{
    _shadowMap = _CreateImage(kDepthFormat, kShadowSize, kShadowSize, kCascades,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    for (uint32_t c = 0; c < kCascades; ++c) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = _shadowMap.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kDepthFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1};
        Check(vkCreateImageView(_dev.device, &vci, nullptr, &_shadowLayers[c]), "vkCreateImageView");
    }

    _brdfLut = _CreateImage(VK_FORMAT_R32G32_SFLOAT, kBrdfLutSize, kBrdfLutSize, 1,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _blackCube = _CreateImage(VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 6,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE,
                              VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);

    const std::vector<float> lut = BuildBrdfLut(kBrdfLutSize);
    Buffer staging;
    _dev.Reserve(staging, lut.size() * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, lut.data(), lut.size() * sizeof(float));
    _dev.Submit([&](VkCommandBuffer cmd) {
        for (const Image *img : {&_brdfLut, &_blackCube}) {
            ImageBarrier(cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT, img == &_blackCube ? 6 : 1),
                         VK_PIPELINE_STAGE_2_NONE, 0,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kBrdfLutSize, kBrdfLutSize, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, _brdfLut.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        const VkClearColorValue black{};
        const VkImageSubresourceRange cube = Range(VK_IMAGE_ASPECT_COLOR_BIT, 6);
        vkCmdClearColorImage(cmd, _blackCube.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &cube);
        for (const Image *img : {&_brdfLut, &_blackCube}) {
            ImageBarrier(cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT, img == &_blackCube ? 6 : 1),
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });
    _dev.Destroy(staging);
}

void Renderer::_Resize(uint32_t width, uint32_t height)
{
    _Destroy(_color);
    _Destroy(_depth);
    _width = width;
    _height = height;
    _color = _CreateImage(kColorFormat, width, height, 1,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _depth = _CreateImage(kDepthFormat, width, height, 1,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _dev.Reserve(_readback, VkDeviceSize(width) * height * (8 + 4),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}

// The GPU is idle here (Render waits for every frame), so rewriting the set
// in place is legal without update-after-bind.
void Renderer::_UpdateDescriptors(const Scene::Gpu &gpu)
{
    std::vector<VkDescriptorImageInfo> images(gpu.textures.size());
    for (size_t i = 0; i < images.size(); ++i) {
        images[i] = {_sampler, gpu.textures[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    const VkDescriptorBufferInfo materials{gpu.materials, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lights{gpu.lights, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo frame{_frameData.buffer, 0, sizeof(FrameData)};
    const VkDescriptorImageInfo shadow{_shadowSampler, _shadowMap.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo env{_clampSampler, gpu.environment ? gpu.environment : _blackCube.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo lut{_clampSampler, _brdfLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    std::vector<VkWriteDescriptorSet> writes;
    auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo *buffer,
                     const VkDescriptorImageInfo *image, uint32_t count = 1) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = _set;
        w.dstBinding = binding;
        w.descriptorCount = count;
        w.descriptorType = type;
        w.pBufferInfo = buffer;
        w.pImageInfo = image;
        writes.push_back(w);
    };
    write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &materials, nullptr);
    write(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lights, nullptr);
    write(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &frame, nullptr);
    write(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &shadow);
    write(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &env);
    write(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &lut);
    if (!images.empty()) {
        write(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, images.data(),
              uint32_t(images.size()));
    }
    vkUpdateDescriptorSets(_dev.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
}

// Splits the camera frustum (clamped to the scene) into kCascades slices and
// fits a stable, texel-snapped orthographic shadow frustum to each.
bool Renderer::_FitCascades(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const
{
    if (gpu.shadowLight == kNone || !gpu.hasBounds) {
        return false;
    }
    const Mat4 invViewProj = ToMat4(frame.invViewProj);
    Vec3 nearC[4], farC[4], nearCenter, farCenter;
    for (int k = 0; k < 4; ++k) {
        const float x = (k & 1) ? 1.0f : -1.0f, y = (k & 2) ? 1.0f : -1.0f;
        nearC[k] = TransformPoint(invViewProj, {x, y, -1});
        farC[k] = TransformPoint(invViewProj, {x, y, 1});
        nearCenter = nearCenter + nearC[k] * 0.25f;
        farCenter = farCenter + farC[k] * 0.25f;
    }
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    const Vec3 forward = Normalize(farCenter - nearCenter);
    const float n = Dot(nearCenter - eye, forward), f = Dot(farCenter - eye, forward);

    Vec3 corners[8];
    float sceneFar = n;
    for (int k = 0; k < 8; ++k) {
        corners[k] = {(k & 1) ? gpu.boundsMax[0] : gpu.boundsMin[0],
                      (k & 2) ? gpu.boundsMax[1] : gpu.boundsMin[1],
                      (k & 4) ? gpu.boundsMax[2] : gpu.boundsMin[2]};
        sceneFar = std::max(sceneFar, Dot(corners[k] - eye, forward));
    }
    const float shadowFar = std::min(f, sceneFar);
    if (!(shadowFar > n) || !(f > n)) {
        return false;
    }

    const Vec3 toLight = Normalize({gpu.shadowDirection[0], gpu.shadowDirection[1], gpu.shadowDirection[2]});
    const Mat4 lightView = LookAlong(toLight * -1.0f);
    float zMin = 1e30f, zMax = -1e30f;  // scene depth range in light view space
    for (const Vec3 &c : corners) {
        const float z = TransformPoint(lightView, c).z;
        zMin = std::min(zMin, z);
        zMax = std::max(zMax, z);
    }
    const float margin = 0.01f * (zMax - zMin) + 1e-3f;

    float splitNear = n;
    for (uint32_t c = 0; c < kCascades; ++c) {
        // Practical split scheme: 75% logarithmic, 25% uniform.
        const float t = float(c + 1) / kCascades;
        const float logSplit = n > 0 ? n * std::pow(shadowFar / n, t) : shadowFar * t;
        const float splitFar = 0.75f * logSplit + 0.25f * (n + (shadowFar - n) * t);

        Vec3 slice[8], center;
        for (int k = 0; k < 4; ++k) {
            slice[k] = nearC[k] + (farC[k] - nearC[k]) * ((splitNear - n) / (f - n));
            slice[k + 4] = nearC[k] + (farC[k] - nearC[k]) * ((splitFar - n) / (f - n));
        }
        for (const Vec3 &p : slice) {
            center = center + p * 0.125f;
        }
        float radius = 0;
        for (const Vec3 &p : slice) {
            radius = std::max(radius, Length(p - center));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;  // quantized so it doesn't shimmer

        // Snap the center to whole shadow texels in light space.
        const float texel = 2 * radius / kShadowSize;
        Vec3 lc = TransformPoint(lightView, center);
        lc.x = std::floor(lc.x / texel) * texel;
        lc.y = std::floor(lc.y / texel) * texel;
        const Mat4 proj = Ortho(lc.x - radius, lc.x + radius, lc.y - radius, lc.y + radius,
                                -zMax - margin, -zMin + margin);
        const Mat4 viewProj = lightView * proj;
        std::memcpy(data->cascadeViewProj[c], viewProj.m, sizeof(viewProj.m));
        data->cascadeSplits[c] = splitFar;
        data->cascadeTexel[c] = texel;
        splitNear = splitFar;
    }
    data->cameraForward[0] = forward.x;
    data->cameraForward[1] = forward.y;
    data->cameraForward[2] = forward.z;
    return true;
}

// ponytail: synchronous, one frame in flight, CPU readback every frame.
// GL interop (step 7) removes the readback; frames in flight come with it.
void Renderer::Render(const Scene &scene, const Frame &frame, void *color, float *depth)
{
    if (frame.width == 0 || frame.height == 0) {
        return;
    }
    if (frame.width != _width || frame.height != _height) {
        _Resize(frame.width, frame.height);
    }
    const Scene::Gpu &gpu = scene.GetGpu();
    const VkDeviceSize colorBytes = VkDeviceSize(_width) * _height * 8;

    FrameData &fd = *static_cast<FrameData *>(_frameData.mapped);
    fd = FrameData{};
    std::memcpy(fd.viewProj, frame.viewProj, sizeof(fd.viewProj));
    std::memcpy(fd.invViewProj, frame.invViewProj, sizeof(fd.invViewProj));
    std::memcpy(fd.cameraPos, frame.cameraPos, sizeof(frame.cameraPos));
    fd.viewport[0] = float(_width);
    fd.viewport[1] = float(_height);
    const bool shadows = gpu.drawCount > 0 && _FitCascades(gpu, frame, &fd);
    fd.counts[0] = gpu.lightCount;
    fd.counts[1] = shadows ? gpu.shadowLight : kNone;
    fd.counts[2] = gpu.environmentLevels;
    fd.counts[3] = gpu.environmentLatlong;
    if (gpu.environment) {
        for (int i = 0; i < 9; ++i) {
            std::memcpy(fd.envSh[i], gpu.environmentSh[i], sizeof(gpu.environmentSh[i]));
        }
        std::memcpy(fd.envWorldToLocal, gpu.environmentWorldToLocal, sizeof(fd.envWorldToLocal));
        std::memcpy(fd.envRadiance, gpu.environmentRadiance, sizeof(gpu.environmentRadiance));
        fd.envRadiance[3] = 1.0f;
    }
    const bool sky = gpu.environment && frame.showEnvironment && gpu.environmentLatlong != kNone;
    _UpdateDescriptors(gpu);

    Check(vkResetCommandBuffer(_cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(_cmd, &bi), "vkBeginCommandBuffer");

    const VkBuffer vbs[2] = {gpu.vertices, gpu.instances};
    const VkDeviceSize offsets[2] = {0, 0};

    // Shadow cascades. Without a shadow light the map is only transitioned so
    // its descriptor is valid; the shader never samples it.
    const VkImageSubresourceRange shadowRange = Range(VK_IMAGE_ASPECT_DEPTH_BIT, kCascades);
    if (shadows) {
        ImageBarrier(_cmd, _shadowMap.image, shadowRange, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        for (uint32_t c = 0; c < kCascades; ++c) {
            VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = _shadowLayers[c];
            att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.clearValue.depthStencil.depth = 1.0f;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {kShadowSize, kShadowSize}};
            ri.layerCount = 1;
            ri.pDepthAttachment = &att;
            vkCmdBeginRendering(_cmd, &ri);
            const VkViewport vp{0, 0, float(kShadowSize), float(kShadowSize), 0, 1};
            const VkRect2D scissor{{0, 0}, {kShadowSize, kShadowSize}};
            vkCmdSetViewport(_cmd, 0, 1, &vp);
            vkCmdSetScissor(_cmd, 0, 1, &scissor);
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline);
            vkCmdSetDepthBias(_cmd, 0.0f, 0.0f, 1.5f);
            vkCmdPushConstants(_cmd, _layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, fd.cascadeViewProj[c]);
            vkCmdBindVertexBuffers(_cmd, 0, 2, vbs, offsets);
            vkCmdDrawIndirect(_cmd, gpu.indirect, 0, gpu.drawCount, sizeof(VkDrawIndirectCommand));
            vkCmdEndRendering(_cmd);
        }
        ImageBarrier(_cmd, _shadowMap.image, shadowRange,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
        ImageBarrier(_cmd, _shadowMap.image, shadowRange, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    ImageBarrier(_cmd, _color.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                 VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                 VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAtt.imageView = _color.view;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(colorAtt.clearValue.color.float32, frame.clearColor, sizeof(frame.clearColor));
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = _depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil.depth = frame.clearDepth;

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {_width, _height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAtt;
    ri.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(_cmd, &ri);

    // No Y flip: NDC y=-1 lands in memory row 0, which is the bottom row
    // in GL/Hydra order, so readback needs no flip either.
    const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
    const VkRect2D scissor{{0, 0}, {_width, _height}};
    vkCmdSetViewport(_cmd, 0, 1, &vp);
    vkCmdSetScissor(_cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    if (gpu.drawCount > 0) {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
        vkCmdPushConstants(_cmd, _layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, frame.viewProj);
        vkCmdBindVertexBuffers(_cmd, 0, 2, vbs, offsets);
        vkCmdDrawIndirect(_cmd, gpu.indirect, 0, gpu.drawCount, sizeof(VkDrawIndirectCommand));
    }
    if (sky) {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skyPipeline);
        vkCmdDraw(_cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(_cmd);

    ImageBarrier(_cmd, _color.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {_width, _height, 1};
    vkCmdCopyImageToBuffer(_cmd, _color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           _readback.buffer, 1, &copy);
    copy.bufferOffset = colorBytes;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImageToBuffer(_cmd, _depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           _readback.buffer, 1, &copy);

    VkBufferMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = _readback.buffer;
    toHost.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(_cmd, &dep);

    Check(vkEndCommandBuffer(_cmd), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = _cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    Check(vkQueueSubmit2(_dev.queue, 1, &si, _fence), "vkQueueSubmit2");
    Check(vkWaitForFences(_dev.device, 1, &_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    Check(vkResetFences(_dev.device, 1, &_fence), "vkResetFences");

    const auto *src = static_cast<const uint8_t *>(_readback.mapped);
    std::memcpy(color, src, colorBytes);
    if (depth) {
        std::memcpy(depth, src + colorBytes, VkDeviceSize(_width) * _height * 4);
    }
}

} // namespace vkr
