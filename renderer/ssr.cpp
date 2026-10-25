#include "ssr.h"

#include "fullscreen.h"

#include <algorithm>

namespace vkr {

namespace {

const uint32_t kSpatialFrag[] =
#include "ssr_denoise0.frag.inc"
;
const uint32_t kTemporalFrag[] =
#include "ssr_denoise1.frag.inc"
;
const uint32_t kCompositeFrag[] =
#include "ssr_denoise2.frag.inc"
;

constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // Renderer::kColorFormat
constexpr uint32_t kBindings = 9;

struct DenoiseParams {  // mirror of the push block in ssr_denoise.frag
    float invViewProj[16];
    float prevViewProj[16];
    float eye[4];     // camera position; w: noise index
    float params[4];  // kDimSsrDenoise's value (< 0: none), history valid, 1/width, 1/height
};

VkImageSubresourceRange Color()
{
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

} // namespace

SsrDenoise::SsrDenoise(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(d, &sci, nullptr, &_nearest), "vkCreateSampler");
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    Check(vkCreateSampler(d, &sci, nullptr, &_linear), "vkCreateSampler");

    // 0: depth, 1..4: mesh.frag's rays, directions, normals, weights,
    // 5: the spatial pass's result, 6: last frame's history (linear),
    // 7: this frame's, 8: the blue noise (SetBlueNoise).
    VkDescriptorSetLayoutBinding bindings[kBindings] = {};
    for (uint32_t i = 0; i < kBindings; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    }
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = kBindings;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DenoiseParams)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    _spatial = CreateFullscreenPipeline(_dev, _layout, kSpatialFrag, sizeof(kSpatialFrag), kFormat);
    _temporal = CreateFullscreenPipeline(_dev, _layout, kTemporalFrag, sizeof(kTemporalFrag), kFormat);
    _composite = CreateFullscreenPipeline(_dev, _layout, kCompositeFrag, sizeof(kCompositeFrag), kColorFormat, true);
}

SsrDenoise::~SsrDenoise()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _Destroy();
    for (VkPipeline p : {_spatial, _temporal, _composite}) {
        vkDestroyPipeline(d, p, nullptr);
    }
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroySampler(d, _nearest, nullptr);
    vkDestroySampler(d, _linear, nullptr);
}

void SsrDenoise::_Destroy()
{
    vkDestroyDescriptorPool(_dev.device, _pool, nullptr);
    _pool = VK_NULL_HANDLE;
    for (Image *img : {&_rays, &_directions, &_normals, &_factor, &_resolved, &_history[0], &_history[1]}) {
        _dev.Destroy(*img);
    }
}

void SsrDenoise::Resize(uint32_t width, uint32_t height, VkImageView depth)
{
    _Destroy();
    _width = width;
    _height = height;
    _depth = depth;
}

void SsrDenoise::Prepare()
{
    if (_rays.image) {
        return;
    }
    const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags target = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (Image *img : {&_rays, &_directions, &_normals, &_factor}) {
        *img = _dev.CreateImage(kFormat, _width, _height, 1, 1, storage, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_VIEW_TYPE_2D);
    }
    for (Image *img : {&_resolved, &_history[0], &_history[1]}) {
        *img = _dev.CreateImage(kFormat, _width, _height, 1, 1, target, VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_VIEW_TYPE_2D);
    }
    _dev.Submit([&](VkCommandBuffer cmd) {
        for (Image *img : {&_rays, &_directions, &_normals, &_factor}) {
            ImageBarrier(cmd, img->image, Color(), VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL);
        }
        // The histories are read before their first write.
        for (Image *img : {&_history[0], &_history[1]}) {
            ImageBarrier(cmd, img->image, Color(), VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kBindings};
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
    for (uint32_t i = 0; i < 2; ++i) {
        const VkImageLayout general = VK_IMAGE_LAYOUT_GENERAL, read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkDescriptorImageInfo images[kBindings] = {
            {_nearest, _depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
            {_nearest, _rays.view, general},
            {_nearest, _directions.view, general},
            {_nearest, _normals.view, general},
            {_nearest, _factor.view, general},
            {_nearest, _resolved.view, read},
            {_linear, _history[i ^ 1].view, read},
            {_nearest, _history[i].view, read},
            {_nearest, _blueNoise, read}};
        VkWriteDescriptorSet writes[kBindings] = {};
        for (uint32_t b = 0; b < kBindings; ++b) {
            writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[b].dstSet = _sets[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].pImageInfo = &images[b];
        }
        vkUpdateDescriptorSets(_dev.device, kBindings, writes, 0, nullptr);
    }
}

void SsrDenoise::Clear(VkCommandBuffer cmd)
{
    // Last frame's passes read it; this frame's shading writes it.
    ImageBarrier(cmd, _factor.image, Color(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range = Color();
    vkCmdClearColorImage(cmd, _factor.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    ImageBarrier(cmd, _factor.image, Color(), VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_GENERAL);
    for (const Image *img : {&_rays, &_directions, &_normals}) {
        ImageBarrier(cmd, img->image, Color(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    }
}

void SsrDenoise::Resolve(VkCommandBuffer cmd, VkImageView color, const float invViewProj[16],
                         const float prevViewProj[16], const float eye[3], uint32_t noise, bool history,
                         float dimension)
{
    for (const Image *img : {&_rays, &_directions, &_normals, &_factor}) {
        ImageBarrier(cmd, img->image, Color(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    }
    _current ^= 1u;
    const Image &cur = _history[_current];
    const VkDescriptorSet set = _sets[_current];
    DenoiseParams params{};
    std::copy(invViewProj, invViewProj + 16, params.invViewProj);
    std::copy(prevViewProj, prevViewProj + 16, params.prevViewProj);
    std::copy(eye, eye + 3, params.eye);
    params.eye[3] = float(noise % 1024u);
    params.params[0] = dimension;
    params.params[1] = history ? 1.0f : 0.0f;
    params.params[2] = 1.0f / float(_width);
    params.params[3] = 1.0f / float(_height);

    auto toTarget = [&](const Image &img) {
        ImageBarrier(cmd, img.image, Color(), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    };
    auto toRead = [&](const Image &img) {
        ImageBarrier(cmd, img.image, Color(), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    };
    toTarget(_resolved);
    DrawFullscreen(cmd, _spatial, _layout, set, _resolved.view, _width, _height, false, VK_SHADER_STAGE_FRAGMENT_BIT,
                   &params, sizeof(params));
    toRead(_resolved);
    toTarget(cur);
    DrawFullscreen(cmd, _temporal, _layout, set, cur.view, _width, _height, false, VK_SHADER_STAGE_FRAGMENT_BIT,
                   &params, sizeof(params));
    toRead(cur);
    DrawFullscreen(cmd, _composite, _layout, set, color, _width, _height, true, VK_SHADER_STAGE_FRAGMENT_BIT, &params,
                   sizeof(params));
}

} // namespace vkr
