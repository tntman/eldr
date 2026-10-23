#include "renderer.h"

#include "ibl.h"
#include "vkmath.h"

#include <algorithm>
#include <cstring>
#include <tuple>
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
    float prevViewProj[16];
    float ssr[4];  // enabled, noise index
    float clusters[4];       // near, far, log2(far / near), light cutoff radiance
    uint32_t localLights[4]; // sphere/rect light count
};

namespace {

const uint32_t kMeshVert[] =
#include "mesh.vert.inc"
;
const uint32_t kMeshFrag[] =
#include "mesh.frag.inc"
;
const uint32_t kFullscreenVert[] =
#include "fullscreen.vert.inc"
;
const uint32_t kSkyFrag[] =
#include "sky.frag.inc"
;
const uint32_t kCullComp[] =
#include "cull.comp.inc"
;
const uint32_t kLightsComp[] =
#include "lights.comp.inc"
;

// Light clusters (shaders/lights.glsl): 16 x 9 x 24, a count and 63
// indices each.
constexpr VkDeviceSize kClusterBytes = 16 * 9 * 24 * 64 * sizeof(uint32_t);

// Push constants of cull.comp; the vertex stages read only the first 64 bytes
// (mat4 viewProj) of the same range.
struct CullParams {
    float planes[6][4];
    uint32_t instanceCapacity;
    uint32_t drawCount;
    float pixelScale;    // perspective: pixels per world unit at view depth 1; ortho: per unit
    float nearDistance;  // view depth of the near plane
    float minRadius;     // in pixels: smaller instances are dropped
    float orthographic;
};
constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

// World-space frustum planes (xyz . p + w >= 0 inside) of a GL clip-space
// viewProj in row-vector order (Gribb/Hartmann).
void FrustumPlanes(const float *vp, float planes[6][4])
{
    auto column = [&](int j, float out[4]) {
        for (int i = 0; i < 4; ++i) {
            out[i] = vp[i * 4 + j];
        }
    };
    float c[4][4];
    for (int j = 0; j < 4; ++j) {
        column(j, c[j]);
    }
    for (int p = 0; p < 6; ++p) {
        const float sign = (p % 2 == 0) ? 1.0f : -1.0f;  // left/right, bottom/top, near/far
        for (int i = 0; i < 4; ++i) {
            planes[p][i] = c[3][i] + sign * c[p / 2][i];
        }
        const float len = std::sqrt(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] +
                                    planes[p][2] * planes[p][2]);
        for (int i = 0; i < 4; ++i) {
            planes[p][i] /= len > 0 ? len : 1.0f;
        }
    }
}

constexpr uint32_t kBrdfLutSize = 64;

// Spots with a cone up to this get one perspective atlas tile, wider
// lights a cube of six. Must match kSpotShadowCone in mesh.frag.
constexpr float kSpotShadowCone = 1.3962634f;  // 80 degrees

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

// Low-discrepancy sub-pixel offsets in [0, 1) for TAA jitter.
float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    for (; index > 0; index /= base) {
        f /= float(base);
        r += f * float(index % base);
    }
    return r;
}

// Frames a still camera accumulates before TAA reports converged; while
// the camera moves the blend weight stays at 1 / (kTaaMoving + 1) = 0.1.
constexpr uint32_t kTaaSamples = 16, kTaaMoving = 9;

} // namespace

Renderer::Renderer(Device &device)
    : _dev(device)
    , _gtao(device)
    , _taa(device)
    , _post(device)
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
    _clusters = _dev.CreateDeviceBuffer(kClusterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    _CreateDescriptors();
    _CreatePipelines();
    _CreateConstantImages();
}

