#include "taa.h"

#include "fullscreen.h"

#include <algorithm>
#include <cstddef>

namespace vkr {

namespace {

const uint32_t kTaaFrag[] =
#include "taa.frag.inc"
;
const uint32_t kTaaUpscaleFrag[] =
#include "taa_upscale.frag.inc"
;
const uint32_t kFilmComp[] =
#include "film.comp.inc"
;

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct TaaParams {  // mirror of the push block in taa.frag
    float reprojection[16];
    float params[4];
    float upscale[4];  // UPSCALE only
};

struct FilmParams {  // mirror of the push block in film.comp
    float params[4];
    float seed[4];
    float adaptive[4];
};

constexpr VkFormat kSumFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

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
    _pipeline = CreateFullscreenPipeline(_dev, _layout, kTaaFrag, sizeof(kTaaFrag), kFormat);
    _upscale = CreateFullscreenPipeline(_dev, _layout, kTaaUpscaleFrag, sizeof(kTaaUpscaleFrag), kFormat);

    const VkDescriptorSetLayoutBinding film[6] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
    };
    slci.bindingCount = 6;
    slci.pBindings = film;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_filmSetLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange filmRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FilmParams)};
    lci.pSetLayouts = &_filmSetLayout;
    lci.pPushConstantRanges = &filmRange;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_filmLayout), "vkCreatePipelineLayout");
    VkShaderModule m = CreateShaderModule(d, kFilmComp, sizeof(kFilmComp));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
    ci.layout = _filmLayout;
    const VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &_film);
    vkDestroyShaderModule(d, m, nullptr);
    Check(r, "vkCreateComputePipelines");
    _dev.Reserve(_active, sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    *static_cast<uint32_t *>(_active.mapped) = 0;
}

TemporalAA::~TemporalAA()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _Destroy();
    vkDestroyPipeline(d, _pipeline, nullptr);
    vkDestroyPipeline(d, _upscale, nullptr);
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroyPipeline(d, _film, nullptr);
    vkDestroyPipelineLayout(d, _filmLayout, nullptr);
    vkDestroyDescriptorSetLayout(d, _filmSetLayout, nullptr);
    _dev.Destroy(_active);
    vkDestroySampler(d, _nearest, nullptr);
    vkDestroySampler(d, _linear, nullptr);
}

void TemporalAA::_Destroy()
{
    vkDestroyDescriptorPool(_dev.device, _pool, nullptr);
    _pool = VK_NULL_HANDLE;
    _dev.Destroy(_resolved);
    _dev.Destroy(_history);
    _dev.Destroy(_sums);
    _dev.Destroy(_weights);
}

