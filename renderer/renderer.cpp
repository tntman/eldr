#include "renderer.h"

#include <cstring>
#include <vector>

namespace vkr {

namespace {

const uint32_t kMeshVert[] =
#include "mesh.vert.inc"
;
const uint32_t kMeshFrag[] =
#include "mesh.frag.inc"
;

struct PushConstants {
    float viewProj[16];
    float cameraPos[3];
    uint32_t lightCount;
};

VkShaderModule CreateShader(VkDevice device, const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(device, &ci, nullptr, &module), "vkCreateShaderModule");
    return module;
}

VkImageSubresourceRange Whole(VkImageAspectFlags aspect)
{
    return {aspect, 0, 1, 0, 1};
}

} // namespace

Renderer::Renderer(Device &device)
    : _dev(device)
{
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = _dev.queueFamily;
    Check(vkCreateCommandPool(_dev.device, &pci, nullptr, &_pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = _pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(_dev.device, &ai, &_cmd), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(vkCreateFence(_dev.device, &fci, nullptr, &_fence), "vkCreateFence");

    // ponytail: one sampler (trilinear, 16x aniso, repeat) for every texture;
    // UsdUVTexture wrapS/wrapT are ignored.
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = VK_TRUE;
    sci.maxAnisotropy = 16.0f;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_sampler), "vkCreateSampler");

    // Bindless: binding 0 is a partially bound array indexed by material data.
    const VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    const VkDescriptorBindingFlags bindingFlags[3] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 3;
    flags.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags};
    slci.bindingCount = 3;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(_dev.device, &slci, nullptr, &_setLayout),
          "vkCreateDescriptorSetLayout");

    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(_dev.device, &dpci, nullptr, &_descriptorPool),
          "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = _descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &_setLayout;
    Check(vkAllocateDescriptorSets(_dev.device, &dsai, &_set), "vkAllocateDescriptorSets");

    _CreatePipeline();
}

Renderer::~Renderer()
{
    vkDeviceWaitIdle(_dev.device);
    _Destroy(_color);
    _Destroy(_depth);
    _dev.Destroy(_readback);
    vkDestroyPipeline(_dev.device, _pipeline, nullptr);
    vkDestroyPipelineLayout(_dev.device, _layout, nullptr);
    vkDestroyDescriptorPool(_dev.device, _descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(_dev.device, _setLayout, nullptr);
    vkDestroySampler(_dev.device, _sampler, nullptr);
    vkDestroyFence(_dev.device, _fence, nullptr);
    vkDestroyCommandPool(_dev.device, _pool, nullptr);
}

void Renderer::_CreatePipeline()
{
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(_dev.device, &lci, nullptr, &_layout), "vkCreatePipelineLayout");

    VkShaderModule vert = CreateShader(_dev.device, kMeshVert, sizeof(kMeshVert));
    VkShaderModule frag = CreateShader(_dev.device, kMeshFrag, sizeof(kMeshFrag));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vert, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main"},
    };

    const VkVertexInputBindingDescription bindings[2] = {
        {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };
    const VkVertexInputAttributeDescription attributes[9] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
        {7, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Instance, color)},
        {8, 1, VK_FORMAT_R32_UINT, offsetof(Instance, material)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 9;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;  // ponytail: all double-sided; honor doubleSided later
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;

    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &kColorFormat;
    rendering.depthAttachmentFormat = kDepthFormat;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = _layout;
    const VkResult r = vkCreateGraphicsPipelines(_dev.device, VK_NULL_HANDLE, 1, &ci,
                                                 nullptr, &_pipeline);
    vkDestroyShaderModule(_dev.device, vert, nullptr);
    vkDestroyShaderModule(_dev.device, frag, nullptr);
    Check(r, "vkCreateGraphicsPipelines");
}

