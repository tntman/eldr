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
const uint32_t kSsgiFrag[] =
#include "ssgi.frag.inc"
;
const uint32_t kSsgiBitmaskFrag[] =
#include "ssgi_bitmask.frag.inc"
;
const uint32_t kSsgiDenoiseFrag[] =
#include "ssgi_denoise.frag.inc"
;

constexpr VkFormat kRawFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr VkFormat kAoFormat = VK_FORMAT_R8_UNORM;
constexpr VkFormat kSsgiFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct GtaoParams {  // mirror of the push block in gtao.frag (the SSGI part only there)
    float invViewProj[16];
    float eye[4];
    float params[4];
    float prevViewProj[16];
    float ssgi[4];      // depth pixels per raw pixel, steps per side, history valid, max radiance
    float temporal[4];  // weight of this frame in SSGI's running average, thickness estimator, object thickness
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

    // Binding 0: scene depth (gtao.frag), 1: raw AO (gtao_denoise.frag), 2:
    // last frame's resolved colour, 3: last frame's SSGI (SSGI; partially
    // bound).
    const VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    const VkDescriptorBindingFlags partial[4] = {0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
                                                 VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 4;
    flags.pBindingFlags = partial;
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags};
    slci.bindingCount = 4;
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
    _ssgiPipeline = CreateFullscreenPipeline(d, _layout, kSsgiFrag, sizeof(kSsgiFrag), kSsgiFormat);
    _ssgiBitmaskPipeline = CreateFullscreenPipeline(d, _layout, kSsgiBitmaskFrag, sizeof(kSsgiBitmaskFrag), kSsgiFormat);
    _ssgiDenoisePipeline = CreateFullscreenPipeline(d, _layout, kSsgiDenoiseFrag, sizeof(kSsgiDenoiseFrag), kSsgiFormat);
}