Renderer::~Renderer()
{
    vkDeviceWaitIdle(_dev.device);
    for (VkImageView v : _shadowLayers) {
        vkDestroyImageView(_dev.device, v, nullptr);
    }
    for (Image *img : {&_color, &_primId, &_instanceId, &_depth, &_shadowMap, &_shadowAtlas, &_brdfLut,
                       &_blackCube}) {
        _dev.Destroy(*img);
    }
    _dev.Destroy(_readback);
    _dev.Destroy(_frameData);
    _dev.Destroy(_commands);
    _dev.Destroy(_clusters);
    _dev.Destroy(_lightShadows);
    _dev.Destroy(_shadowTiles);
    for (VkPipeline p : {_prepassPipeline, _pipeline, _shadowPipeline, _skyPipeline, _cullPipeline,
                         _lightsPipeline}) {
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
    constexpr VkShaderStageFlags kGeometry = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 0 is the bindless array, partially bound and indexed by material
    // data. 7..10 are the GPU-driven scene: instances, draw infos, visible
    // lists and the culled indirect commands; 11 is the GTAO result, 12
    // and 13 last frame's color and this frame's depth for reflections, 14
    // the light clusters (written by lights.comp, which also reads 2 and 3),
    // 15 and 16 the skinning pools (SkinVertex, joint matrices), 17..19
    // the local lights' shadow atlas, first tile per light and the tiles, 20
    // the atlas again without depth compare (soft shadows' blocker search).
    constexpr VkShaderStageFlags kLighting = kFragment | VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorSetLayoutBinding bindings[21] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures, kFragment},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kLighting},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kLighting},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kLighting},
        {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
    };
    const VkDescriptorBindingFlags bindingFlags[21] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 21;
    flags.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags};
    slci.bindingCount = 21;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(_dev.device, &slci, nullptr, &_setLayout),
          "vkCreateDescriptorSetLayout");

    const VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11},
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
    // One range for both users (vertex: mat4 viewProj; cull: CullParams), so
    // every push names the same stages and the ranges never partly overlap.
    VkPushConstantRange range{kPushStages, 0, sizeof(CullParams)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(_dev.device, &lci, nullptr, &_layout), "vkCreatePipelineLayout");

    const VkDevice device = _dev.device;
    for (auto [code, bytes, pipeline] : {std::tuple{kCullComp, sizeof(kCullComp), &_cullPipeline},
                                         std::tuple{kLightsComp, sizeof(kLightsComp), &_lightsPipeline}}) {
        VkShaderModule module = CreateShaderModule(device, code, bytes);
        VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
        cci.layout = _layout;
        const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cci, nullptr, pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        Check(r, "vkCreateComputePipelines");
    }
    VkShaderModule meshVert = CreateShaderModule(device, kMeshVert, sizeof(kMeshVert));
    VkShaderModule meshFrag = CreateShaderModule(device, kMeshFrag, sizeof(kMeshFrag));
    VkShaderModule skyVert = CreateShaderModule(device, kFullscreenVert, sizeof(kFullscreenVert));
    VkShaderModule skyFrag = CreateShaderModule(device, kSkyFrag, sizeof(kSkyFrag));
    auto stage = [](VkShaderStageFlagBits s, VkShaderModule m) {
        return VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                               nullptr, 0, s, m, "main"};
    };

    // Per-instance data comes from SSBOs (scene.glsl), not vertex attributes.
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
    };
    VkPipelineVertexInputStateCreateInfo meshInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    meshInput.vertexBindingDescriptionCount = 1;
    meshInput.pVertexBindingDescriptions = &binding;
    meshInput.vertexAttributeDescriptionCount = 3;
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
    // Prepass and shadows write depth; shading (and the sky) only test
    // against the finished prepass depth.
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineDepthStencilStateCreateInfo depthReadOnly = depth;
    depthReadOnly.depthWriteEnable = VK_FALSE;
    depthReadOnly.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // Attachments: color, primId, instanceId. Identical blend states, so no
    // independentBlend feature is needed (the sky writes -1 ids).
    VkPipelineColorBlendAttachmentState attachments[3] = {};
    for (VkPipelineColorBlendAttachmentState &a : attachments) {
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 3;
    blend.pAttachments = attachments;
    const VkFormat colorFormats[3] = {kColorFormat, kIdFormat, kIdFormat};
    VkDynamicState dynamicStates[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                       VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 3;
    rendering.pColorAttachmentFormats = colorFormats;
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
    ci.pDepthStencilState = &depthReadOnly;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = _layout;
    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &_pipeline);

    // Depth prepass: same vertex stage, depth only. Shadow: plus slope-scaled bias.
    if (r == VK_SUCCESS) {
        VkPipelineRenderingCreateInfo depthRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        depthRendering.depthAttachmentFormat = kDepthFormat;
        VkPipelineColorBlendStateCreateInfo noBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo pci = ci;
        pci.pNext = &depthRendering;
        pci.stageCount = 1;
        pci.pDepthStencilState = &depth;
        pci.pColorBlendState = &noBlend;
        r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &_prepassPipeline);

        VkPipelineRasterizationStateCreateInfo shadowRaster = raster;
        shadowRaster.depthBiasEnable = VK_TRUE;
        VkPipelineDynamicStateCreateInfo shadowDynamic = dynamic;
        shadowDynamic.dynamicStateCount = 3;
        VkGraphicsPipelineCreateInfo sci = pci;
        sci.pRasterizationState = &shadowRaster;
        sci.pDynamicState = &shadowDynamic;
        if (r == VK_SUCCESS) {
            r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &sci, nullptr, &_shadowPipeline);
        }
    }

    // Sky: full-screen triangle at depth 1, drawn after geometry.
    if (r == VK_SUCCESS) {
        VkPipelineShaderStageCreateInfo skyStages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, skyVert),
                                                        stage(VK_SHADER_STAGE_FRAGMENT_BIT, skyFrag)};
        VkGraphicsPipelineCreateInfo kci = ci;
        kci.pStages = skyStages;
        kci.pVertexInputState = &noInput;
        r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &kci, nullptr, &_skyPipeline);
    }

    for (VkShaderModule m : {meshVert, meshFrag, skyVert, skyFrag}) {
        vkDestroyShaderModule(device, m, nullptr);
    }
    Check(r, "vkCreateGraphicsPipelines");
}

