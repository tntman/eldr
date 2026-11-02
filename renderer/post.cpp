#include "post.h"

#include "fullscreen.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vkr {

namespace {

const uint32_t kBloomDownFrag[] =
#include "bloom_down.frag.inc"
;
const uint32_t kBloomUpFrag[] =
#include "bloom_up.frag.inc"
;
const uint32_t kTonemapFrag[] =
#include "tonemap.frag.inc"
;
const uint32_t kHistogramComp[] =
#include "histogram.comp.inc"
;
const uint32_t kExposureComp[] =
#include "exposure.comp.inc"
;

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
constexpr float kMinLogLum = -10.0f, kLogLumRange = 20.0f;  // EV -10 .. +10
constexpr uint32_t kMaxBloomLevels = 6;

struct ExposureState {  // mirror of Exposure in post.glsl
    float ev, targetEv, avgLogLum, pad;
};

struct Params {  // mirror of PostParams in post.glsl
    float a[4], b[4], c[4];
};

void BufferBarrier(VkCommandBuffer cmd, VkBuffer buffer,
                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkImageSubresourceRange Mip(uint32_t level, uint32_t count = 1)
{
    return {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
}

} // namespace

PostProcess::PostProcess(Device &device)
    : _dev(device)
{
    const VkDevice d = _dev.device;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(d, &sci, nullptr, &_linear), "vkCreateSampler");

    const VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kStages},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kStages},
    };
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = 4;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(d, &slci, nullptr, &_setLayout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange range{kStages, 0, sizeof(Params)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(d, &lci, nullptr, &_layout), "vkCreatePipelineLayout");

    // Device-local: every histogram workgroup adds to it atomically (in
    // host memory that crossed PCIe, ~5 ms at 720p).
    _histogramBuffer = _dev.CreateDeviceBuffer(256 * sizeof(uint32_t),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    _dev.Reserve(_exposureBuffer, sizeof(ExposureState), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(_exposureBuffer.mapped, 0, sizeof(ExposureState));

    auto compute = [&](const uint32_t *code, size_t bytes) {
        VkShaderModule m = CreateShaderModule(d, code, bytes);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
        ci.layout = _layout;
        VkPipeline p = VK_NULL_HANDLE;
        const VkResult r = vkCreateComputePipelines(d, _dev.pipelineCache, 1, &ci, nullptr, &p);
        vkDestroyShaderModule(d, m, nullptr);
        Check(r, "vkCreateComputePipelines");
        return p;
    };
    _histogram = compute(kHistogramComp, sizeof(kHistogramComp));
    _exposure = compute(kExposureComp, sizeof(kExposureComp));

    _down = CreateFullscreenPipeline(_dev, _layout, kBloomDownFrag, sizeof(kBloomDownFrag), kFormat);
    _up = CreateFullscreenPipeline(_dev, _layout, kBloomUpFrag, sizeof(kBloomUpFrag), kFormat, true);
    _tonemap = CreateFullscreenPipeline(_dev, _layout, kTonemapFrag, sizeof(kTonemapFrag), kFormat);
}

PostProcess::~PostProcess()
{
    const VkDevice d = _dev.device;
    vkDeviceWaitIdle(d);
    _DestroyTargets();
    for (VkPipeline p : {_histogram, _exposure, _down, _up, _tonemap}) {
        vkDestroyPipeline(d, p, nullptr);
    }
    vkDestroyPipelineLayout(d, _layout, nullptr);
    vkDestroyDescriptorSetLayout(d, _setLayout, nullptr);
    vkDestroySampler(d, _linear, nullptr);
    _dev.Destroy(_histogramBuffer);
    _dev.Destroy(_exposureBuffer);
}

void PostProcess::_DestroyTargets()
{
    const VkDevice d = _dev.device;
    vkDestroyDescriptorPool(d, _pool, nullptr);  // frees the sets too
    _pool = VK_NULL_HANDLE;
    _mipSets.clear();
    for (VkImageView v : _bloomMips) {
        vkDestroyImageView(d, v, nullptr);
    }
    _bloomMips.clear();
    _dev.Destroy(_bloom);
    _dev.Destroy(_output);
    _dev.Destroy(_exr);
}

VkDescriptorSet PostProcess::_CreateSet(VkImageView src)
{
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = _pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &_setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Check(vkAllocateDescriptorSets(_dev.device, &ai, &set), "vkAllocateDescriptorSets");

    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkDescriptorImageInfo images[2] = {{_linear, src, ro}, {_linear, _bloomMips[0], ro}};
    const VkDescriptorBufferInfo buffers[2] = {
        {_histogramBuffer.buffer, 0, VK_WHOLE_SIZE}, {_exposureBuffer.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i < 2) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        } else {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i - 2];
        }
    }
    vkUpdateDescriptorSets(_dev.device, 4, writes, 0, nullptr);
    return set;
}