Renderer::Image Renderer::_CreateImage(VkFormat format, VkImageUsageFlags usage,
                                       VkImageAspectFlags aspect)
{
    Image img;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {_width, _height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    Check(vkCreateImage(_dev.device, &ci, nullptr, &img.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(_dev.device, img.image, &req);
    img.memory = _dev.Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindImageMemory(_dev.device, img.image, img.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = Whole(aspect);
    Check(vkCreateImageView(_dev.device, &vci, nullptr, &img.view), "vkCreateImageView");
    return img;
}

void Renderer::_Destroy(Image &img)
{
    vkDestroyImageView(_dev.device, img.view, nullptr);
    vkDestroyImage(_dev.device, img.image, nullptr);
    vkFreeMemory(_dev.device, img.memory, nullptr);
    img = Image();
}

void Renderer::_Resize(uint32_t width, uint32_t height)
{
    _Destroy(_color);
    _Destroy(_depth);
    _width = width;
    _height = height;
    _color = _CreateImage(kColorFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT);
    _depth = _CreateImage(kDepthFormat,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
    _dev.Reserve(_readback, VkDeviceSize(width) * height * (8 + 4),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}

// The GPU is idle here (Render waits for every frame), so rewriting the set
// in place is legal without update-after-bind.
void Renderer::_UpdateDescriptors(const Scene::Gpu &gpu)
{
    std::vector<VkDescriptorImageInfo> images(gpu.textures.size());
    for (size_t i = 0; i < images.size(); ++i) {
        images[i] = {_sampler, gpu.textures[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    const VkDescriptorBufferInfo materials{gpu.materials, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lights{gpu.lights, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[3] = {};
    for (VkWriteDescriptorSet &w : writes) {
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = _set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[0].dstBinding = 1;
    writes[0].pBufferInfo = &materials;
    writes[1].dstBinding = 2;
    writes[1].pBufferInfo = &lights;
    writes[2].dstBinding = 0;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = uint32_t(images.size());
    writes[2].pImageInfo = images.data();
    vkUpdateDescriptorSets(_dev.device, images.empty() ? 2 : 3, writes, 0, nullptr);
}

// ponytail: synchronous, one frame in flight, CPU readback every frame.
// GL interop (step 7) removes the readback; frames in flight come with it.
void Renderer::Render(const Scene &scene, const Frame &frame, void *color, float *depth)
{
    if (frame.width == 0 || frame.height == 0) {
        return;
    }
    if (frame.width != _width || frame.height != _height) {
        _Resize(frame.width, frame.height);
    }
    const Scene::Gpu &gpu = scene.GetGpu();
    const VkDeviceSize colorBytes = VkDeviceSize(_width) * _height * 8;
    if (gpu.drawCount > 0) {
        _UpdateDescriptors(gpu);
    }

    Check(vkResetCommandBuffer(_cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(_cmd, &bi), "vkBeginCommandBuffer");

    ImageBarrier(_cmd, _color.image, Whole(VK_IMAGE_ASPECT_COLOR_BIT),
                 VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ImageBarrier(_cmd, _depth.image, Whole(VK_IMAGE_ASPECT_DEPTH_BIT),
                 VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAtt.imageView = _color.view;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(colorAtt.clearValue.color.float32, frame.clearColor, sizeof(frame.clearColor));
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = _depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil.depth = frame.clearDepth;

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {_width, _height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAtt;
    ri.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(_cmd, &ri);

    if (gpu.drawCount > 0) {
        // No Y flip: NDC y=-1 lands in memory row 0, which is the bottom row
        // in GL/Hydra order, so readback needs no flip either.
        const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
        const VkRect2D scissor{{0, 0}, {_width, _height}};
        vkCmdSetViewport(_cmd, 0, 1, &vp);
        vkCmdSetScissor(_cmd, 0, 1, &scissor);
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);

        PushConstants pc;
        std::memcpy(pc.viewProj, frame.viewProj, sizeof(pc.viewProj));
        std::memcpy(pc.cameraPos, frame.cameraPos, sizeof(pc.cameraPos));
        pc.lightCount = gpu.lightCount;
        vkCmdPushConstants(_cmd, _layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);

        const VkBuffer vbs[2] = {gpu.vertices, gpu.instances};
        const VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(_cmd, 0, 2, vbs, offsets);
        vkCmdDrawIndirect(_cmd, gpu.indirect, 0, gpu.drawCount, sizeof(VkDrawIndirectCommand));
    }
    vkCmdEndRendering(_cmd);

    ImageBarrier(_cmd, _color.image, Whole(VK_IMAGE_ASPECT_COLOR_BIT),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(_cmd, _depth.image, Whole(VK_IMAGE_ASPECT_DEPTH_BIT),
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {_width, _height, 1};
    vkCmdCopyImageToBuffer(_cmd, _color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           _readback.buffer, 1, &copy);
    copy.bufferOffset = colorBytes;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImageToBuffer(_cmd, _depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           _readback.buffer, 1, &copy);

    VkBufferMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = _readback.buffer;
    toHost.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(_cmd, &dep);

    Check(vkEndCommandBuffer(_cmd), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = _cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    Check(vkQueueSubmit2(_dev.queue, 1, &si, _fence), "vkQueueSubmit2");
    Check(vkWaitForFences(_dev.device, 1, &_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    Check(vkResetFences(_dev.device, 1, &_fence), "vkResetFences");

    const auto *src = static_cast<const uint8_t *>(_readback.mapped);
    std::memcpy(color, src, colorBytes);
    if (depth) {
        std::memcpy(depth, src + colorBytes, VkDeviceSize(_width) * _height * 4);
    }
}

} // namespace vkr
