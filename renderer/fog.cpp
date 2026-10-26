#include "fog.h"

#include <tuple>

namespace vkr {

namespace {

const uint32_t kInjectComp[] =
#include "fog_inject.comp.inc"
;
const uint32_t kIntegrateComp[] =
#include "fog_integrate.comp.inc"
;

constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

void MemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

Fog::Fog(Device &device)
    : _dev(device)
{
    Resize(1, 1, 1);
}

Fog::~Fog()
{
    vkDeviceWaitIdle(_dev.device);
    _Destroy();
    vkDestroyPipeline(_dev.device, _inject, nullptr);
    vkDestroyPipeline(_dev.device, _integrate, nullptr);
}

void Fog::CreatePipelines(VkPipelineLayout layout)
{
    for (auto [code, bytes, pipeline] : {std::tuple{kInjectComp, sizeof(kInjectComp), &_inject},
                                         std::tuple{kIntegrateComp, sizeof(kIntegrateComp), &_integrate}}) {
        VkShaderModule module = CreateShaderModule(_dev.device, code, bytes);
        VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                     module, "main"};
        cci.layout = layout;
        const VkResult r = vkCreateComputePipelines(_dev.device, _dev.pipelineCache, 1, &cci, nullptr, pipeline);
        vkDestroyShaderModule(_dev.device, module, nullptr);
        Check(r, "vkCreateComputePipelines");
    }
}

void Fog::_Destroy()
{
    for (Image *img : {&_injected[0], &_injected[1], &_volume}) {
        _dev.Destroy(*img);
    }
}

Image Fog::_Create(uint32_t x, uint32_t y, uint32_t z)
{
    Image img;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = kFormat;
    ci.extent = {x, y, z};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Check(vkCreateImage(_dev.device, &ci, nullptr, &img.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(_dev.device, img.image, &req);
    img.memory = _dev.Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindImageMemory(_dev.device, img.image, img.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = kFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Check(vkCreateImageView(_dev.device, &vci, nullptr, &img.view), "vkCreateImageView");
    return img;
}

bool Fog::Resize(uint32_t x, uint32_t y, uint32_t z)
{
    if (x == _x && y == _y && z == _z) {
        return _recorded;
    }
    _Destroy();
    _injected[0] = _Create(x, y, z);
    _injected[1] = _Create(x, y, z);
    _volume = _Create(x, y, z);
    _x = x;
    _y = y;
    _z = z;
    _recorded = false;
    // GENERAL from here on (storage and sampled alike); no fog until the
    // first Record.
    _dev.Submit([&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (const Image *img : {&_injected[0], &_injected[1], &_volume}) {
            ImageBarrier(cmd, img->image, range, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, img == &_volume ? 1.0f : 0.0f}};
            vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        }
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    return false;
}

void Fog::Descriptors(VkDescriptorImageInfo storage[3], VkDescriptorImageInfo *volume, VkSampler sampler) const
{
    storage[0] = {VK_NULL_HANDLE, _injected[_current].view, VK_IMAGE_LAYOUT_GENERAL};
    storage[1] = {VK_NULL_HANDLE, _injected[_current ^ 1].view, VK_IMAGE_LAYOUT_GENERAL};
    storage[2] = {VK_NULL_HANDLE, _volume.view, VK_IMAGE_LAYOUT_GENERAL};
    *volume = {sampler, _volume.view, VK_IMAGE_LAYOUT_GENERAL};
}

void Fog::Record(VkCommandBuffer cmd, VkPipelineLayout layout, VkDescriptorSet set)
{
    // After last frame's passes (which read this frame's targets) are done.
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _inject);
    vkCmdDispatch(cmd, (_x + 3) / 4, (_y + 3) / 4, (_z + 3) / 4);
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _integrate);
    vkCmdDispatch(cmd, (_x + 7) / 8, (_y + 7) / 8, 1);
    MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    _current ^= 1;
    _recorded = true;
    ++_frames;
}

float Fog::Jitter() const
{
    const float x = 0.5f + 0.618034f * float(_frames % 1024);
    return x - float(uint32_t(x));
}

} // namespace vkr