void PostProcess::Resize(uint32_t width, uint32_t height, VkImageView hdr)
{
    _DestroyTargets();
    _width = width;
    _height = height;
    _hdr = hdr;

    // Bloom chain from half resolution down to roughly 8 pixels.
    const uint32_t bw = std::max(1u, width / 2), bh = std::max(1u, height / 2);
    uint32_t levels = 1;
    while (levels < kMaxBloomLevels && (std::min(bw, bh) >> levels) >= 8) {
        ++levels;
    }
    _bloom = _dev.CreateImage(kFormat, bw, bh, 1, levels,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    for (uint32_t i = 0; i < levels; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = _bloom.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kFormat;
        vci.subresourceRange = Mip(i);
        VkImageView v = VK_NULL_HANDLE;
        Check(vkCreateImageView(_dev.device, &vci, nullptr, &v), "vkCreateImageView");
        _bloomMips.push_back(v);
    }
    _output = _dev.CreateImage(kFormat, width, height, 1, 1,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);

    const uint32_t sets = levels + 1;
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * sets},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * sets}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = sets;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(_dev.device, &pci, nullptr, &_pool), "vkCreateDescriptorPool");
    _hdrSet = _CreateSet(hdr);
    for (VkImageView mip : _bloomMips) {
        _mipSets.push_back(_CreateSet(mip));
    }
}

