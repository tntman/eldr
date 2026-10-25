#include "subsurface.h"

#include "fullscreen.h"

#include <algorithm>

namespace vkr {

namespace {

const uint32_t kBlurFrag[] =
#include "subsurface_blur.frag.inc"
;

constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // Renderer::kColorFormat

struct BlurParams {  // mirror of the push block in subsurface_blur.frag
    float invViewProj[16];
    float params[4];  // direction (pixels), taps, pass (0: horizontal, 1: vertical into the colour)
};

VkImageSubresourceRange Color()
{
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

} // namespace

Subsurface::Subsurface(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(d, &sci, nullptr, &_sampler), "vkCreateSampler");

    // 0: what the pass blurs (the light, then the horizontal pass's
    // result), 1: the light, 2: ids, 3: depth.
    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    }
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 4;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlurParams)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    _horizontal = CreateFullscreenPipeline(_dev, _layout, kBlurFrag, sizeof(kBlurFrag), kLightFormat);
    _vertical = CreateFullscreenPipeline(_dev, _layout, kBlurFrag, sizeof(kBlurFrag), kColorFormat, true);
}

Subsurface::~Subsurface()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _Destroy();
    vkDestroyPipeline(d, _horizontal, nullptr);
    vkDestroyPipeline(d, _vertical, nullptr);
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroySampler(d, _sampler, nullptr);
}

void Subsurface::_Destroy()
{
    vkDestroyDescriptorPool(_dev.device, _pool, nullptr);
    _pool = VK_NULL_HANDLE;
    _dev.Destroy(_light);
    _dev.Destroy(_id);
    _dev.Destroy(_blurred);
}

void Subsurface::Resize(uint32_t width, uint32_t height, VkImageView depth)
{
    _Destroy();
    _width = width;
    _height = height;
    _depth = depth;
}

void Subsurface::BeginLighting(VkCommandBuffer cmd)
{
    if (!_light.image) {
        constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        _light = _dev.CreateImage(kLightFormat, _width, _height, 1, 1, kUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_VIEW_TYPE_2D);
        _id = _dev.CreateImage(kIdFormat, _width, _height, 1, 1, kUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                               VK_IMAGE_VIEW_TYPE_2D);
        _blurred = _dev.CreateImage(kLightFormat, _width, _height, 1, 1, kUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_IMAGE_VIEW_TYPE_2D);
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 2;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        Check(vkCreateDescriptorPool(_dev.device, &pci, nullptr, &_pool), "vkCreateDescriptorPool");
        const VkDescriptorSetLayout layouts[2] = {_setLayout, _setLayout};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = _pool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        Check(vkAllocateDescriptorSets(_dev.device, &ai, _sets), "vkAllocateDescriptorSets");
        for (uint32_t pass = 0; pass < 2; ++pass) {
            const VkDescriptorImageInfo images[4] = {
                {_sampler, pass == 0 ? _light.view : _blurred.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {_sampler, _light.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {_sampler, _id.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {_sampler, _depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet writes[4] = {};
            for (uint32_t i = 0; i < 4; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = _sets[pass];
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &images[i];
            }
            vkUpdateDescriptorSets(_dev.device, 4, writes, 0, nullptr);
        }
    }
    for (const Image *img : {&_light, &_id}) {
        ImageBarrier(cmd, img->image, Color(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
}

void Subsurface::Blur(VkCommandBuffer cmd, VkImageView color, const float invViewProj[16], uint32_t samples)
{
    for (const Image *img : {&_light, &_id}) {
        ImageBarrier(cmd, img->image, Color(), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    BlurParams params{};
    std::copy(invViewProj, invViewProj + 16, params.invViewProj);
    params.params[2] = float(std::clamp(samples, 1u, 64u));

    ImageBarrier(cmd, _blurred.image, Color(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    params.params[0] = 1.0f;
    DrawFullscreen(cmd, _horizontal, _layout, _sets[0], _blurred.view, _width, _height, false,
                   VK_SHADER_STAGE_FRAGMENT_BIT, &params, sizeof(params));
    ImageBarrier(cmd, _blurred.image, Color(), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    params.params[0] = 0.0f;
    params.params[1] = 1.0f;
    params.params[3] = 1.0f;
    DrawFullscreen(cmd, _vertical, _layout, _sets[1], color, _width, _height, true, VK_SHADER_STAGE_FRAGMENT_BIT,
                   &params, sizeof(params));
}

} // namespace vkr
