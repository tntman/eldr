#include "lens.h"

#include "fullscreen.h"

#include <algorithm>

namespace vkr {

namespace {

const uint32_t kTilesComp[] =
#include "lens_tiles.comp.inc"
;
const uint32_t kDofFrag[] =
#include "dof.frag.inc"
;
const uint32_t kMotionBlurFrag[] =
#include "motion_blur.frag.inc"
;
const uint32_t kMotionResidualFrag[] =
#include "motion_residual.frag.inc"
;

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr uint32_t kTile = 32;  // pixels per tile side, as in lens.glsl
constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

VkImageSubresourceRange Color()
{
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

} // namespace

Lens::Lens(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(d, &sci, nullptr, &_nearest), "vkCreateSampler");
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    Check(vkCreateSampler(d, &sci, nullptr, &_linear), "vkCreateSampler");

    // 0 colour (linear), 1 depth, 2 motion vectors, 3 tiles; 4 the tiles
    // as the tile pass writes them.
    VkDescriptorSetLayoutBinding bindings[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
        bindings[i] = {i, i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       kStages};
    }
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 5;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{kStages, 0, sizeof(Params)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");

    VkShaderModule m = CreateShaderModule(d, kTilesComp, sizeof(kTilesComp));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m,
                "main"};
    ci.layout = _layout;
    const VkResult r = vkCreateComputePipelines(d, _dev.pipelineCache, 1, &ci, nullptr, &_tilesPipeline);
    vkDestroyShaderModule(d, m, nullptr);
    Check(r, "vkCreateComputePipelines");
    _dof = CreateFullscreenPipeline(_dev, _layout, kDofFrag, sizeof(kDofFrag), kFormat);
    _blur = CreateFullscreenPipeline(_dev, _layout, kMotionBlurFrag, sizeof(kMotionBlurFrag), kFormat);
    _residual = CreateFullscreenPipeline(_dev, _layout, kMotionResidualFrag, sizeof(kMotionResidualFrag), kFormat);
}

Lens::~Lens()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _Destroy();
    for (VkPipeline p : {_tilesPipeline, _dof, _blur, _residual}) {
        vkDestroyPipeline(d, p, nullptr);
    }
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroySampler(d, _nearest, nullptr);
    vkDestroySampler(d, _linear, nullptr);
}

void Lens::DropScratch()
{
    _dev.Destroy(_scratch);
}

void Lens::_Destroy()
{
    vkDestroyDescriptorPool(_dev.device, _pool, nullptr);
    _pool = VK_NULL_HANDLE;
    _dev.Destroy(_scratch);
    _dev.Destroy(_tiles);
}

void Lens::Resize(uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight, VkImage color,
                  VkImageView colorView, VkImageView depth, VkImageView velocity, VkImage resolved,
                  VkImageView resolvedView, VkImageView history)
{
    _Destroy();
    _width = width;
    _height = height;
    _outWidth = outWidth;
    _outHeight = outHeight;
    _color = color;
    _colorView = colorView;
    _resolved = resolved;
    _resolvedView = resolvedView;
    _tiles = _dev.CreateImage(kFormat, (width + kTile - 1) / kTile, (height + kTile - 1) / kTile, 1, 1,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _dev.Submit([&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, _tiles.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    });

    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(_dev.device, &pci, nullptr, &_pool), "vkCreateDescriptorPool");
    const VkDescriptorSetLayout layouts[2] = {_setLayout, _setLayout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = _pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    Check(vkAllocateDescriptorSets(_dev.device, &ai, sets), "vkAllocateDescriptorSets");
    _dofSet = sets[0];   // colour: the copy depth of field reads
    _blurSet = sets[1];  // colour: TAA's history
    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo images[10];
    VkWriteDescriptorSet writes[10] = {};
    for (uint32_t s = 0; s < 2; ++s) {
        images[s * 5 + 0] = {_linear, history, ro};  // depth of field's: its copy once made (_RecordOnColor)
        images[s * 5 + 1] = {_nearest, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        images[s * 5 + 2] = {_nearest, velocity, ro};
        images[s * 5 + 3] = {_nearest, _tiles.view, VK_IMAGE_LAYOUT_GENERAL};
        images[s * 5 + 4] = {VK_NULL_HANDLE, _tiles.view, VK_IMAGE_LAYOUT_GENERAL};
        for (uint32_t b = 0; b < 5; ++b) {
            VkWriteDescriptorSet &w = writes[s * 5 + b];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = sets[s];
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = b == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &images[s * 5 + b];
        }
    }
    vkUpdateDescriptorSets(_dev.device, 10, writes, 0, nullptr);
}

void Lens::_Fill(Params *params) const
{
    params->texel[0] = 1.0f / float(_width);
    params->texel[1] = 1.0f / float(_height);
}

void Lens::RecordTiles(VkCommandBuffer cmd, Params params)
{
    _Fill(&params);
    // Last frame's lens passes read the tiles.
    ImageBarrier(cmd, _tiles.image, Color(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _tilesPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_blurSet, 0, nullptr);
    vkCmdPushConstants(cmd, _layout, kStages, 0, sizeof(params), &params);
    vkCmdDispatch(cmd, (_width + kTile - 1) / kTile, (_height + kTile - 1) / kTile, 1);
    ImageBarrier(cmd, _tiles.image, Color(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
}

void Lens::RecordDepthOfField(VkCommandBuffer cmd, Params params)
{
    _RecordOnColor(cmd, params, _dof);
}

void Lens::RecordResidualMotionBlur(VkCommandBuffer cmd, Params params)
{
    _RecordOnColor(cmd, params, _residual);
}

void Lens::_RecordOnColor(VkCommandBuffer cmd, Params params, VkPipeline pipeline)
{
    _Fill(&params);
    if (!_scratch.image) {  // first use at this size; the set isn't bound yet this frame
        _scratch = _dev.CreateImage(kFormat, _width, _height, 1, 1,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
        const VkDescriptorImageInfo image{_linear, _scratch.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = _dofSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &image;
        vkUpdateDescriptorSets(_dev.device, 1, &w, 0, nullptr);
    }
    // The pass reads a copy of the colour and writes the colour itself.
    ImageBarrier(cmd, _color, Color(),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(cmd, _scratch.image, Color(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {_width, _height, 1};
    vkCmdCopyImage(cmd, _color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _scratch.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ImageBarrier(cmd, _scratch.image, Color(), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, _color, Color(), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    DrawFullscreen(cmd, pipeline, _layout, _dofSet, _colorView, _width, _height, false, kStages, &params,
                   sizeof(params));
}

void Lens::RecordMotionBlur(VkCommandBuffer cmd, Params params)
{
    _Fill(&params);
    // Reads TAA's history (the same image as the resolve) and rewrites the
    // resolve: the next frame's history stays unblurred.
    ImageBarrier(cmd, _resolved, Color(),
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    DrawFullscreen(cmd, _blur, _layout, _blurSet, _resolvedView, _outWidth, _outHeight, false, kStages, &params,
                   sizeof(params));
    ImageBarrier(cmd, _resolved, Color(),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace vkr
