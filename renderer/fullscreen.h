#pragma once

#include "device.h"

namespace vkr {

// Pipeline drawing fullscreen.vert + `frag` into one color attachment of
// `format`: no depth, dynamic viewport/scissor, optionally additive.
VkPipeline CreateFullscreenPipeline(VkDevice device, VkPipelineLayout layout,
                                    const uint32_t *frag, size_t fragBytes, VkFormat format,
                                    bool additive = false);

// One fullscreen triangle into `target` (in COLOR_ATTACHMENT_OPTIMAL).
void DrawFullscreen(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout,
                    VkDescriptorSet set, VkImageView target, uint32_t width, uint32_t height,
                    bool load, VkShaderStageFlags pushStages, const void *push, uint32_t pushBytes);

} // namespace vkr
