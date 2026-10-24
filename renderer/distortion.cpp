#include "distortion.h"

#include <algorithm>
#include <cmath>

namespace vkr {

namespace {

const uint32_t kFullscreenVert[] =
#include "fullscreen.vert.inc"
;
const uint32_t kDistortionFrag[] =
#include "distortion.frag.inc"
;

constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kIdFormat = VK_FORMAT_R32_SINT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

struct Push {  // mirror of Params in distortion.frag
    float a[4], b[4], c[4], clearColor[4], d[4], e[4];
};

VkImageSubresourceRange Range(VkImageAspectFlags aspect)
{
    return {aspect, 0, 1, 0, 1};
}

} // namespace

bool LensDistortion::Params::Active() const
{
    return fisheye || k1 != 0 || k2 != 0 || k3 != 0 || k4 != 0 || k5 != 0 || k6 != 0 || p1 != 0 || p2 != 0 ||
           asym[0] != 0 || asym[1] != 0 || anaSq != 1 || scale != 1 || center[0] != 0 || center[1] != 0;
}

bool LensDistortion::Warp(const Params &p, float, const float s[2], float source[2])
{
    float qx = (s[0] - p.center[0]) * p.anaSq, qy = s[1] - p.center[1];
    const float r2 = qx * qx + qy * qy;
    bool valid = true;
    float k;
    if (p.fisheye) {
        const float rho = std::sqrt(r2) * p.tanHalfWidth, rho2 = rho * rho;
        const float theta = rho * (1 + p.k1 * rho2 + p.k2 * rho2 * rho2);
        valid = theta < 1.5707f;
        k = rho > 1e-6f ? std::tan(std::min(theta, 1.5707f)) / rho : 1.0f;
    } else {
        const float r4 = r2 * r2, r6 = r4 * r2;
        k = (1 + p.k1 * r2 + p.k2 * r4 + p.k3 * r6) / (1 + p.k4 * r2 + p.k5 * r4 + p.k6 * r6);
    }
    const float x = qx, y = qy;
    qx = x * k + 2 * p.p1 * x * y + p.p2 * (r2 + 2 * x * x) + p.asym[0] * r2;
    qy = y * k + p.p1 * (r2 + 2 * y * y) + 2 * p.p2 * x * y + p.asym[1] * r2;
    source[0] = p.center[0] + qx / p.anaSq / p.scale;
    source[1] = p.center[1] + qy / p.scale;
    return valid;
}

// The warp's reach along the image border (its extremes for radial warps).
float LensDistortion::Overscan(const Params &p, float aspect)
{
    constexpr int kSteps = 64;
    constexpr float kMax = 2.0f;
    float reach = 1.0f;
    for (int i = 0; i <= kSteps; ++i) {
        const float t = 2.0f * float(i) / kSteps - 1.0f;
        const float border[4][2] = {{-1, t / aspect}, {1, t / aspect}, {t, -1 / aspect}, {t, 1 / aspect}};
        for (const auto &s : border) {
            float source[2];
            if (!Warp(p, aspect, s, source)) {
                return kMax;
            }
            reach = std::max({reach, std::abs(source[0]), std::abs(source[1]) * aspect});
        }
    }
    return std::min(reach, kMax);
}

LensDistortion::LensDistortion(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(d, &sci, nullptr, &_linear), "vkCreateSampler");
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    Check(vkCreateSampler(d, &sci, nullptr, &_nearest), "vkCreateSampler");

    // The copies of colour, depth, primId and instanceId.
    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    }
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 4;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &size;
    Check(vkCreateDescriptorPool(d, &pci, nullptr, &_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = _pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &_setLayout;
    Check(vkAllocateDescriptorSets(d, &ai, &_set), "vkAllocateDescriptorSets");

    // Fullscreen triangle into colour, primId, instanceId and depth (the
    // fragment's own, depth test always passes).
    VkShaderModule vert = CreateShaderModule(d, kFullscreenVert, sizeof(kFullscreenVert));
    VkShaderModule frag = CreateShaderModule(d, kDistortionFrag, sizeof(kDistortionFrag));
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main"},
    };
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
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
    const VkFormat formats[3] = {kColorFormat, kIdFormat, kIdFormat};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 3;
    rendering.pColorAttachmentFormats = formats;
    rendering.depthAttachmentFormat = kDepthFormat;
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
    ci.layout = _layout;
    const VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &_pipeline);
    vkDestroyShaderModule(d, vert, nullptr);
    vkDestroyShaderModule(d, frag, nullptr);
    Check(r, "vkCreateGraphicsPipelines");
}