void PostProcess::Record(VkCommandBuffer cmd, const Settings &s, float dt, bool resetExposure,
                         const float background[4], bool linear)
{
    auto fullscreen = [&](VkPipeline pipeline, VkDescriptorSet set, VkImageView target,
                          uint32_t width, uint32_t height, bool load, const Params &params) {
        DrawFullscreen(cmd, pipeline, _layout, set, target, width, height, load, kStages,
                       &params, sizeof(params));
    };

    // Histogram, then exposure (one invocation; it also does the adaptation).
    vkCmdFillBuffer(cmd, _histogramBuffer.buffer, 0, VK_WHOLE_SIZE, 0);
    BufferBarrier(cmd, _histogramBuffer.buffer,
                  VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_hdrSet, 0, nullptr);
    const Params histogramParams{{kMinLogLum, kLogLumRange, 0, 0}, {float(_width), float(_height), 0, 0}, {}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _histogram);
    vkCmdPushConstants(cmd, _layout, kStages, 0, sizeof(Params), &histogramParams);
    vkCmdDispatch(cmd, (_width + 15) / 16, (_height + 15) / 16, 1);
    BufferBarrier(cmd, _histogramBuffer.buffer,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    const Params exposureParams{{dt, s.exposureCompensation, s.autoExposure ? 1.0f : 0.0f,
                                 resetExposure ? 1.0f : 0.0f},
                                {kMinLogLum, kLogLumRange, 0, 0}, {}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _exposure);
    vkCmdPushConstants(cmd, _layout, kStages, 0, sizeof(Params), &exposureParams);
    vkCmdDispatch(cmd, 1, 1, 1);
    BufferBarrier(cmd, _exposureBuffer.buffer,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);

    // Bloom: downsample the HDR scene into the mip chain, then tent-upsample
    // back up, adding each level onto the next larger one.
    const uint32_t levels = uint32_t(_bloomMips.size());
    const bool bloom = s.bloomIntensity > 0.0f;
    auto mipSize = [&](uint32_t i, uint32_t *w, uint32_t *h) {
        *w = std::max(1u, std::max(1u, _width / 2) >> i);
        *h = std::max(1u, std::max(1u, _height / 2) >> i);
    };
    ImageBarrier(cmd, _bloom.image, Mip(0, levels), VK_PIPELINE_STAGE_2_NONE, 0,
                 bloom ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 bloom ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 bloom ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (bloom) {
        for (uint32_t i = 0; i < levels; ++i) {
            uint32_t w, h, sw = _width, sh = _height;
            mipSize(i, &w, &h);
            if (i > 0) {
                mipSize(i - 1, &sw, &sh);
            }
            const Params params{{1.0f / sw, 1.0f / sh, i == 0 ? 1.0f : 0.0f, 0},
                                {1.0f / w, 1.0f / h, 0, 0}, {}};
            fullscreen(_down, i == 0 ? _hdrSet : _mipSets[i - 1], _bloomMips[i], w, h, false, params);
            ImageBarrier(cmd, _bloom.image, Mip(i),
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        for (uint32_t i = levels - 1; i-- > 0;) {
            uint32_t w, h, sw, sh;
            mipSize(i, &w, &h);
            mipSize(i + 1, &sw, &sh);
            ImageBarrier(cmd, _bloom.image, Mip(i),
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            const Params params{{1.0f / sw, 1.0f / sh, 1.0f, 0}, {1.0f / w, 1.0f / h, 0, 0}, {}};
            fullscreen(_up, _mipSets[i + 1], _bloomMips[i], w, h, true, params);
            ImageBarrier(cmd, _bloom.image, Mip(i),
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    // Tonemap into the output.
    ImageBarrier(cmd, _output.image, Mip(0), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    Params tonemapParams{{s.bloomIntensity, float(levels), float(s.tonemapper), 0},
                         {1.0f / _width, 1.0f / _height, 0, 0}, {}};
    std::copy(background, background + 4, tonemapParams.c);
    fullscreen(_tonemap, _hdrSet, _output.view, _width, _height, false, tonemapParams);
    ImageBarrier(cmd, _output.image, Mip(0),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (linear) {
        if (!_exr.image) {
            _exr = _dev.CreateImage(kFormat, _width, _height, 1, 1,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
        }
        ImageBarrier(cmd, _exr.image, Mip(0), VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // Exposure only: tonemapper None, no bloom.
        tonemapParams.a[0] = 0.0f;
        tonemapParams.a[2] = float(Settings::Tonemapper::None);
        fullscreen(_tonemap, _hdrSet, _exr.view, _width, _height, false, tonemapParams);
        ImageBarrier(cmd, _exr.image, Mip(0),
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
}

void PostProcess::Apply(Device &dev, const Settings &s, const float background[4], const void *hdr, uint32_t width,
                        uint32_t height, void *color, void *linear)
{
    PostProcess post(dev);
    Image source = dev.CreateImage(kFormat, width, height, 1, 1,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    post.Resize(width, height, source.view);
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 8;
    Buffer staging;  // the image in, then each output out
    dev.Reserve(staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    std::memcpy(staging.mapped, hdr, bytes);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    auto readBack = [&](VkCommandBuffer cmd, VkImage image) {
        BufferBarrier(cmd, staging.buffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
        BufferBarrier(cmd, staging.buffer, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    };
    dev.Submit([&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, source.image, Mip(0), VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging.buffer, source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ImageBarrier(cmd, source.image, Mip(0), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        post.Record(cmd, s, 0.0f, true, background, linear != nullptr);
        if (color) {
            readBack(cmd, post.Output());
        }
    });
    if (color) {
        std::memcpy(color, staging.mapped, bytes);
    }
    if (linear) {
        dev.Submit([&](VkCommandBuffer cmd) { readBack(cmd, post.LinearOutput()); });
        std::memcpy(linear, staging.mapped, bytes);
    }
    dev.Destroy(staging);
    dev.Destroy(source);
}

float PostProcess::Exposure() const
{
    return std::exp2(static_cast<const ExposureState *>(_exposureBuffer.mapped)->ev);
}

bool PostProcess::ExposureConverged() const
{
    const auto *state = static_cast<const ExposureState *>(_exposureBuffer.mapped);
    return std::abs(state->targetEv - state->ev) < 0.02f;
}

} // namespace vkr