void TemporalAA::Resize(uint32_t width, uint32_t height)
{
    _Destroy();
    _width = width;
    _height = height;
    _resolved = _dev.CreateImage(kFormat, width, height, 1, 1,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _history = _dev.CreateImage(kFormat, width, height, 1, 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    for (Image *sums : {&_sums, &_weights}) {
        *sums = _dev.CreateImage(kSumFormat, width, height, 1, 1, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_VIEW_TYPE_2D);
    }
    // The resolve shader always binds the history, so give it a defined
    // layout; the film's sums live in GENERAL.
    _dev.Submit([&](VkCommandBuffer cmd) {
        for (const Image *sums : {&_sums, &_weights}) {
            ImageBarrier(cmd, sums->image, Color(), VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        }
        ImageBarrier(cmd, _history.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range = Color();
        vkCmdClearColorImage(cmd, _history.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        ImageBarrier(cmd, _history.image, Color(),
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(_dev.device, &pci, nullptr, &_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = _pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &_setLayout;
    Check(vkAllocateDescriptorSets(_dev.device, &ai, &_set), "vkAllocateDescriptorSets");
    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo history{_linear, _history.view, ro};
    const VkWriteDescriptorSet historyWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _set, 1, 0, 1,
                                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &history};
    vkUpdateDescriptorSets(_dev.device, 1, &historyWrite, 0, nullptr);

    ai.pSetLayouts = &_filmSetLayout;
    Check(vkAllocateDescriptorSets(_dev.device, &ai, &_filmSet), "vkAllocateDescriptorSets");
    const VkImageLayout general = VK_IMAGE_LAYOUT_GENERAL;
    const VkDescriptorImageInfo film[5] = {{}, {_nearest, _history.view, ro},
                                           {VK_NULL_HANDLE, _sums.view, general},
                                           {VK_NULL_HANDLE, _weights.view, general},
                                           {VK_NULL_HANDLE, _resolved.view, general}};
    const VkDescriptorBufferInfo active{_active.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet filmWrites[5] = {};
    filmWrites[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, _filmSet, 5, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &active};
    for (uint32_t i = 1; i < 5; ++i) {
        filmWrites[i - 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        filmWrites[i - 1].dstSet = _filmSet;
        filmWrites[i - 1].dstBinding = i;
        filmWrites[i - 1].descriptorCount = 1;
        filmWrites[i - 1].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                 : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        filmWrites[i - 1].pImageInfo = &film[i];
    }
    vkUpdateDescriptorSets(_dev.device, 5, filmWrites, 0, nullptr);
}

void TemporalAA::SetInput(uint32_t width, uint32_t height, VkImageView color, VkImageView depth,
                          VkImageView velocity)
{
    _inWidth = width;
    _inHeight = height;
    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo images[3] = {
        {_nearest, color, ro}, {_nearest, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}, {_nearest, velocity, ro}};
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = i < 3 ? _set : _filmSet;
        writes[i].dstBinding = i == 0 || i == 3 ? 0 : i + 1;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i == 3 ? 0 : i];
    }
    vkUpdateDescriptorSets(_dev.device, 4, writes, 0, nullptr);
}

void TemporalAA::Record(VkCommandBuffer cmd, const float reprojection[16], float alpha, const float offset[2])
{
    TaaParams params{};
    std::copy(reprojection, reprojection + 16, params.reprojection);
    params.params[0] = alpha;
    params.params[1] = 1.0f / _width;
    params.params[2] = 1.0f / _height;
    const bool upscale = _inWidth != _width || _inHeight != _height;
    if (upscale) {
        params.upscale[0] = offset[0];
        params.upscale[1] = offset[1];
        params.upscale[2] = float(_inWidth) / float(_width);
        params.upscale[3] = float(_inHeight) / float(_height);
    }

    ImageBarrier(cmd, _resolved.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    DrawFullscreen(cmd, upscale ? _upscale : _pipeline, _layout, _set, _resolved.view, _width, _height, false,
                   VK_SHADER_STAGE_FRAGMENT_BIT, &params,
                   upscale ? sizeof(params) : uint32_t(offsetof(TaaParams, upscale)));
    _CopyToHistory(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void TemporalAA::RecordFilm(VkCommandBuffer cmd, const float offset[2], float radius, uint32_t mode, float seed,
                            float threshold, uint32_t minSamples, float exposure)
{
    const FilmParams params{{offset[0], offset[1], radius, float(mode)}, {seed, 0, 0, 0},
                            {threshold, float(minSamples), exposure, 0}};
    // The count of pixels still sampling starts at 0 (last frame's host read is done).
    vkCmdFillBuffer(cmd, _active.buffer, 0, sizeof(uint32_t), 0);
    ImageBarrier(cmd, _resolved.image, Color(), VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    // Last frame's film writes come before this one's reads.
    VkMemoryBarrier2 sums{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    sums.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    sums.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    sums.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    sums.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &sums;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _film);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _filmLayout, 0, 1, &_filmSet, 0, nullptr);
    vkCmdPushConstants(cmd, _filmLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(cmd, (_width + 7) / 8, (_height + 7) / 8, 1);
    VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    dep.pMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(cmd, &dep);
    _CopyToHistory(cmd, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

// The resolve becomes next frame's history; leaves it SHADER_READ_ONLY_OPTIMAL.
void TemporalAA::_CopyToHistory(VkCommandBuffer cmd, VkImageLayout layout, VkPipelineStageFlags2 stage,
                                VkAccessFlags2 access)
{
    ImageBarrier(cmd, _resolved.image, Color(), stage, access, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(cmd, _history.image, Color(),
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {_width, _height, 1};
    vkCmdCopyImage(cmd, _resolved.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   _history.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ImageBarrier(cmd, _history.image, Color(),
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, _resolved.image, Color(),
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace vkr