// Shadow cascades, BRDF LUT and the no-dome placeholder never change size.
void Renderer::_CreateConstantImages()
{
    _shadowMap = _dev.CreateImage(kDepthFormat, kShadowSize, kShadowSize, kCascades, 1,
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

    _shadowAtlas = _dev.CreateImage(kDepthFormat, kShadowAtlasSize, kShadowAtlasSize, 1, 1,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _brdfLut = _dev.CreateImage(VK_FORMAT_R32G32_SFLOAT, kBrdfLutSize, kBrdfLutSize, 1, 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _blackCube = _dev.CreateImage(VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 6, 1,
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
    for (Image *img : {&_color, &_primId, &_instanceId, &_depth}) {
        _dev.Destroy(*img);
    }
    _width = width;
    _height = height;
    constexpr VkImageUsageFlags kTarget = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    _color = _dev.CreateImage(kColorFormat, width, height, 1, 1, kTarget,
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _primId = _dev.CreateImage(kIdFormat, width, height, 1, 1, kTarget,
                               VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _instanceId = _dev.CreateImage(kIdFormat, width, height, 1, 1, kTarget,
                                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _depth = _dev.CreateImage(kDepthFormat, width, height, 1, 1,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _dev.Reserve(_readback, VkDeviceSize(width) * height * (8 + 4 + 4 + 4),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    _gtao.Resize(width, height, _depth.view);
    _taa.Resize(width, height, _color.view, _depth.view);
    _post.Resize(width, height, _taa.Output());
    _taaValid = false;
    _historyValid = false;
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
    const VkDescriptorBufferInfo instances{gpu.instances, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo draws{gpu.draws, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo visible{gpu.visible, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo commands{_commands.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo ao{_clampSampler, _gtao.Output(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo previous{_clampSampler, _taa.History(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo depth{_clampSampler, _depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo clusters{_clusters.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo skin{gpu.skin, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo joints{gpu.joints, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo atlas{_shadowSampler, _shadowAtlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo atlasDepth{_clampSampler, _shadowAtlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo lightShadows{_lightShadows.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo shadowTiles{_shadowTiles.buffer, 0, VK_WHOLE_SIZE};

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
    write(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &instances, nullptr);
    write(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &draws, nullptr);
    write(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &visible, nullptr);
    write(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &commands, nullptr);
    write(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &ao);
    write(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &previous);
    write(13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &depth);
    write(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &clusters, nullptr);
    write(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &skin, nullptr);
    write(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &joints, nullptr);
    write(17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &atlas);
    write(18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lightShadows, nullptr);
    write(19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &shadowTiles, nullptr);
    write(20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &atlasDepth);
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

// Depth range of the light clusters (near plane to the scene's far end,
// along the view axis) and the cutoff that bounds each light's range: 1% of
// the radiance the current exposure maps to middle gray.
void Renderer::_FitClusters(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const
{
    const Mat4 invViewProj = ToMat4(frame.invViewProj);
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    const Vec3 nearCenter = TransformPoint(invViewProj, {0, 0, -1});
    const Vec3 forward = Normalize(TransformPoint(invViewProj, {0, 0, 0}) - nearCenter);
    float n = Dot(nearCenter - eye, forward);
    float f = Dot(TransformPoint(invViewProj, {0, 0, 1}) - eye, forward);
    if (!(f < 1e30f)) {
        f = 1e30f;  // infinite far plane
    }
    if (gpu.hasBounds) {
        float sceneFar = n;
        for (int k = 0; k < 8; ++k) {
            const Vec3 c{(k & 1) ? gpu.boundsMax[0] : gpu.boundsMin[0], (k & 2) ? gpu.boundsMax[1] : gpu.boundsMin[1],
                         (k & 4) ? gpu.boundsMax[2] : gpu.boundsMin[2]};
            sceneFar = std::max(sceneFar, Dot(c - eye, forward));
        }
        f = std::min(f, sceneFar);
    }
    n = std::max(n, 1e-5f * std::max(f, 1e-3f));  // log slices need near > 0
    f = std::max(f, n * 1.01f);
    const float exposure = frame.postProcess ? _post.Exposure() : 1.0f;
    data->clusters[0] = n;
    data->clusters[1] = f;
    data->clusters[2] = std::log2(f / n);
    data->clusters[3] = 0.01f * 0.18f / exposure;
    data->cameraForward[0] = forward.x;
    data->cameraForward[1] = forward.y;
    data->cameraForward[2] = forward.z;
}

// std430 mirror of ShadowTile in mesh.frag.
struct Renderer::ShadowTile {
    float viewProj[16];
    float rect[4];    // atlas uv offset, uv scale
    float params[4];  // shadow texel size per unit of distance from the light, near, far, light size
};

// Gives atlas tiles to the shadow-casting lights whose range reaches into
// the view, brightest at the camera first, until the atlas is full; the
// rest stay unshadowed. Writes each light's first tile (or kNone).
// ponytail: fixed 512 tiles redrawn every frame, each with every caster;
// per-tile culling and caching static lights' tiles come when it matters.
std::vector<Renderer::ShadowTile> Renderer::_PlanLocalShadows(const Scene::Gpu &gpu, const Frame &frame,
                                                              float cutoff)
{
    const uint32_t lights = gpu.lightCount + gpu.localLightCount;
    _dev.Reserve(_lightShadows, std::max(lights, 1u) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto *firstTile = static_cast<uint32_t *>(_lightShadows.mapped);
    std::fill(firstTile, firstTile + lights, kNone);
    std::vector<ShadowTile> tiles;

    float planes[6][4];
    FrustumPlanes(frame.viewProj, planes);
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    struct Candidate {
        const Scene::Gpu::ShadowCaster *caster;
        float range, priority;
    };
    std::vector<Candidate> candidates;
    for (const Scene::Gpu::ShadowCaster &c : gpu.shadowCasters) {
        const float range = std::sqrt(c.rangeIntensity / cutoff) + c.extent;
        bool inView = range > 0.0f;
        for (const auto &p : planes) {
            inView = inView && p[0] * c.position[0] + p[1] * c.position[1] + p[2] * c.position[2] + p[3] >= -range;
        }
        if (inView) {
            const Vec3 d = Vec3{c.position[0], c.position[1], c.position[2]} - eye;
            candidates.push_back({&c, range, c.rangeIntensity / std::max(Dot(d, d), 1e-12f)});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.priority > b.priority; });

    constexpr uint32_t perRow = kShadowAtlasSize / kShadowTileSize, capacity = perRow * perRow;
    constexpr float tileUv = float(kShadowTileSize) / float(kShadowAtlasSize);
    const Vec3 cube[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Candidate &cand : candidates) {
        const Scene::Gpu::ShadowCaster &c = *cand.caster;
        const bool spot = c.coneAngle <= kSpotShadowCone;
        if (tiles.size() + (spot ? 1 : 6) > capacity) {
            continue;
        }
        firstTile[c.light] = uint32_t(tiles.size());
        const Vec3 position{c.position[0], c.position[1], c.position[2]};
        const float n = std::max(c.radius * 1.05f, cand.range * 1e-4f), f = std::max(cand.range, n * 2.0f);
        // A few texels of margin: PCF near a tile's (or cube face's) edge
        // must not read the neighbour.
        const float margin = 1.0f + 6.0f / float(kShadowTileSize);
        const float tanHalf = (spot ? std::tan(c.coneAngle) : 1.0f) * margin;
        auto add = [&](Vec3 direction) {
            ShadowTile t{};
            const Mat4 viewProj = Translate(position * -1.0f) * LookAlong(direction) * Perspective(tanHalf, n, f);
            std::memcpy(t.viewProj, viewProj.m, sizeof(t.viewProj));
            const uint32_t k = uint32_t(tiles.size());
            t.rect[0] = float(k % perRow) * tileUv;
            t.rect[1] = float(k / perRow) * tileUv;
            t.rect[2] = t.rect[3] = tileUv;
            t.params[0] = 2.0f * tanHalf / float(kShadowTileSize);
            t.params[1] = n;
            t.params[2] = f;
            t.params[3] = c.size;
            tiles.push_back(t);
        };
        if (spot) {
            add({c.axis[0], c.axis[1], c.axis[2]});
        } else {
            for (const Vec3 &d : cube) {
                add(d);  // same face order as LocalShadow() in mesh.frag
            }
        }
    }
    _dev.Reserve(_shadowTiles, std::max<size_t>(tiles.size(), 1) * sizeof(ShadowTile),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(tiles.begin(), tiles.end(), static_cast<ShadowTile *>(_shadowTiles.mapped));
    return tiles;
}

// All tiles in one rendering pass over the atlas, each through its own
// viewport. Without tiles the atlas is only transitioned for its descriptor.
void Renderer::_RenderLocalShadows(const Scene::Gpu &gpu, const std::vector<ShadowTile> &tiles)
{
    const VkImageSubresourceRange range = Range(VK_IMAGE_ASPECT_DEPTH_BIT);
    if (tiles.empty()) {
        ImageBarrier(_cmd, _shadowAtlas.image, range, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return;
    }
    constexpr VkPipelineStageFlags2 kTests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    ImageBarrier(_cmd, _shadowAtlas.image, range, VK_PIPELINE_STAGE_2_NONE, 0,
                 kTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = _shadowAtlas.view;
    att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kShadowAtlasSize, kShadowAtlasSize}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &att;
    vkCmdBeginRendering(_cmd, &ri);
    // GTAO bound its own set; the shadow pass needs the scene's.
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline);
    vkCmdSetDepthBias(_cmd, 0.0f, 0.0f, 1.5f);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
    vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
    for (const ShadowTile &t : tiles) {
        const float x = t.rect[0] * kShadowAtlasSize, y = t.rect[1] * kShadowAtlasSize;
        const VkViewport vp{x, y, float(kShadowTileSize), float(kShadowTileSize), 0, 1};
        const VkRect2D scissor{{int32_t(x), int32_t(y)}, {kShadowTileSize, kShadowTileSize}};
        vkCmdSetViewport(_cmd, 0, 1, &vp);
        vkCmdSetScissor(_cmd, 0, 1, &scissor);
        vkCmdPushConstants(_cmd, _layout, kPushStages, 0, 64, t.viewProj);
        vkCmdDrawIndexedIndirect(_cmd, gpu.shadowCommands, 0, gpu.drawCount, sizeof(VkDrawIndexedIndirectCommand));
    }
    vkCmdEndRendering(_cmd);
    ImageBarrier(_cmd, _shadowAtlas.image, range, kTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool Renderer::IsConverged() const
{
    return (!_adapting || _post.ExposureConverged()) && _taaConverged;
}

uint32_t Renderer::VisibleInstances() const
{
    const auto *commands = static_cast<const VkDrawIndexedIndirectCommand *>(_commands.mapped);
    uint32_t total = 0;
    for (uint32_t i = 0; i < _lastDrawCount; ++i) {
        total += commands[i].instanceCount;
    }
    return total;
}

// ponytail: synchronous, one frame in flight, CPU readback every frame.
// GL interop (step 7) removes the readback; frames in flight come with it.
void Renderer::Render(const Scene &scene, const Frame &frame, const Targets &out)
{
    if (frame.width == 0 || frame.height == 0) {
        return;
    }
    if (frame.width != _width || frame.height != _height) {
        _Resize(frame.width, frame.height);
    }
    const Scene::Gpu &gpu = scene.GetGpu();

    FrameData &fd = *static_cast<FrameData *>(_frameData.mapped);
    fd = FrameData{};
    std::memcpy(fd.viewProj, frame.viewProj, sizeof(fd.viewProj));
    std::memcpy(fd.invViewProj, frame.invViewProj, sizeof(fd.invViewProj));

    // TAA: offset the main pass by a sub-pixel Halton jitter. The camera
    // matrices used for culling, shadows and reprojection stay unjittered.
    const bool taa = frame.postProcess && frame.taa;
    float jitteredViewProj[16];
    std::memcpy(jitteredViewProj, frame.viewProj, sizeof(jitteredViewProj));
    Mat4 unjitter;          // jittered NDC -> unjittered NDC
    uint32_t sample = 0;    // jitter (and GTAO noise) index, 1..kTaaSamples
    if (taa) {
        sample = (_haltonIndex++ % kTaaSamples) + 1;
        const float jx = (Halton(sample, 2) - 0.5f) * 2.0f / float(_width);
        const float jy = (Halton(sample, 3) - 0.5f) * 2.0f / float(_height);
        for (int r = 0; r < 4; ++r) {  // clip.xy += jitter * clip.w
            jitteredViewProj[r * 4 + 0] += jx * jitteredViewProj[r * 4 + 3];
            jitteredViewProj[r * 4 + 1] += jy * jitteredViewProj[r * 4 + 3];
        }
        unjitter.m[3][0] = -jx;
        unjitter.m[3][1] = -jy;
        // The sky reconstructs rays per pixel, so it follows the jitter too.
        const Mat4 invJittered = unjitter * ToMat4(frame.invViewProj);
        std::memcpy(fd.invViewProj, invJittered.m, sizeof(fd.invViewProj));
    }
    std::memcpy(fd.viewProj, jitteredViewProj, sizeof(fd.viewProj));

    // Reflections shade hits with last frame's resolved image, which exists
    // once post (TAA resolve) ran at this size.
    if (frame.ssr && frame.postProcess && _historyValid) {
        std::memcpy(fd.prevViewProj, _prevViewProj, sizeof(fd.prevViewProj));
        fd.ssr[0] = 1.0f;
        fd.ssr[1] = float(sample);
    }

    std::memcpy(fd.cameraPos, frame.cameraPos, sizeof(frame.cameraPos));
    fd.viewport[0] = float(_width);
    fd.viewport[1] = float(_height);
    const bool shadows = gpu.drawCount > 0 && gpu.instanceCapacity > 0 && _FitCascades(gpu, frame, &fd);
    fd.counts[0] = gpu.lightCount;
    fd.localLights[0] = gpu.localLightCount;
    fd.localLights[1] = sample;
    if (gpu.localLightCount > 0) {
        _FitClusters(gpu, frame, &fd);
    }
    const std::vector<ShadowTile> shadowTiles = _PlanLocalShadows(gpu, frame, fd.clusters[3]);
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
    // Culling output; host-visible so VisibleInstances() can read it back.
    _dev.Reserve(_commands, VkDeviceSize(gpu.drawCount) * sizeof(VkDrawIndexedIndirectCommand),
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    _UpdateDescriptors(gpu);
    const bool draw = gpu.drawCount > 0 && gpu.instanceCapacity > 0;
    _lastDrawCount = draw ? gpu.drawCount : 0;

    Check(vkResetCommandBuffer(_cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(_cmd, &bi), "vkBeginCommandBuffer");

    const VkDeviceSize vertexOffset = 0;
    if (draw) {
        // GPU culling: reset the commands from the scene's template, then one
        // invocation per instance slot appends survivors and counts them.
        const VkBufferCopy reset{0, 0, VkDeviceSize(gpu.drawCount) * sizeof(VkDrawIndexedIndirectCommand)};
        vkCmdCopyBuffer(_cmd, gpu.commandTemplate, _commands.buffer, 1, &reset);
        VkMemoryBarrier2 toCull{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toCull.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toCull.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toCull.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toCull.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &toCull;
        vkCmdPipelineBarrier2(_cmd, &dep);

        CullParams cull{};
        FrustumPlanes(frame.viewProj, cull.planes);
        cull.instanceCapacity = gpu.instanceCapacity;
        cull.drawCount = gpu.drawCount;
        // Screen size of an instance: its world radius over the size of a
        // pixel at its depth (which grows linearly with depth unless ortho).
        const Mat4 inv = ToMat4(frame.invViewProj);
        const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
        const Vec3 nearCenter = TransformPoint(inv, {0, 0, -1}), mid = TransformPoint(inv, {0, 0, 0});
        const Vec3 forward = Normalize(mid - nearCenter);
        const float pixel = Length(TransformPoint(inv, {0, 2.0f / float(_height), 0}) - mid);
        cull.orthographic = std::abs(frame.viewProj[3]) + std::abs(frame.viewProj[7]) +
                            std::abs(frame.viewProj[11]) < 1e-9f ? 1.0f : 0.0f;
        cull.pixelScale = (cull.orthographic > 0 ? 1.0f : Dot(mid - eye, forward)) / std::max(pixel, 1e-30f);
        cull.nearDistance = Dot(nearCenter - eye, forward);
        cull.minRadius = 0.5f;
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(cull), &cull);
        vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);

        VkMemoryBarrier2 toDraw{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toDraw.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toDraw.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toDraw.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                              VK_PIPELINE_STAGE_2_HOST_BIT;
        toDraw.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                               VK_ACCESS_2_HOST_READ_BIT;
        dep.pMemoryBarriers = &toDraw;
        vkCmdPipelineBarrier2(_cmd, &dep);
    }
    if (gpu.localLightCount > 0) {
        // Light clusters, read by the shading pass.
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _lightsPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdDispatch(_cmd, (16 * 9 * 24 + 63) / 64, 1, 1);
        VkMemoryBarrier2 toShading{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toShading.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toShading.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toShading.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toShading.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &toShading;
        vkCmdPipelineBarrier2(_cmd, &dep);
    }
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);

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
            vkCmdPushConstants(_cmd, _layout, kPushStages, 0, 64, fd.cascadeViewProj[c]);
            vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
            vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
            // ponytail: shadows draw every instance; per-cascade culling later.
            vkCmdDrawIndexedIndirect(_cmd, gpu.shadowCommands, 0, gpu.drawCount,
                                     sizeof(VkDrawIndexedIndirectCommand));
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

    // Depth prepass: the main pass then shades each pixel once, and GTAO
    // reads the finished depth before it. From here on depth is read-only.
    constexpr VkPipelineStageFlags2 kDepthTests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                 VK_PIPELINE_STAGE_2_NONE, 0, kDepthTests,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = _depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil.depth = frame.clearDepth;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {_width, _height}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(_cmd, &ri);
    // No Y flip: NDC y=-1 lands in memory row 0, which is the bottom row
    // in GL/Hydra order, so readback needs no flip either.
    const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
    const VkRect2D scissor{{0, 0}, {_width, _height}};
    vkCmdSetViewport(_cmd, 0, 1, &vp);
    vkCmdSetScissor(_cmd, 0, 1, &scissor);
    if (draw) {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _prepassPipeline);
        vkCmdPushConstants(_cmd, _layout, kPushStages, 0, 64, jitteredViewProj);
        vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
        vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(_cmd, _commands.buffer, 0, gpu.drawCount,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }
    vkCmdEndRendering(_cmd);
    ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                 kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 kDepthTests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);

    // AO only darkens the dome lighting, so without a dome it is skipped.
    if (frame.ambientOcclusion && draw && gpu.environment) {
        _gtao.Record(_cmd, fd.invViewProj, frame.cameraPos, frame.aoRadius, sample);
    } else {
        _gtao.Clear(_cmd);
    }

    _RenderLocalShadows(gpu, draw ? shadowTiles : std::vector<ShadowTile>());

    const Image *colorTargets[3] = {&_color, &_primId, &_instanceId};
    for (const Image *img : colorTargets) {
        ImageBarrier(_cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    VkRenderingAttachmentInfo colorAtts[3] = {};
    for (int i = 0; i < 3; ++i) {
        colorAtts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtts[i].imageView = colorTargets[i]->view;
        colorAtts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtts[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::fill(colorAtts[i].clearValue.color.int32, colorAtts[i].clearValue.color.int32 + 4, -1);
    }
    // With post on, the scene is premultiplied by coverage (cleared to
    // transparent) and the tonemap composites the app's colour behind it.
    const float transparent[4] = {0, 0, 0, 0};
    std::memcpy(colorAtts[0].clearValue.color.float32, frame.postProcess ? transparent : frame.clearColor,
                sizeof(frame.clearColor));
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = colorAtts;
    vkCmdBeginRendering(_cmd, &ri);
    vkCmdSetViewport(_cmd, 0, 1, &vp);
    vkCmdSetScissor(_cmd, 0, 1, &scissor);
    // GTAO bound its own set; the sky and shading need the scene's again.
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    if (draw) {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
        vkCmdPushConstants(_cmd, _layout, kPushStages, 0, 64, jitteredViewProj);
        vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
        vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(_cmd, _commands.buffer, 0, gpu.drawCount,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }
    if (sky) {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skyPipeline);
        vkCmdDraw(_cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(_cmd);

    constexpr VkPipelineStageFlags2 kPostStages =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    _adapting = frame.postProcess && frame.post.autoExposure;
    if (frame.postProcess) {
        // HDR color feeds TAA and post (TAA also reads the read-only depth).
        ImageBarrier(_cmd, _color.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     kPostStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // TAA weight: 1 without usable history; 1/(n+1) over n history
        // frames, capped at kTaaMoving while the camera moves (0.1) and at
        // kTaaSamples - 1 while it is still (a 16-sample average).
        float alpha = 1.0f;
        Mat4 reprojection;
        if (taa) {
            const bool moved = _taaValid && std::memcmp(frame.viewProj, _prevViewProj, sizeof(_prevViewProj)) != 0;
            if (_taaValid) {
                reprojection = unjitter * ToMat4(frame.invViewProj) * ToMat4(_prevViewProj);
            }
            if (!_taaValid) {
                _taaFrames = 0;
                _taaWarmup = true;
            } else if (moved) {
                _taaFrames = std::min(_taaFrames, kTaaMoving);
                _taaWarmup = false;
            }
            alpha = 1.0f / float(_taaFrames + 1);
            _taaConverged = _taaValid && !moved && _taaFrames == kTaaSamples - 1;  // sample 16 of a still camera
            _taaFrames = std::min(_taaFrames + 1, kTaaSamples - 1);
            _taaValid = true;
        } else {
            _taaValid = false;
            _taaWarmup = false;
            _taaConverged = true;
        }

        // Adaptation runs on wall-clock time. The first frame, toggling
        // auto-exposure and a fresh TAA history snap straight to the target:
        // while TAA accumulates a new image its histogram still shifts, and
        // the settled image should be exposed for itself.
        const auto now = std::chrono::steady_clock::now();
        const float dt = _hasLastFrame
            ? std::min(std::chrono::duration<float>(now - _lastFrame).count(), 0.1f) : 0.0f;
        const bool reset = !_hasLastFrame || frame.post.autoExposure != _lastAutoExposure || _taaWarmup;
        _lastFrame = now;
        _hasLastFrame = true;
        _lastAutoExposure = frame.post.autoExposure;
        if (_taaConverged) {
            _taaWarmup = false;
        }

        _taa.Record(_cmd, &reprojection.m[0][0], alpha);
        _post.Record(_cmd, frame.post, dt, reset, frame.clearColor);
        std::memcpy(_prevViewProj, frame.viewProj, sizeof(_prevViewProj));
        _historyValid = true;
    } else {
        _taaValid = false;
        _taaConverged = true;
        _historyValid = false;
    }

    // Everything that may be read back goes to TRANSFER_SRC (with post on,
    // color comes from the post output, already in TRANSFER_SRC).
    for (const Image *img : colorTargets) {
        if (img == &_color && frame.postProcess) {
            continue;
        }
        ImageBarrier(_cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                 kDepthTests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // Copy only what the caller asked for, packed one after another.
    struct Output {
        VkImage image;
        VkImageAspectFlags aspect;
        VkDeviceSize bytesPerPixel;
        void *dst;
        VkDeviceSize offset;
    };
    Output outputs[4] = {
        {frame.postProcess ? _post.Output() : _color.image, VK_IMAGE_ASPECT_COLOR_BIT, 8, out.color, 0},
        {_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 4, out.depth, 0},
        {_primId.image, VK_IMAGE_ASPECT_COLOR_BIT, 4, out.primId, 0},
        {_instanceId.image, VK_IMAGE_ASPECT_COLOR_BIT, 4, out.instanceId, 0},
    };
    VkDeviceSize offset = 0;
    for (Output &o : outputs) {
        if (!o.dst) {
            continue;
        }
        o.offset = offset;
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {o.aspect, 0, 0, 1};
        copy.imageExtent = {_width, _height, 1};
        vkCmdCopyImageToBuffer(_cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               _readback.buffer, 1, &copy);
        offset += o.bytesPerPixel * _width * _height;
    }

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
    for (const Output &o : outputs) {
        if (o.dst) {
            std::memcpy(o.dst, src + o.offset, o.bytesPerPixel * _width * _height);
        }
    }
}

} // namespace vkr
