#include "gtao.h"

#include "fullscreen.h"

#include <algorithm>

namespace vkr {

namespace {

const uint32_t kGtaoFrag[] =
#include "gtao.frag.inc"
;
const uint32_t kDenoiseFrag[] =
#include "gtao_denoise.frag.inc"
;

constexpr VkFormat kRawFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kAoFormat = VK_FORMAT_R8_UNORM;

struct GtaoParams {  // mirror of the push block in gtao.frag
    float invViewProj[16];
    float eye[4];
    float params[4];
};

VkImageSubresourceRange Color()
{
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

} // namespace

Gtao::Gtao(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(d, &sci, nullptr, &_sampler), "vkCreateSampler");

    // Binding 0: scene depth (gtao.frag), 1: raw AO (gtao_denoise.frag).
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 2;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GtaoParams)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    _gtaoPipeline = CreateFullscreenPipeline(d, _layout, kGtaoFrag, sizeof(kGtaoFrag), kRawFormat);
    _denoisePipeline = CreateFullscreenPipeline(d, _layout, kDenoiseFrag, sizeof(kDenoiseFrag), kAoFormat);
}

Gtao::~Gtao()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _Destroy();
    vkDestroyPipeline(d, _gtaoPipeline, nullptr);
    vkDestroyPipeline(d, _denoisePipeline, nullptr);
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroySampler(d, _sampler, nullptr);
}

void Gtao::_Destroy()
{
    vkDestroyDescriptorPool(_dev.device, _pool, nullptr);
    _pool = VK_NULL_HANDLE;
    _dev.Destroy(_raw);
    _dev.Destroy(_ao);
}

void Gtao::Resize(uint32_t width, uint32_t height, VkImageView depth)
{
    _Destroy();
    _width = width;
    _height = height;
    _raw = _dev.CreateImage(kRawFormat, width, height, 1, 1,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _ao = _dev.CreateImage(kAoFormat, width, height, 1, 1,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
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
    const VkDescriptorImageInfo images[2] = {
        {_sampler, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {_sampler, _raw.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(_dev.device, 2, writes, 0, nullptr);
}

void Gtao::Record(VkCommandBuffer cmd, const float invViewProj[16], const float eye[3], float radius,
                  uint32_t noise)
{
    GtaoParams params{};
    std::copy(invViewProj, invViewProj + 16, params.invViewProj);
    std::copy(eye, eye + 3, params.eye);
    params.params[0] = radius;
    params.params[1] = float(noise);
    params.params[2] = 1.0f / _width;
    params.params[3] = 1.0f / _height;

    ImageBarrier(cmd, _raw.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    DrawFullscreen(cmd, _gtaoPipeline, _layout, _set, _raw.view, _width, _height, false,
                   VK_SHADER_STAGE_FRAGMENT_BIT, &params, sizeof(params));
    ImageBarrier(cmd, _raw.image, Color(),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    ImageBarrier(cmd, _ao.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    DrawFullscreen(cmd, _denoisePipeline, _layout, _set, _ao.view, _width, _height, false,
                   VK_SHADER_STAGE_FRAGMENT_BIT, &params, sizeof(params));
    ImageBarrier(cmd, _ao.image, Color(),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Gtao::Clear(VkCommandBuffer cmd)
{
    ImageBarrier(cmd, _ao.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    const VkImageSubresourceRange range = Color();
    vkCmdClearColorImage(cmd, _ao.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
    ImageBarrier(cmd, _ao.image, Color(), VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace vkr
