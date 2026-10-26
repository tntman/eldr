#include "transparency.h"

#include "fullscreen.h"

#include <algorithm>

namespace vkr {

namespace {

const uint32_t kOitResolveFrag[] =
#include "oit_resolve.frag.inc"
;
const uint32_t kBackgroundUnscaleComp[] =
#include "background_unscale.comp.inc"
;

constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // Renderer::kColorFormat

VkImageSubresourceRange Color()
{
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
}

} // namespace

Transparency::Transparency(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(d, &sci, nullptr, &_sampler), "vkCreateSampler");

    // Binding 0: accumulated colour and coverage, 1: revealage.
    const VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 2;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    _resolve = CreateFullscreenPipeline(_dev, _layout, kOitResolveFrag, sizeof(kOitResolveFrag), kColorFormat,
                                        false, true);
}

Transparency::~Transparency()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _Destroy();
    _DestroyTint(_sunTint);
    _DestroyTint(_atlasTint);
    vkDestroyPipeline(d, _unscale, nullptr);
    vkDestroyPipelineLayout(d, _unscaleLayout, nullptr);
    vkDestroyDescriptorPool(d, _unscalePool, nullptr);
    vkDestroyDescriptorSetLayout(d, _unscaleSetLayout, nullptr);
    vkDestroyPipeline(d, _resolve, nullptr);
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroySampler(d, _sampler, nullptr);
}

void Transparency::_Destroy()
{
    vkDestroyDescriptorPool(_dev.device, _pool, nullptr);
    _pool = VK_NULL_HANDLE;
    _dev.Destroy(_accum);
    _dev.Destroy(_revealage);
    vkDestroyImageView(_dev.device, _backgroundLevel0, nullptr);
    _backgroundLevel0 = VK_NULL_HANDLE;
    _dev.Destroy(_background);
    _backgroundLevels = 0;
}

