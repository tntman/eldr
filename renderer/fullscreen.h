#pragma once

#include "device.h"

namespace vkr {

// Pipeline (through the device's cache on disk) drawing fullscreen.vert + `frag` into one color attachment of
// `format`: no depth, dynamic viewport/scissor, optionally additive or
// premultiplied over the target (`over`: src + (1 - src alpha) dst) or
// multiplied with it (`multiply`: src dst). With a `depthFormat`, also a
// depth attachment of it, written always (gl_FragDepth).
VkPipeline CreateFullscreenPipeline(const Device &dev, VkPipelineLayout layout,
                                    const uint32_t *frag, size_t fragBytes, VkFormat format,
                                    bool additive = false, bool over = false, bool multiply = false,
                                    VkFormat depthFormat = VK_FORMAT_UNDEFINED);

// One fullscreen triangle into `target` (in COLOR_ATTACHMENT_OPTIMAL).
void DrawFullscreen(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout,
                    VkDescriptorSet set, VkImageView target, uint32_t width, uint32_t height,
                    bool load, VkShaderStageFlags pushStages, const void *push, uint32_t pushBytes);

} // namespace vkr