Gtao::~Gtao()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _Destroy();
    vkDestroyPipeline(d, _gtaoPipeline, nullptr);
    vkDestroyPipeline(d, _denoisePipeline, nullptr);
    vkDestroyPipeline(d, _ssgiPipeline, nullptr);
    vkDestroyPipeline(d, _ssgiBitmaskPipeline, nullptr);
    vkDestroyPipeline(d, _ssgiDenoisePipeline, nullptr);
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
    _dev.Destroy(_ssgiRaw);
    _dev.Destroy(_ssgi[0]);
    _dev.Destroy(_ssgi[1]);
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

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &size;
    Check(vkCreateDescriptorPool(_dev.device, &pci, nullptr, &_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = _pool;
    const VkDescriptorSetLayout layouts[2] = {_setLayout, _setLayout};
    VkDescriptorSet sets[2];
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    Check(vkAllocateDescriptorSets(_dev.device, &ai, sets), "vkAllocateDescriptorSets");
    _set = sets[0];
    _ssgiSet = sets[1];
    _depth = depth;
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
    if (_ssgiOn) {
        _CreateSsgi();
    }
}

void Gtao::SetSsgi(bool on, uint32_t scale)
{
    scale = std::clamp(scale, 1u, 2u);
    _current ^= 1u;
    if (on == _ssgiOn && scale == _scale) {
        return;
    }
    _ssgiOn = on;
    _scale = scale;
    _dev.Destroy(_ssgiRaw);
    _dev.Destroy(_ssgi[0]);
    _dev.Destroy(_ssgi[1]);
    if (on && _pool) {
        _CreateSsgi();
    }
}

void Gtao::_CreateSsgi()
{
    _dev.Destroy(_ssgiRaw);
    _dev.Destroy(_ssgi[0]);
    _dev.Destroy(_ssgi[1]);
    _accumulated = 0;
    _ssgiRaw = _dev.CreateImage(kSsgiFormat, (_width + _scale - 1) / _scale, (_height + _scale - 1) / _scale, 1, 1,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    for (Image &img : _ssgi) {
        img = _dev.CreateImage(kSsgiFormat, _width, _height, 1, 1,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    }
    // Readable from the start (the first frame has no history to blend).
    _dev.Submit([&](VkCommandBuffer cmd) {
        for (const Image &img : _ssgi) {
            ImageBarrier(cmd, img.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkClearColorValue visible{{1.0f, 0.0f, 0.0f, 0.0f}};
            const VkImageSubresourceRange range = Color();
            vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &visible, 1, &range);
            ImageBarrier(cmd, img.image, Color(), VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });
    const VkDescriptorImageInfo images[2] = {
        {_sampler, _depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
        {_sampler, _ssgiRaw.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _ssgiSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(_dev.device, 2, writes, 0, nullptr);
}

// One fullscreen pass into `target`, left shader-readable.
void Gtao::_Draw(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set, const Image &target, uint32_t width,
                 uint32_t height, const void *params)
{
    ImageBarrier(cmd, target.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    DrawFullscreen(cmd, pipeline, _layout, set, target.view, width, height, false, VK_SHADER_STAGE_FRAGMENT_BIT,
                   params, sizeof(GtaoParams));
    ImageBarrier(cmd, target.image, Color(),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Gtao::RecordSsgi(VkCommandBuffer cmd, const float invViewProj[16], const float prevViewProj[16],
                      const float eye[3], float radius, uint32_t noise, uint32_t slices, uint32_t steps,
                      VkImageView history, bool historyValid, float maxRadiance, bool still, bool thickness,
                      float objectThickness)
{
    const VkDescriptorImageInfo images[2] = {{_sampler, history, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {_sampler, _ssgi[_current ^ 1u].view,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _ssgiSet;
        writes[i].dstBinding = 2 + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(_dev.device, 2, writes, 0, nullptr);
    _accumulated = still ? std::min(_accumulated + 1, 16u) : 1;

    GtaoParams params{};
    std::copy(invViewProj, invViewProj + 16, params.invViewProj);
    std::copy(prevViewProj, prevViewProj + 16, params.prevViewProj);
    std::copy(eye, eye + 3, params.eye);
    params.eye[3] = float(slices);
    params.params[0] = radius;
    params.params[1] = float(noise);
    params.params[2] = 1.0f / _width;
    params.params[3] = 1.0f / _height;
    params.ssgi[0] = float(_scale);
    params.ssgi[1] = float(steps);
    params.ssgi[2] = historyValid ? 1.0f : 0.0f;
    params.ssgi[3] = maxRadiance;
    params.temporal[0] = 1.0f / float(_accumulated);
    params.temporal[1] = thickness ? 1.0f : 0.0f;
    params.temporal[2] = objectThickness;
    _Draw(cmd, objectThickness > 0.0f ? _ssgiBitmaskPipeline : _ssgiPipeline, _ssgiSet, _ssgiRaw, (_width + _scale - 1) / _scale, (_height + _scale - 1) / _scale,
          &params);
    _Draw(cmd, _ssgiDenoisePipeline, _ssgiSet, _ssgi[_current], _width, _height, &params);
}

void Gtao::Record(VkCommandBuffer cmd, const float invViewProj[16], const float eye[3], float radius,
                  uint32_t noise, uint32_t slices)
{
    GtaoParams params{};
    std::copy(invViewProj, invViewProj + 16, params.invViewProj);
    std::copy(eye, eye + 3, params.eye);
    params.eye[3] = float(slices);
    params.params[0] = radius;
    params.params[1] = float(noise);
    params.params[2] = 1.0f / _width;
    params.params[3] = 1.0f / _height;

    _Draw(cmd, _gtaoPipeline, _set, _raw, _width, _height, &params);
    _Draw(cmd, _denoisePipeline, _set, _ao, _width, _height, &params);
}

void Gtao::Clear(VkCommandBuffer cmd)
{
    const Image &out = _ssgiOn ? _ssgi[_current] : _ao;  // SSGI: visible, no light gathered
    _accumulated = 0;
    ImageBarrier(cmd, out.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue white{{1.0f, _ssgiOn ? 0.0f : 1.0f, _ssgiOn ? 0.0f : 1.0f, _ssgiOn ? 0.0f : 1.0f}};
    const VkImageSubresourceRange range = Color();
    vkCmdClearColorImage(cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
    ImageBarrier(cmd, out.image, Color(), VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace vkr
