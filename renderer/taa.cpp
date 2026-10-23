#include "taa.h"

#include "fullscreen.h"

#include <algorithm>

namespace vkr {

namespace {

const uint32_t kTaaFrag[] =
#include "taa.frag.inc"
;

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct TaaParams {  // mirror of the push block in taa.frag
    float reprojection[16];
    float params[4];
};

VkImageSubresourceRange Color()
{
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

} // namespace

TemporalAA::TemporalAA(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(d, &sci, nullptr, &_nearest), "vkCreateSampler");
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    Check(vkCreateSampler(d, &sci, nullptr, &_linear), "vkCreateSampler");

    const VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 4;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TaaParams)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    _pipeline = CreateFullscreenPipeline(d, _layout, kTaaFrag, sizeof(kTaaFrag), kFormat);
}

TemporalAA::~TemporalAA()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _Destroy();
    vkDestroyPipeline(d, _pipeline, nullptr);
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroySampler(d, _nearest, nullptr);
    vkDestroySampler(d, _linear, nullptr);
}

void TemporalAA::_Destroy()
{
    vkDestroyDescriptorPool(_dev.device, _pool, nullptr);
    _pool = VK_NULL_HANDLE;
    _dev.Destroy(_resolved);
    _dev.Destroy(_history);
}

void TemporalAA::Resize(uint32_t width, uint32_t height, VkImageView color, VkImageView depth,
                        VkImageView velocity)
{
    _Destroy();
    _width = width;
    _height = height;
    _resolved = _dev.CreateImage(kFormat, width, height, 1, 1,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _history = _dev.CreateImage(kFormat, width, height, 1, 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    // The resolve shader always binds the history, so give it a defined layout.
    _dev.Submit([&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, _history.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range = Color();
        vkCmdClearColorImage(cmd, _history.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        ImageBarrier(cmd, _history.image, Color(),
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &size;
    Check(vkCreateDescriptorPool(_dev.device, &pci, nullptr, &_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = _pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &_setLayout;
    Check(vkAllocateDescriptorSets(_dev.device, &ai, &_set), "vkAllocateDescriptorSets");
    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo images[4] = {
        {_nearest, color, ro}, {_linear, _history.view, ro},
        {_nearest, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}, {_nearest, velocity, ro}};
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(_dev.device, 4, writes, 0, nullptr);
}

void TemporalAA::Record(VkCommandBuffer cmd, const float reprojection[16], float alpha)
{
    TaaParams params{};
    std::copy(reprojection, reprojection + 16, params.reprojection);
    params.params[0] = alpha;
    params.params[1] = 1.0f / _width;
    params.params[2] = 1.0f / _height;

    ImageBarrier(cmd, _resolved.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    DrawFullscreen(cmd, _pipeline, _layout, _set, _resolved.view, _width, _height, false,
                   VK_SHADER_STAGE_FRAGMENT_BIT, &params, sizeof(params));

    // The resolve becomes next frame's history.
    ImageBarrier(cmd, _resolved.image, Color(),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(cmd, _history.image, Color(),
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {_width, _height, 1};
    vkCmdCopyImage(cmd, _resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   _history.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ImageBarrier(cmd, _history.image, Color(),
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, _resolved.image, Color(),
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace vkr