LensDistortion::~LensDistortion()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    for (Image *img : {&_color, &_depth, &_primId, &_instanceId}) {
        _dev.Destroy(*img);
    }
    vkDestroyPipeline(d, _pipeline, nullptr);
    vkDestroyDescriptorPool(d, _pool, nullptr);
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroySampler(d, _linear, nullptr);
    vkDestroySampler(d, _nearest, nullptr);
}

// The GPU is idle while Render records, so the copies can be remade here.
void LensDistortion::_Resize(uint32_t width, uint32_t height)
{
    for (Image *img : {&_color, &_depth, &_primId, &_instanceId}) {
        _dev.Destroy(*img);
    }
    _width = width;
    _height = height;
    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    _color = _dev.CreateImage(kColorFormat, width, height, 1, 1, kUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_VIEW_TYPE_2D);
    _depth = _dev.CreateImage(kDepthFormat, width, height, 1, 1, kUsage, VK_IMAGE_ASPECT_DEPTH_BIT,
                              VK_IMAGE_VIEW_TYPE_2D);
    _primId = _dev.CreateImage(kIdFormat, width, height, 1, 1, kUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                               VK_IMAGE_VIEW_TYPE_2D);
    _instanceId = _dev.CreateImage(kIdFormat, width, height, 1, 1, kUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                                   VK_IMAGE_VIEW_TYPE_2D);
    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo images[4] = {
        {_linear, _color.view, ro}, {_nearest, _depth.view, ro}, {_nearest, _primId.view, ro},
        {_nearest, _instanceId.view, ro}};
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = _set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(_dev.device, 4, writes, 0, nullptr);
}

void LensDistortion::Record(VkCommandBuffer cmd, const Params &p, uint32_t width, uint32_t height, VkImage color,
                            VkImageView colorView, VkImage depth, VkImageView depthView, VkImage primId,
                            VkImageView primIdView, VkImage instanceId, VkImageView instanceIdView,
                            const float clearColor[4], float clearDepth)
{
    if (width != _width || height != _height) {
        _Resize(width, height);
    }
    // Copy the frame aside, then warp the copies back into it.
    struct Target {
        VkImage image, copy;
        VkImageAspectFlags aspect;
        VkPipelineStageFlags2 stage;
        VkAccessFlags2 access;
        VkImageLayout layout;
    };
    constexpr VkPipelineStageFlags2 kColorStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkPipelineStageFlags2 kDepthStage =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const Target targets[4] = {
        {color, _color.image, VK_IMAGE_ASPECT_COLOR_BIT, kColorStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {depth, _depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, kDepthStage,
         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL},
        {primId, _primId.image, VK_IMAGE_ASPECT_COLOR_BIT, kColorStage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {instanceId, _instanceId.image, VK_IMAGE_ASPECT_COLOR_BIT, kColorStage,
         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    };
    for (const Target &t : targets) {
        ImageBarrier(cmd, t.copy, Range(t.aspect), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {t.aspect, 0, 0, 1};
        copy.extent = {width, height, 1};
        vkCmdCopyImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.copy,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ImageBarrier(cmd, t.copy, Range(t.aspect), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ImageBarrier(cmd, t.image, Range(t.aspect), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     t.stage, t.access, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.layout);
    }

    VkRenderingAttachmentInfo colors[3] = {};
    const VkImageView views[3] = {colorView, primIdView, instanceIdView};
    for (int i = 0; i < 3; ++i) {
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = views[i];
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depthView;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    const Push push{{p.k1, p.k2, p.anaSq, p.scale},
                    {p.center[0], p.center[1], p.asym[0], p.asym[1]},
                    {p.tanHalfWidth, p.overscan, float(width) / float(height), p.fisheye ? 1.0f : 0.0f},
                    {clearColor[0], clearColor[1], clearColor[2], clearColor[3]},
                    {clearDepth, p.p1, p.p2, 0},
                    {p.k3, p.k4, p.k5, p.k6}};
    vkCmdPushConstants(cmd, _layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    for (const Target &t : targets) {
        ImageBarrier(cmd, t.image, Range(t.aspect), t.stage, t.access, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT, t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
}

} // namespace vkr