void Transparency::Resize(uint32_t width, uint32_t height)
{
    _Destroy();
    _width = width;
    _height = height;
    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    _accum = _dev.CreateImage(kAccumFormat, width, height, 1, 1, kUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_VIEW_TYPE_2D);
    _revealage = _dev.CreateImage(kRevealageFormat, width, height, 1, 1, kUsage, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_VIEW_TYPE_2D);

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
        {_sampler, _accum.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {_sampler, _revealage.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
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

VkImageView Transparency::EnsureBackground()
{
    // Called per frame before recording: the unscale's set is rewritten on
    // its first use (a view made since may reuse an old handle).
    _unscaleViews[0] = _unscaleViews[1] = VK_NULL_HANDLE;
    if (!_background.image) {
        uint32_t levels = 1;
        while ((std::max(_width, _height) >> levels) > 0) {
            ++levels;
        }
        _background = _dev.CreateImage(kColorFormat, _width, _height, 1, levels,
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
        _backgroundLevels = levels;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = _background.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kColorFormat;
        vci.subresourceRange = Color();
        Check(vkCreateImageView(_dev.device, &vci, nullptr, &_backgroundLevel0), "vkCreateImageView");
        _dev.Submit([&](VkCommandBuffer cmd) {
            ImageBarrier(cmd, _background.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1},
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
    }
    return _background.view;
}

void Transparency::_Unscale(VkCommandBuffer cmd, VkImageView splits, float direct, float indirect)
{
    const VkDevice d = _dev.device;
    if (!_unscale) {
        const VkDescriptorSetLayoutBinding bindings[2] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        };
        VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        slci.bindingCount = 2;
        slci.pBindings = bindings;
        Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_unscaleSetLayout), "vkCreateDescriptorSetLayout");
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &_unscaleSetLayout;
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &range;
        Check(vkCreatePipelineLayout(d, &lci, nullptr, &_unscaleLayout), "vkCreatePipelineLayout");
        VkShaderModule m = CreateShaderModule(d, kBackgroundUnscaleComp, sizeof(kBackgroundUnscaleComp));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m,
                    "main"};
        ci.layout = _unscaleLayout;
        const VkResult r = vkCreateComputePipelines(d, _dev.pipelineCache, 1, &ci, nullptr, &_unscale);
        vkDestroyShaderModule(d, m, nullptr);
        Check(r, "vkCreateComputePipelines");
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        Check(vkCreateDescriptorPool(d, &pci, nullptr, &_unscalePool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = _unscalePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &_unscaleSetLayout;
        Check(vkAllocateDescriptorSets(d, &ai, &_unscaleSet), "vkAllocateDescriptorSets");
    }
    // Once per frame (EnsureBackground), as Subsurface::Blur updates its
    // sets; not again while this command buffer has it bound (refraction
    // layers copy with the same views).
    if (_unscaleViews[0] != _backgroundLevel0 || _unscaleViews[1] != splits) {
        _unscaleViews[0] = _backgroundLevel0;
        _unscaleViews[1] = splits;
        const VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, _backgroundLevel0, VK_IMAGE_LAYOUT_GENERAL},
                                                 {VK_NULL_HANDLE, splits, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = _unscaleSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(d, 2, writes, 0, nullptr);
    }

    // The shading pass's splits; level 0 from the copy to GENERAL and back.
    VkMemoryBarrier2 stored{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    stored.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    stored.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    stored.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    stored.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &stored;
    vkCmdPipelineBarrier2(cmd, &dep);
    ImageBarrier(cmd, _background.image, Color(), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _unscale);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _unscaleLayout, 0, 1, &_unscaleSet, 0, nullptr);
    const float params[4] = {direct, indirect, 0.0f, 0.0f};
    vkCmdPushConstants(cmd, _unscaleLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), params);
    vkCmdDispatch(cmd, (_width + 7) / 8, (_height + 7) / 8, 1);
    ImageBarrier(cmd, _background.image, Color(), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
}

void Transparency::RecordBackground(VkCommandBuffer cmd, VkImage color, VkImageView splits, float direct,
                                    float indirect)
{
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, _backgroundLevels, 0, 1};
    ImageBarrier(cmd, color, Color(), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(cmd, _background.image, all, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {_width, _height, 1};
    vkCmdCopyImage(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _background.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ImageBarrier(cmd, color, Color(), VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (splits) {
        _Unscale(cmd, splits, direct, indirect);
    }
    // Each mip a linear 2x2 average of the one above (premultiplied, so
    // coverage averages with the colour).
    for (uint32_t level = 1; level < _backgroundLevels; ++level) {
        ImageBarrier(cmd, _background.image, {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1},
                     VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.srcOffsets[1] = {int32_t(std::max(_width >> (level - 1), 1u)),
                              int32_t(std::max(_height >> (level - 1), 1u)), 1};
        blit.dstOffsets[1] = {int32_t(std::max(_width >> level, 1u)), int32_t(std::max(_height >> level, 1u)), 1};
        vkCmdBlitImage(cmd, _background.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _background.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    // All but the last level are TRANSFER_SRC now, the last TRANSFER_DST.
    if (_backgroundLevels > 1) {
        ImageBarrier(cmd, _background.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, _backgroundLevels - 1, 0, 1},
                     VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    ImageBarrier(cmd, _background.image, {VK_IMAGE_ASPECT_COLOR_BIT, _backgroundLevels - 1, 1, 0, 1},
                 VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Transparency::_DestroyTint(TintMap &map)
{
    for (int i = 0; i < 4; ++i) {
        vkDestroyImageView(_dev.device, map.colorLayers[i], nullptr);
        vkDestroyImageView(_dev.device, map.depthLayers[i], nullptr);
        map.colorLayers[i] = map.depthLayers[i] = VK_NULL_HANDLE;
    }
    _dev.Destroy(map.color);
    _dev.Destroy(map.depth);
    map.size = 0;
}

bool Transparency::EnsureTintMaps(uint32_t sunSize, uint32_t cascades, uint32_t atlasSize)
{
    if (_sunTint.size == sunSize && _atlasTint.size == atlasSize && _tintCascades == cascades) {
        return false;
    }
    _tintCascades = cascades;
    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (TintMap *map : {&_sunTint, &_atlasTint}) {
        const bool sun = map == &_sunTint;
        _DestroyTint(*map);
        map->size = sun ? sunSize : atlasSize;
        const uint32_t layers = sun ? cascades : 1;
        const VkImageViewType type = sun ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        map->color = _dev.CreateImage(kTintFormat, map->size, map->size, layers, 1,
                                      kUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, type);
        map->depth = _dev.CreateImage(kTintDepthFormat, map->size, map->size, layers, 1,
                                      kUsage | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                                      type);
        for (uint32_t layer = 0; sun && layer < layers; ++layer) {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.image = map->color.image;
            vci.format = kTintFormat;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
            Check(vkCreateImageView(_dev.device, &vci, nullptr, &map->colorLayers[layer]), "vkCreateImageView");
            vci.image = map->depth.image;
            vci.format = kTintDepthFormat;
            vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
            Check(vkCreateImageView(_dev.device, &vci, nullptr, &map->depthLayers[layer]), "vkCreateImageView");
        }
    }
    _dev.Submit([&](VkCommandBuffer cmd) {
        for (const TintMap *map : {&_sunTint, &_atlasTint}) {
            const uint32_t layers = map == &_sunTint ? cascades : 1;
            _ClearTint(cmd, *map, layers, VK_IMAGE_LAYOUT_UNDEFINED);
            for (const auto &[image, aspect] : {std::pair{map->color.image, VK_IMAGE_ASPECT_COLOR_BIT},
                                                {map->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT}}) {
                ImageBarrier(cmd, image, {VkImageAspectFlags(aspect), 0, 1, 0, layers}, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                             VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    });
    return true;
}

void Transparency::_ClearTint(VkCommandBuffer cmd, const TintMap &map, uint32_t layers, VkImageLayout from)
{
    const VkImageSubresourceRange color{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    const VkImageSubresourceRange depth{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    ImageBarrier(cmd, map.color.image, color, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    ImageBarrier(cmd, map.depth.image, depth, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
    vkCmdClearColorImage(cmd, map.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &color);
    const VkClearDepthStencilValue far{1.0f, 0};
    vkCmdClearDepthStencilImage(cmd, map.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &depth);
}

void Transparency::BeginTint(VkCommandBuffer cmd, const TintMap &map, uint32_t layers)
{
    _ClearTint(cmd, map, layers, VK_IMAGE_LAYOUT_UNDEFINED);
    ImageBarrier(cmd, map.color.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ImageBarrier(cmd, map.depth.image, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers}, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
}

void Transparency::EndTint(VkCommandBuffer cmd, const TintMap &map, uint32_t layers)
{
    ImageBarrier(cmd, map.color.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers},
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,  // and fog
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, map.depth.image, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers},
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Transparency::BeginAccumulation(VkCommandBuffer cmd)
{
    for (const Image *img : {&_accum, &_revealage}) {
        ImageBarrier(cmd, img->image, Color(), VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
}

void Transparency::Resolve(VkCommandBuffer cmd, VkImageView color)
{
    for (const Image *img : {&_accum, &_revealage}) {
        ImageBarrier(cmd, img->image, Color(), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    const float size[4] = {float(_width), float(_height), 0, 0};
    DrawFullscreen(cmd, _resolve, _layout, _set, color, _width, _height, true, VK_SHADER_STAGE_FRAGMENT_BIT,
                   size, sizeof(size));
}

} // namespace vkr
