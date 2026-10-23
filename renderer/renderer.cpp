#include "renderer.h"

#include "ibl.h"
#include "vkmath.h"

#include <algorithm>
#include <cstring>
#include <tuple>
#include <vector>

namespace vkr {

// std140 mirror of FrameData in shaders/frame.glsl.
struct FrameData {
    float viewProj[16];
    float invViewProj[16];
    float cameraPos[4];
    float cameraForward[4];
    float viewport[4];
    uint32_t counts[4];  // lightCount, shadowLight, envLevels, envLatlong
    float cascadeViewProj[4][16];
    float cascadeSplits[4];
    float cascadeTexel[4];
    float envSh[9][4];
    float envWorldToLocal[16];
    float envRadiance[4];
    float prevViewProj[16];
    float ssr[4];  // enabled, noise index
    float clusters[4];       // near, far, log2(far / near), light cutoff radiance
    uint32_t localLights[4]; // sphere/rect light count
    uint32_t features[4];    // kFeature* flags, SSR steps, soft shadow samples
    float translucency[4];   // see frame.glsl
};

namespace {

const uint32_t kMeshVert[] =
#include "mesh.vert.inc"
;
const uint32_t kMeshDepthVert[] =
#include "mesh_depth.vert.inc"
;
const uint32_t kMeshFrag[] =
#include "mesh.frag.inc"
;
const uint32_t kMeshGraphVert[] =
#include "mesh_graph.vert.inc"
;
const uint32_t kMeshGraphFrag[] =
#include "mesh_graph.frag.inc"
;
const uint32_t kMaskFrag[] =
#include "mask.frag.inc"
;
const uint32_t kFullscreenVert[] =
#include "fullscreen.vert.inc"
;
const uint32_t kSkyFrag[] =
#include "sky.frag.inc"
;
const uint32_t kCullComp[] =
#include "cull.comp.inc"
;
const uint32_t kLightsComp[] =
#include "lights.comp.inc"
;
const uint32_t kSkinComp[] =
#include "skin.comp.inc"
;
const uint32_t kShadowTintFrag[] =
#include "shadow_tint.frag.inc"
;

// FrameData::features[0] (Frame switches the shaders read), as in frame.glsl.
constexpr uint32_t kFeatureNormalMaps = 1, kFeatureMaterialGraphs = 2, kFeatureClusters = 4;
constexpr uint32_t kFeatureProcedurals = 256;
// Translucency (transparency.glsl).
constexpr uint32_t kFeatureRefraction = 4096, kFeatureTranslucentShadows = 8192;
// Subsurface scattering (subsurface.glsl).
constexpr uint32_t kFeatureSubsurface = 16384;

// A material sampler: trilinear, `anisotropy` (1: none), wrap modes
// s + kWrapModes * t (Scene::Gpu::textureWraps).
VkSampler CreateMaterialSampler(VkDevice device, float anisotropy, uint32_t wrap)
{
    static const VkSamplerAddressMode modes[kWrapModes] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = modes[wrap % kWrapModes];
    sci.addressModeV = modes[wrap / kWrapModes % kWrapModes];
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;  // Storm's HdWrapBlack
    sci.anisotropyEnable = anisotropy > 1.0f;
    sci.maxAnisotropy = anisotropy;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    Check(vkCreateSampler(device, &sci, nullptr, &sampler), "vkCreateSampler");
    return sampler;
}

// Light clusters (shaders/lights.glsl): 16 x 9 x 24, a count and 63
// indices each.
constexpr VkDeviceSize kClusterBytes = 16 * 9 * 24 * 64 * sizeof(uint32_t);

// Push constants of cull.comp; the vertex stages read only the first 64 bytes
// (mat4 viewProj) of the same range.
struct CullParams {
    float planes[6][4];
    uint32_t instanceCapacity;
    uint32_t drawCount;
    float pixelScale;    // perspective: pixels per world unit at view depth 1; ortho: per unit
    float nearDistance;  // view depth of the near plane
    float minRadius;     // in pixels: smaller instances are dropped
    float orthographic;
    uint32_t listBase;     // list of instance indices written (see kVisibleLists)
    uint32_t commandBase;  // first of its draw commands
};

constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

// What the vertex stage reads of the same push range.
struct DrawParams {
    float viewProj[16];
    uint32_t listBase;
    uint32_t motion;  // see mesh.vert
};

void PushDraw(VkCommandBuffer cmd, VkPipelineLayout layout, const float *viewProj, uint32_t listBase,
              uint32_t motion = 0)
{
    DrawParams p;
    std::memcpy(p.viewProj, viewProj, sizeof(p.viewProj));
    p.listBase = listBase;
    p.motion = motion;
    vkCmdPushConstants(cmd, layout, kPushStages, 0, sizeof(p), &p);
}

// The opaque and cutout draws of one command list (commands[first] at
// offset), one indirect call per chunk (Scene::Gpu::drawChunks), each with
// its pipeline: `cutout` for chunks of cutout materials, in the chunk's
// variant (chunkKind >> 2, within `variants`: depth passes have no program
// variants). Translucent chunks are left to the translucent pass (and cast
// no shadows).
void DrawChunks(VkCommandBuffer cmd, const Scene::Gpu &gpu, VkBuffer commands, VkDeviceSize offset,
                const VkPipeline *opaque, const VkPipeline *cutout, uint8_t variants)
{
    constexpr VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
    VkPipeline bound = VK_NULL_HANDLE;
    for (size_t i = 0; i + 1 < gpu.drawChunks.size(); ++i) {
        const uint8_t kind = gpu.chunkKind[i] & (Scene::Gpu::kWide - 1);
        if (kind == Scene::Gpu::kTranslucent) {
            continue;
        }
        const VkPipeline pipeline = (kind == Scene::Gpu::kCutout ? cutout : opaque)[(gpu.chunkKind[i] >> 2) & variants];
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        const uint32_t first = gpu.drawChunks[i];
        vkCmdDrawIndexedIndirect(cmd, commands, offset + first * stride, gpu.drawChunks[i + 1] - first,
                                 uint32_t(stride));
    }
}

// World-space frustum planes (xyz . p + w >= 0 inside) of a GL clip-space
// viewProj in row-vector order (Gribb/Hartmann).
void FrustumPlanes(const float *vp, float planes[6][4])
{
    auto column = [&](int j, float out[4]) {
        for (int i = 0; i < 4; ++i) {
            out[i] = vp[i * 4 + j];
        }
    };
    float c[4][4];
    for (int j = 0; j < 4; ++j) {
        column(j, c[j]);
    }
    for (int p = 0; p < 6; ++p) {
        const float sign = (p % 2 == 0) ? 1.0f : -1.0f;  // left/right, bottom/top, near/far
        for (int i = 0; i < 4; ++i) {
            planes[p][i] = c[3][i] + sign * c[p / 2][i];
        }
        const float len = std::sqrt(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] +
                                    planes[p][2] * planes[p][2]);
        for (int i = 0; i < 4; ++i) {
            planes[p][i] /= len > 0 ? len : 1.0f;
        }
    }
}

constexpr uint32_t kBrdfLutSize = 64;

// Spots with a cone up to this get one perspective atlas tile, wider
// lights a cube of six. Must match kSpotShadowCone in mesh.frag.
constexpr float kSpotShadowCone = 1.3962634f;  // 80 degrees

VkImageSubresourceRange Range(VkImageAspectFlags aspect, uint32_t layers = 1)
{
    return {aspect, 0, 1, 0, layers};
}

Mat4 ToMat4(const float *m)
{
    Mat4 r;
    std::memcpy(r.m, m, sizeof(r.m));
    return r;
}

// Low-discrepancy sub-pixel offsets in [0, 1) for TAA jitter.
float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    for (; index > 0; index /= base) {
        f /= float(base);
        r += f * float(index % base);
    }
    return r;
}

// While the camera moves the TAA blend weight stays at 1 / (kTaaMoving + 1)
// = 0.1; a still camera accumulates Frame::taaSamples.
constexpr uint32_t kTaaMoving = 9;

} // namespace

Renderer::Renderer(Device &device)
    : _dev(device)
    , _gtao(device)
    , _taa(device)
    , _lens(device)
    , _post(device)
    , _transparency(device)
    , _subsurface(device)
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

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_dev.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families);
    vkGetPhysicalDeviceQueueFamilyProperties(_dev.physical, &families, family.data());
    if (family[_dev.queueFamily].timestampValidBits > 0) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(_dev.physical, &props);
        _timestampMs = double(props.limits.timestampPeriod) * 1e-6;
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kMaxTimestamps;
        Check(vkCreateQueryPool(_dev.device, &qci, nullptr, &_timestamps), "vkCreateQueryPool");
    }

    _dev.Reserve(_frameData, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    _clusters = _dev.CreateDeviceBuffer(kClusterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    _CreateDescriptors();
    _CreatePipelines();
    _CreateConstantImages();
}

Renderer::~Renderer()
{
    vkDeviceWaitIdle(_dev.device);
    for (VkImageView v : _shadowLayers) {
        vkDestroyImageView(_dev.device, v, nullptr);
    }
    for (Image *img : {&_color, &_primId, &_instanceId, &_depth, &_velocity, &_shadowMap, &_shadowAtlas, &_brdfLut,
                       &_blackCube}) {
        _dev.Destroy(*img);
    }
    _dev.Destroy(_readback);
    _dev.Destroy(_frameData);
    _dev.Destroy(_commands);
    _dev.Destroy(_prevJoints);
    _dev.Destroy(_clusters);
    _dev.Destroy(_lightShadows);
    _dev.Destroy(_shadowTiles);
    for (VkPipeline p : {_skyPipeline, _cullPipeline, _lightsPipeline, _skinPipeline}) {
        vkDestroyPipeline(_dev.device, p, nullptr);
    }
    for (int v = 0; v < 4; ++v) {
        for (VkPipeline p : {_prepassPipeline[v], _shadowPipeline[v], _prepassCutoutPipeline[v],
                             _shadowCutoutPipeline[v], _tintDepthPipeline[v], _tintColorPipeline[v]}) {
            vkDestroyPipeline(_dev.device, p, nullptr);
        }
    }
    for (int v = 0; v < 16; ++v) {
        vkDestroyPipeline(_dev.device, _pipeline[v], nullptr);
        vkDestroyPipeline(_dev.device, _translucentPipeline[v], nullptr);
        vkDestroyPipeline(_dev.device, _oitPipeline[v], nullptr);
        vkDestroyPipeline(_dev.device, _subsurfacePipeline[v], nullptr);
    }
    vkDestroyPipelineLayout(_dev.device, _layout, nullptr);
    vkDestroyDescriptorPool(_dev.device, _descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(_dev.device, _setLayout, nullptr);
    for (VkSampler s : _samplers) {
        vkDestroySampler(_dev.device, s, nullptr);
    }
    for (VkSampler s : {_clampSampler, _shadowSampler}) {
        vkDestroySampler(_dev.device, s, nullptr);
    }
    vkDestroyFence(_dev.device, _fence, nullptr);
    vkDestroyQueryPool(_dev.device, _timestamps, nullptr);
    vkDestroyCommandPool(_dev.device, _pool, nullptr);
}

void Renderer::_CreateDescriptors()
{
    // The materials' samplers are made by Render (Frame::anisotropy).
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxAnisotropy = 1.0f;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_clampSampler), "vkCreateSampler");

    // Outside the cascade reads as depth 1, i.e. lit.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    sci.maxLod = 0.0f;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_shadowSampler), "vkCreateSampler");

    constexpr VkShaderStageFlags kFragment = VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr VkShaderStageFlags kGeometry = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 0 is the bindless array, partially bound and indexed by material
    // data. 7..10 are the GPU-driven scene: instances, draw infos, visible
    // lists and the culled indirect commands; 11 is the GTAO result, 12
    // and 13 last frame's color and this frame's depth for reflections, 14
    // the light clusters (written by lights.comp, which also reads 2 and 3),
    // 15, 16, 21 and 22 skinning (SkinVertex, joint matrices built by
    // skin.comp, skinning transforms, per-joint rest bounds), 23 and 24
    // blend shape offsets and weights, 25 the UDIM tile table, 26 and 27
    // last frame's joints and moved rigid transforms (motion vectors), 17..19
    // the local lights' shadow atlas, first tile per light and the tiles, 20
    // the atlas again without depth compare (soft shadows' blocker search),
    // 28..32 material programs: ops, constants, primvar table, primvar values
    // and vertex-rate primvars; 42 the refraction background, 43..46 the
    // translucent shadows' tint maps (Transparency).
    constexpr VkShaderStageFlags kLighting = kFragment | VK_SHADER_STAGE_COMPUTE_BIT;
    constexpr uint32_t kBindings = 38;
    const VkDescriptorSetLayoutBinding bindings[kBindings] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures, kFragment},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kLighting},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kLighting},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry | kFragment},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry | kFragment},
        {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kLighting},
        {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
        {27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
        {28, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {29, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {31, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment},
        {32, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
        {42, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {43, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {44, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {45, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {46, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
    };
    const VkDescriptorBindingFlags bindingFlags[kBindings] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = kBindings;
    flags.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags};
    slci.bindingCount = kBindings;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(_dev.device, &slci, nullptr, &_setLayout),
          "vkCreateDescriptorSetLayout");

    const VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures + 13},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 23},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(_dev.device, &dpci, nullptr, &_descriptorPool),
          "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = _descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &_setLayout;
    Check(vkAllocateDescriptorSets(_dev.device, &dsai, &_set), "vkAllocateDescriptorSets");
}

void Renderer::_CreatePipelines()
{
    // One range for both users (vertex: mat4 viewProj; cull: CullParams), so
    // every push names the same stages and the ranges never partly overlap.
    VkPushConstantRange range{kPushStages, 0, sizeof(CullParams)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(_dev.device, &lci, nullptr, &_layout), "vkCreatePipelineLayout");

    const VkDevice device = _dev.device;
    for (auto [code, bytes, pipeline] : {std::tuple{kCullComp, sizeof(kCullComp), &_cullPipeline},
                                         std::tuple{kLightsComp, sizeof(kLightsComp), &_lightsPipeline},
                                         std::tuple{kSkinComp, sizeof(kSkinComp), &_skinPipeline}}) {
        VkShaderModule module = CreateShaderModule(device, code, bytes);
        VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
        cci.layout = _layout;
        const VkResult r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cci, nullptr, pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        Check(r, "vkCreateComputePipelines");
    }
    VkShaderModule meshVert = CreateShaderModule(device, kMeshVert, sizeof(kMeshVert));
    VkShaderModule depthVert = CreateShaderModule(device, kMeshDepthVert, sizeof(kMeshDepthVert));
    VkShaderModule meshFrag = CreateShaderModule(device, kMeshFrag, sizeof(kMeshFrag));
    VkShaderModule graphVert = CreateShaderModule(device, kMeshGraphVert, sizeof(kMeshGraphVert));
    VkShaderModule graphFrag = CreateShaderModule(device, kMeshGraphFrag, sizeof(kMeshGraphFrag));
    VkShaderModule maskFrag = CreateShaderModule(device, kMaskFrag, sizeof(kMaskFrag));
    VkShaderModule tintFrag = CreateShaderModule(device, kShadowTintFrag, sizeof(kShadowTintFrag));
    VkShaderModule skyVert = CreateShaderModule(device, kFullscreenVert, sizeof(kFullscreenVert));
    VkShaderModule skyFrag = CreateShaderModule(device, kSkyFrag, sizeof(kSkyFrag));
    auto stage = [](VkShaderStageFlagBits s, VkShaderModule m) {
        return VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                               nullptr, 0, s, m, "main"};
    };

    // Per-instance data comes from SSBOs (scene.glsl), not vertex attributes.
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {3, 0, VK_FORMAT_R16G16B16A16_SFLOAT, offsetof(Vertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo meshInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    meshInput.vertexBindingDescriptionCount = 1;
    meshInput.pVertexBindingDescriptions = &binding;
    meshInput.vertexAttributeDescriptionCount = 4;
    meshInput.pVertexAttributeDescriptions = attributes;
    VkPipelineVertexInputStateCreateInfo noInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

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
    // Prepass and shadows write depth; shading (and the sky) test against
    // the finished prepass depth, and write it only without a prepass
    // (dynamic depth write, Frame::depthPrepass).
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineDepthStencilStateCreateInfo depthReadOnly = depth;
    depthReadOnly.depthWriteEnable = VK_FALSE;
    depthReadOnly.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // Attachments: color, primId, instanceId, velocity. Identical blend
    // states, so no independentBlend feature is needed (the sky writes -1 ids
    // and no velocity).
    VkPipelineColorBlendAttachmentState attachments[4] = {};
    for (VkPipelineColorBlendAttachmentState &a : attachments) {
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 4;
    blend.pAttachments = attachments;
    const VkFormat colorFormats[4] = {kColorFormat, kIdFormat, kIdFormat, kVelocityFormat};
    VkDynamicState dynamicStates[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                       VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    const VkDynamicState shadingStates[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
    VkPipelineDynamicStateCreateInfo shadingDynamic = dynamic;
    shadingDynamic.dynamicStateCount = 3;
    shadingDynamic.pDynamicStates = shadingStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 4;
    rendering.pColorAttachmentFormats = colorFormats;
    rendering.depthAttachmentFormat = kDepthFormat;

    VkPipelineShaderStageCreateInfo stages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, meshVert),
                                                 stage(VK_SHADER_STAGE_FRAGMENT_BIT, meshFrag)};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &meshInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depthReadOnly;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &shadingDynamic;
    ci.layout = _layout;
    // Every mesh pipeline four times, its vertex stage specialized with
    // kWideSkin (variant bit 0: eight skinning influences) and
    // kDualQuaternion (bit 1), see mesh.vert; the shading ones four more
    // with the GRAPH shaders (bit 2: material programs) and four more of
    // those with noise (bit 3: program.glsl's kProcedurals).
    const VkSpecializationMapEntry skinEntries[2] = {{0, 0, sizeof(VkBool32)},
                                                     {1, sizeof(VkBool32), sizeof(VkBool32)}};
    const VkBool32 skinFlags[4][2] = {{VK_FALSE, VK_FALSE}, {VK_TRUE, VK_FALSE}, {VK_FALSE, VK_TRUE}, {VK_TRUE, VK_TRUE}};
    // The shading pipelines' fragment constants: transparency.glsl's
    // kShadingPass (`pass`: 1 OIT, 2 subsurface) and, with noise,
    // program.glsl's kProcedurals.
    const VkSpecializationMapEntry fragmentEntries[2] = {{12, 0, sizeof(uint32_t)},
                                                         {8, sizeof(uint32_t), sizeof(VkBool32)}};
    auto createMesh = [&](VkGraphicsPipelineCreateInfo info, VkPipeline *out, int variants, uint32_t pass = 0) {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        std::copy(info.pStages, info.pStages + info.stageCount, stages);
        info.pStages = stages;
        const uint32_t fragmentData[2] = {pass, VK_TRUE};
        const VkSpecializationInfo passSpec{1, fragmentEntries, sizeof(uint32_t), fragmentData};
        const VkSpecializationInfo proceduralSpec{2, fragmentEntries, sizeof(fragmentData), fragmentData};
        if (pass) {
            stages[1].pSpecializationInfo = &passSpec;
        }
        VkResult res = VK_SUCCESS;
        for (int v = 0; v < variants && res == VK_SUCCESS; ++v) {
            if ((v & 8) && !(v & 4)) {
                continue;  // noise comes with a program
            }
            const VkSpecializationInfo spec{2, skinEntries, sizeof(skinFlags[v & 3]), skinFlags[v & 3]};
            stages[0].pSpecializationInfo = &spec;  // the vertex stage comes first
            if (v >= 4) {  // shading pipelines only
                stages[0].module = graphVert;
                stages[1].module = graphFrag;
                stages[1].pSpecializationInfo = v >= 8 ? &proceduralSpec : &passSpec;
            }
            res = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &out[v]);
        }
        return res;
    };
    VkResult r = createMesh(ci, _pipeline, 16);

    // Translucent: premultiplied blending over the opaque colour (and its
    // coverage alpha); ids are written (picking), motion vectors stay the
    // opaque surface's.
    if (r == VK_SUCCESS) {
        VkPipelineColorBlendAttachmentState over[4] = {attachments[0], attachments[1], attachments[2],
                                                       attachments[3]};
        over[0].blendEnable = VK_TRUE;
        over[0].srcColorBlendFactor = over[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        over[0].dstColorBlendFactor = over[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        over[0].colorBlendOp = over[0].alphaBlendOp = VK_BLEND_OP_ADD;
        over[3].colorWriteMask = 0;
        VkPipelineColorBlendStateCreateInfo overBlend = blend;
        overBlend.pAttachments = over;
        VkGraphicsPipelineCreateInfo tci = ci;
        tci.pColorBlendState = &overBlend;
        r = createMesh(tci, _translucentPipeline, 16);
    }
    // Weighted blended OIT (Transparency): colour and coverage summed,
    // ids as above, location 3 the revealage, multiplied by 1 - coverage.
    if (r == VK_SUCCESS) {
        VkPipelineColorBlendAttachmentState oit[4] = {attachments[0], attachments[1], attachments[2],
                                                      attachments[3]};
        oit[0].blendEnable = VK_TRUE;
        oit[0].srcColorBlendFactor = oit[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        oit[0].dstColorBlendFactor = oit[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        oit[3].blendEnable = VK_TRUE;
        oit[3].srcColorBlendFactor = oit[3].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        oit[3].dstColorBlendFactor = oit[3].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        VkPipelineColorBlendStateCreateInfo oitBlend = blend;
        oitBlend.pAttachments = oit;
        const VkFormat oitFormats[4] = {Transparency::kAccumFormat, kIdFormat, kIdFormat,
                                        Transparency::kRevealageFormat};
        VkPipelineRenderingCreateInfo oitRendering = rendering;
        oitRendering.pColorAttachmentFormats = oitFormats;
        VkGraphicsPipelineCreateInfo oci = ci;
        oci.pNext = &oitRendering;
        oci.pColorBlendState = &oitBlend;
        r = createMesh(oci, _oitPipeline, 16, 1);
    }
    // Subsurface pass (Subsurface): the subsurface lobe's light and the
    // radii (locations 0 and 1), nothing else written.
    if (r == VK_SUCCESS) {
        VkPipelineColorBlendAttachmentState sss[4] = {attachments[0], attachments[1], attachments[2],
                                                      attachments[3]};
        sss[2].colorWriteMask = sss[3].colorWriteMask = 0;
        VkPipelineColorBlendStateCreateInfo sssBlend = blend;
        sssBlend.pAttachments = sss;
        const VkFormat sssFormats[4] = {Subsurface::kLightFormat, Subsurface::kIdFormat, kIdFormat, kVelocityFormat};
        VkPipelineRenderingCreateInfo sssRendering = rendering;
        sssRendering.pColorAttachmentFormats = sssFormats;
        VkGraphicsPipelineCreateInfo sci = ci;
        sci.pNext = &sssRendering;
        sci.pColorBlendState = &sssBlend;
        r = createMesh(sci, _subsurfacePipeline, 16, 2);
    }

    // Depth prepass: depth only, with mesh.vert's DEPTH_ONLY variant (same
    // positions, invariant). Shadow: plus slope-scaled bias.
    if (r == VK_SUCCESS) {
        const VkPipelineShaderStageCreateInfo depthStages[1] = {stage(VK_SHADER_STAGE_VERTEX_BIT, depthVert)};
        VkPipelineRenderingCreateInfo depthRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        depthRendering.depthAttachmentFormat = kDepthFormat;
        VkPipelineColorBlendStateCreateInfo noBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo pci = ci;
        pci.pNext = &depthRendering;
        pci.stageCount = 1;
        pci.pStages = depthStages;
        pci.pDepthStencilState = &depth;
        pci.pColorBlendState = &noBlend;
        pci.pDynamicState = &dynamic;
        r = createMesh(pci, _prepassPipeline, 4);

        VkPipelineRasterizationStateCreateInfo shadowRaster = raster;
        shadowRaster.depthBiasEnable = VK_TRUE;
        VkPipelineDynamicStateCreateInfo shadowDynamic = dynamic;
        shadowDynamic.dynamicStateCount = 3;
        VkGraphicsPipelineCreateInfo sci = pci;
        sci.pRasterizationState = &shadowRaster;
        sci.pDynamicState = &shadowDynamic;
        if (r == VK_SUCCESS) {
            r = createMesh(sci, _shadowPipeline, 4);
        }

        // Cutouts: the same two with the alpha test.
        const VkPipelineShaderStageCreateInfo maskStages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, depthVert),
                                                               stage(VK_SHADER_STAGE_FRAGMENT_BIT, maskFrag)};
        pci.stageCount = sci.stageCount = 2;
        pci.pStages = sci.pStages = maskStages;
        if (r == VK_SUCCESS) {
            r = createMesh(pci, _prepassCutoutPipeline, 4);
        }
        if (r == VK_SUCCESS) {
            r = createMesh(sci, _shadowCutoutPipeline, 4);
        }

        // Translucent shadow casters (Transparency's tint maps): depth like
        // the shadows, colour masked; then no depth test, colour
        // multiplied.
        const VkPipelineShaderStageCreateInfo tintStages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, depthVert),
                                                               stage(VK_SHADER_STAGE_FRAGMENT_BIT, tintFrag)};
        const VkFormat tintFormat = Transparency::kTintFormat;
        VkPipelineRenderingCreateInfo tintRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        tintRendering.colorAttachmentCount = 1;
        tintRendering.pColorAttachmentFormats = &tintFormat;
        tintRendering.depthAttachmentFormat = Transparency::kTintDepthFormat;
        VkPipelineColorBlendAttachmentState tintAttachment{};
        VkPipelineColorBlendStateCreateInfo tintBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        tintBlend.attachmentCount = 1;
        tintBlend.pAttachments = &tintAttachment;
        VkGraphicsPipelineCreateInfo tci = sci;
        tci.pNext = &tintRendering;
        tci.pStages = tintStages;
        tci.pColorBlendState = &tintBlend;
        if (r == VK_SUCCESS) {
            r = createMesh(tci, _tintDepthPipeline, 4);
        }
        tintAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        tintAttachment.blendEnable = VK_TRUE;
        tintAttachment.srcColorBlendFactor = tintAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        tintAttachment.dstColorBlendFactor = tintAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        VkPipelineDepthStencilStateCreateInfo noDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        tci.pDepthStencilState = &noDepth;
        if (r == VK_SUCCESS) {
            r = createMesh(tci, _tintColorPipeline, 4);
        }
    }

    // Sky: full-screen triangle at depth 1, drawn after geometry.
    if (r == VK_SUCCESS) {
        VkPipelineShaderStageCreateInfo skyStages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, skyVert),
                                                        stage(VK_SHADER_STAGE_FRAGMENT_BIT, skyFrag)};
        VkGraphicsPipelineCreateInfo kci = ci;
        kci.pStages = skyStages;
        kci.pVertexInputState = &noInput;
        r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &kci, nullptr, &_skyPipeline);
    }

    for (VkShaderModule m : {meshVert, depthVert, meshFrag, graphVert, graphFrag, maskFrag, skyVert, skyFrag,
                             tintFrag}) {
        vkDestroyShaderModule(device, m, nullptr);
    }
    Check(r, "vkCreateGraphicsPipelines");
}

// The sun's cascades, `size` texels square; Render makes them (again) when
// Frame::shadowResolution changes. The GPU is idle there.
void Renderer::_CreateShadowMap(uint32_t size)
{
    for (VkImageView &v : _shadowLayers) {
        vkDestroyImageView(_dev.device, v, nullptr);
        v = VK_NULL_HANDLE;
    }
    _dev.Destroy(_shadowMap);
    _shadowMap = _dev.CreateImage(kDepthFormat, size, size, kCascades, 1,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    for (uint32_t c = 0; c < kCascades; ++c) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = _shadowMap.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kDepthFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1};
        Check(vkCreateImageView(_dev.device, &vci, nullptr, &_shadowLayers[c]), "vkCreateImageView");
    }
    _shadowSize = size;
}

// The local lights' atlas, BRDF LUT and the no-dome placeholder never change
// size.
void Renderer::_CreateConstantImages()
{
    _shadowAtlas = _dev.CreateImage(kDepthFormat, kShadowAtlasSize, kShadowAtlasSize, 1, 1,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _brdfLut = _dev.CreateImage(VK_FORMAT_R32G32_SFLOAT, kBrdfLutSize, kBrdfLutSize, 1, 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _blackCube = _dev.CreateImage(VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 6, 1,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE,
                                  VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);

    const std::vector<float> lut = BuildBrdfLut(kBrdfLutSize);
    Buffer staging;
    _dev.Reserve(staging, lut.size() * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, lut.data(), lut.size() * sizeof(float));
    _dev.Submit([&](VkCommandBuffer cmd) {
        for (const Image *img : {&_brdfLut, &_blackCube}) {
            ImageBarrier(cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT, img == &_blackCube ? 6 : 1),
                         VK_PIPELINE_STAGE_2_NONE, 0,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kBrdfLutSize, kBrdfLutSize, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, _brdfLut.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        const VkClearColorValue black{};
        const VkImageSubresourceRange cube = Range(VK_IMAGE_ASPECT_COLOR_BIT, 6);
        vkCmdClearColorImage(cmd, _blackCube.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &cube);
        for (const Image *img : {&_brdfLut, &_blackCube}) {
            ImageBarrier(cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT, img == &_blackCube ? 6 : 1),
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });
    _dev.Destroy(staging);
}

void Renderer::_Resize(uint32_t width, uint32_t height)
{
    for (Image *img : {&_color, &_primId, &_instanceId, &_depth, &_velocity}) {
        _dev.Destroy(*img);
    }
    _width = width;
    _height = height;
    constexpr VkImageUsageFlags kTarget = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    _color = _dev.CreateImage(kColorFormat, width, height, 1, 1, kTarget,
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _primId = _dev.CreateImage(kIdFormat, width, height, 1, 1, kTarget,
                               VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _instanceId = _dev.CreateImage(kIdFormat, width, height, 1, 1, kTarget,
                                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _velocity = _dev.CreateImage(kVelocityFormat, width, height, 1, 1,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _depth = _dev.CreateImage(kDepthFormat, width, height, 1, 1,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _dev.Reserve(_readback, VkDeviceSize(width) * height * (8 + 4 + 4 + 4 + 4),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    _gtao.Resize(width, height, _depth.view);
    _taa.Resize(width, height, _color.view, _depth.view, _velocity.view);
    _lens.Resize(width, height, _color.image, _color.view, _depth.view, _velocity.view, _taa.OutputImage(),
                 _taa.Output(), _taa.History());
    _post.Resize(width, height, _taa.Output());
    _transparency.Resize(width, height);
    _subsurface.Resize(width, height, _depth.view);
    _taaValid = false;
    _historyValid = false;
}

// The GPU is idle here (Render waits for every frame), so rewriting the set
// in place is legal without update-after-bind.
void Renderer::_UpdateDescriptors(const Scene::Gpu &gpu, bool prepass, VkImageView background)
{
    std::vector<VkDescriptorImageInfo> images(gpu.textures.size());
    for (size_t i = 0; i < images.size(); ++i) {
        images[i] = {_samplers[gpu.textureWraps[i]], gpu.textures[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    const VkDescriptorBufferInfo materials{gpu.materials, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lights{gpu.lights, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo frame{_frameData.buffer, 0, sizeof(FrameData)};
    const VkDescriptorImageInfo shadow{_shadowSampler, _shadowMap.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo env{_clampSampler, gpu.environment ? gpu.environment : _blackCube.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo lut{_clampSampler, _brdfLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo instances{gpu.instances, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo draws{gpu.draws, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo visible{gpu.visible, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo commands{_commands.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo ao{_clampSampler, _gtao.Output(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo previous{_clampSampler, _taa.History(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    // Without the prepass the main pass writes depth and nothing reads it
    // there (no SSR): the LUT stands in, so the attachment isn't also bound
    // for sampling.
    const VkDescriptorImageInfo depth = prepass
        ? VkDescriptorImageInfo{_clampSampler, _depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}
        : VkDescriptorImageInfo{_clampSampler, _brdfLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo clusters{_clusters.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo skin{gpu.skin, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo joints{gpu.joints, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo skinXforms{gpu.skinXforms, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo jointBounds{gpu.jointBounds, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo blendOffsets{gpu.blendOffsets, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo blendWeights{gpu.blendWeights, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo udimTiles{gpu.udimTiles, 0, VK_WHOLE_SIZE};
    // Without skinned instances the joint pool may not exist yet: bind the
    // (unused) motion rows instead.
    const VkDescriptorBufferInfo prevJoints{_prevJoints.buffer ? _prevJoints.buffer : gpu.motion, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo motion{gpu.motion, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo programCode{gpu.programCode, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo programConstants{gpu.programConstants, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo primvarTable{gpu.primvarTable, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo primvarData{gpu.primvarData, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo primvarVertices{gpu.primvarVertices, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo atlas{_shadowSampler, _shadowAtlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo atlasDepth{_clampSampler, _shadowAtlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo lightShadows{_lightShadows.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo shadowTiles{_shadowTiles.buffer, 0, VK_WHOLE_SIZE};
    // Without refraction this frame the LUT stands in for its background.
    const VkDescriptorImageInfo refraction{_clampSampler, background ? background : _brdfLut.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const Transparency::TintMap &sunTint = _transparency.SunTint(), &atlasTint = _transparency.AtlasTint();
    const VkSampler nearest = _transparency.NearestSampler();
    const VkDescriptorImageInfo tints[4] = {
        {_clampSampler, sunTint.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest, sunTint.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {_clampSampler, atlasTint.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest, atlasTint.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};

    std::vector<VkWriteDescriptorSet> writes;
    auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo *buffer,
                     const VkDescriptorImageInfo *image, uint32_t count = 1) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = _set;
        w.dstBinding = binding;
        w.descriptorCount = count;
        w.descriptorType = type;
        w.pBufferInfo = buffer;
        w.pImageInfo = image;
        writes.push_back(w);
    };
    write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &materials, nullptr);
    write(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lights, nullptr);
    write(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &frame, nullptr);
    write(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &shadow);
    write(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &env);
    write(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &lut);
    write(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &instances, nullptr);
    write(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &draws, nullptr);
    write(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &visible, nullptr);
    write(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &commands, nullptr);
    write(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &ao);
    write(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &previous);
    write(13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &depth);
    write(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &clusters, nullptr);
    write(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &skin, nullptr);
    write(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &joints, nullptr);
    write(17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &atlas);
    write(18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lightShadows, nullptr);
    write(19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &shadowTiles, nullptr);
    write(20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &atlasDepth);
    write(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &skinXforms, nullptr);
    write(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &jointBounds, nullptr);
    write(23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &blendOffsets, nullptr);
    write(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &blendWeights, nullptr);
    write(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &udimTiles, nullptr);
    write(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &prevJoints, nullptr);
    write(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &motion, nullptr);
    write(28, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &programCode, nullptr);
    write(29, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &programConstants, nullptr);
    write(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &primvarTable, nullptr);
    write(31, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &primvarData, nullptr);
    write(32, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &primvarVertices, nullptr);
    write(42, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &refraction);
    for (uint32_t i = 0; i < 4; ++i) {
        write(43 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &tints[i]);
    }
    if (!images.empty()) {
        write(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, images.data(),
              uint32_t(images.size()));
    }
    vkUpdateDescriptorSets(_dev.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
}

// Splits the camera frustum (clamped to the scene) into Frame::shadowCascades
// slices and fits a stable, texel-snapped orthographic shadow frustum to
// each. Splits past the last are -1, so Shadow() finds none there.
bool Renderer::_FitCascades(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const
{
    if (gpu.shadowLight == kNone || !gpu.hasBounds) {
        return false;
    }
    const Mat4 invViewProj = ToMat4(frame.invViewProj);
    Vec3 nearC[4], farC[4], nearCenter, farCenter;
    for (int k = 0; k < 4; ++k) {
        const float x = (k & 1) ? 1.0f : -1.0f, y = (k & 2) ? 1.0f : -1.0f;
        nearC[k] = TransformPoint(invViewProj, {x, y, -1});
        farC[k] = TransformPoint(invViewProj, {x, y, 1});
        nearCenter = nearCenter + nearC[k] * 0.25f;
        farCenter = farCenter + farC[k] * 0.25f;
    }
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    const Vec3 forward = Normalize(farCenter - nearCenter);
    const float n = Dot(nearCenter - eye, forward), f = Dot(farCenter - eye, forward);

    Vec3 corners[8];
    float sceneFar = n;
    for (int k = 0; k < 8; ++k) {
        corners[k] = {(k & 1) ? gpu.boundsMax[0] : gpu.boundsMin[0],
                      (k & 2) ? gpu.boundsMax[1] : gpu.boundsMin[1],
                      (k & 4) ? gpu.boundsMax[2] : gpu.boundsMin[2]};
        sceneFar = std::max(sceneFar, Dot(corners[k] - eye, forward));
    }
    const float shadowFar = std::min(f, sceneFar);
    if (!(shadowFar > n) || !(f > n)) {
        return false;
    }

    const Vec3 toLight = Normalize({gpu.shadowDirection[0], gpu.shadowDirection[1], gpu.shadowDirection[2]});
    const Mat4 lightView = LookAlong(toLight * -1.0f);
    float zMin = 1e30f, zMax = -1e30f;  // scene depth range in light view space
    for (const Vec3 &c : corners) {
        const float z = TransformPoint(lightView, c).z;
        zMin = std::min(zMin, z);
        zMax = std::max(zMax, z);
    }
    const float margin = 0.01f * (zMax - zMin) + 1e-3f;

    const uint32_t cascades = std::clamp(frame.shadowCascades, 1u, kCascades);
    std::fill(data->cascadeSplits, data->cascadeSplits + kCascades, -1.0f);
    float splitNear = n;
    for (uint32_t c = 0; c < cascades; ++c) {
        // Practical split scheme: 75% logarithmic, 25% uniform.
        const float t = float(c + 1) / float(cascades);
        const float logSplit = n > 0 ? n * std::pow(shadowFar / n, t) : shadowFar * t;
        const float splitFar = 0.75f * logSplit + 0.25f * (n + (shadowFar - n) * t);

        Vec3 slice[8], center;
        for (int k = 0; k < 4; ++k) {
            slice[k] = nearC[k] + (farC[k] - nearC[k]) * ((splitNear - n) / (f - n));
            slice[k + 4] = nearC[k] + (farC[k] - nearC[k]) * ((splitFar - n) / (f - n));
        }
        for (const Vec3 &p : slice) {
            center = center + p * 0.125f;
        }
        float radius = 0;
        for (const Vec3 &p : slice) {
            radius = std::max(radius, Length(p - center));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;  // quantized so it doesn't shimmer

        // Snap the center to whole shadow texels in light space.
        const float texel = 2 * radius / float(_shadowSize);
        Vec3 lc = TransformPoint(lightView, center);
        lc.x = std::floor(lc.x / texel) * texel;
        lc.y = std::floor(lc.y / texel) * texel;
        const Mat4 proj = Ortho(lc.x - radius, lc.x + radius, lc.y - radius, lc.y + radius,
                                -zMax - margin, -zMin + margin);
        const Mat4 viewProj = lightView * proj;
        std::memcpy(data->cascadeViewProj[c], viewProj.m, sizeof(viewProj.m));
        data->cascadeSplits[c] = splitFar;
        data->cascadeTexel[c] = texel;
        splitNear = splitFar;
    }
    data->cameraForward[0] = forward.x;
    data->cameraForward[1] = forward.y;
    data->cameraForward[2] = forward.z;
    return true;
}

// Depth range of the light clusters (near plane to the scene's far end,
// along the view axis) and the cutoff that bounds each light's range: 1% of
// the radiance the current exposure maps to middle gray.
void Renderer::_FitClusters(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const
{
    const Mat4 invViewProj = ToMat4(frame.invViewProj);
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    const Vec3 nearCenter = TransformPoint(invViewProj, {0, 0, -1});
    const Vec3 forward = Normalize(TransformPoint(invViewProj, {0, 0, 0}) - nearCenter);
    float n = Dot(nearCenter - eye, forward);
    float f = Dot(TransformPoint(invViewProj, {0, 0, 1}) - eye, forward);
    if (!(f < 1e30f)) {
        f = 1e30f;  // infinite far plane
    }
    if (gpu.hasBounds) {
        float sceneFar = n;
        for (int k = 0; k < 8; ++k) {
            const Vec3 c{(k & 1) ? gpu.boundsMax[0] : gpu.boundsMin[0], (k & 2) ? gpu.boundsMax[1] : gpu.boundsMin[1],
                         (k & 4) ? gpu.boundsMax[2] : gpu.boundsMin[2]};
            sceneFar = std::max(sceneFar, Dot(c - eye, forward));
        }
        f = std::min(f, sceneFar);
    }
    n = std::max(n, 1e-5f * std::max(f, 1e-3f));  // log slices need near > 0
    f = std::max(f, n * 1.01f);
    const float exposure = frame.postProcess ? _post.Exposure() : 1.0f;
    data->clusters[0] = n;
    data->clusters[1] = f;
    data->clusters[2] = std::log2(f / n);
    data->clusters[3] = 0.01f * 0.18f / exposure;
    data->cameraForward[0] = forward.x;
    data->cameraForward[1] = forward.y;
    data->cameraForward[2] = forward.z;
}

// std430 mirror of ShadowTile in mesh.frag.
struct Renderer::ShadowTile {
    float viewProj[16];
    float rect[4];    // atlas uv offset, uv scale
    float params[4];  // shadow texel size per unit of distance from the light, near, far, light size
};

// Gives atlas tiles to the shadow-casting lights whose range reaches into
// the view, brightest at the camera first, until the atlas is full; the
// rest stay unshadowed. Writes each light's first tile (or kNone).
std::vector<Renderer::ShadowTile> Renderer::_PlanLocalShadows(const Scene::Gpu &gpu, const Frame &frame,
                                                              float cutoff)
{
    const uint32_t lights = gpu.lightCount + gpu.localLightCount;
    _dev.Reserve(_lightShadows, std::max(lights, 1u) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto *firstTile = static_cast<uint32_t *>(_lightShadows.mapped);
    std::fill(firstTile, firstTile + lights, kNone);
    std::vector<ShadowTile> tiles;

    float planes[6][4];
    FrustumPlanes(frame.viewProj, planes);
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    struct Candidate {
        const Scene::Gpu::ShadowCaster *caster;
        float range, priority;
    };
    std::vector<Candidate> candidates;
    for (const Scene::Gpu::ShadowCaster &c : gpu.shadowCasters) {
        if (!frame.localShadows || !frame.localLights) {
            break;
        }
        // Rounded up to a power of sqrt 2: the cutoff follows the exposure,
        // which drifts while it adapts, and every change of a tile's far
        // plane redraws the atlas.
        const float range = std::exp2(std::ceil(std::log2(std::sqrt(c.rangeIntensity / cutoff)) * 2.0f) * 0.5f) +
                            c.extent;
        bool inView = range > 0.0f;
        for (const auto &p : planes) {
            inView = inView && p[0] * c.position[0] + p[1] * c.position[1] + p[2] * c.position[2] + p[3] >= -range;
        }
        if (inView) {
            const Vec3 d = Vec3{c.position[0], c.position[1], c.position[2]} - eye;
            candidates.push_back({&c, range, c.rangeIntensity / std::max(Dot(d, d), 1e-12f)});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.priority > b.priority; });

    // Pick by priority, then place in light order: as long as the same lights
    // win, the tiles (and the cached atlas) stay where they were.
    const uint32_t tileSize = std::clamp(frame.localShadowResolution, 64u, kShadowAtlasSize / 2);
    const uint32_t perRow = kShadowAtlasSize / tileSize, capacity = perRow * perRow;
    std::vector<Candidate> chosen;
    uint32_t used = 0;
    for (const Candidate &cand : candidates) {
        const uint32_t need = cand.caster->coneAngle <= kSpotShadowCone ? 1 : 6;
        if (used + need <= capacity) {
            chosen.push_back(cand);
            used += need;
        }
    }
    std::sort(chosen.begin(), chosen.end(),
              [](const Candidate &a, const Candidate &b) { return a.caster->light < b.caster->light; });

    const float tileUv = float(tileSize) / float(kShadowAtlasSize);
    const Vec3 cube[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Candidate &cand : chosen) {
        const Scene::Gpu::ShadowCaster &c = *cand.caster;
        const bool spot = c.coneAngle <= kSpotShadowCone;
        firstTile[c.light] = uint32_t(tiles.size());
        const Vec3 position{c.position[0], c.position[1], c.position[2]};
        const float n = std::max(c.radius * 1.05f, cand.range * 1e-4f), f = std::max(cand.range, n * 2.0f);
        // A few texels of margin: PCF near a tile's (or cube face's) edge
        // must not read the neighbour.
        const float margin = 1.0f + 6.0f / float(tileSize);
        const float tanHalf = (spot ? std::tan(c.coneAngle) : 1.0f) * margin;
        auto add = [&](Vec3 direction) {
            ShadowTile t{};
            const Mat4 viewProj = Translate(position * -1.0f) * LookAlong(direction) * Perspective(tanHalf, n, f);
            std::memcpy(t.viewProj, viewProj.m, sizeof(t.viewProj));
            const uint32_t k = uint32_t(tiles.size());
            t.rect[0] = float(k % perRow) * tileUv;
            t.rect[1] = float(k / perRow) * tileUv;
            t.rect[2] = t.rect[3] = tileUv;
            t.params[0] = 2.0f * tanHalf / float(tileSize);
            t.params[1] = n;
            t.params[2] = f;
            t.params[3] = frame.softShadows ? c.size : 0.0f;
            tiles.push_back(t);
        };
        if (spot) {
            add({c.axis[0], c.axis[1], c.axis[2]});
        } else {
            for (const Vec3 &d : cube) {
                add(d);  // same face order as LocalShadow() in mesh.frag
            }
        }
    }
    _dev.Reserve(_shadowTiles, std::max<size_t>(tiles.size(), 1) * sizeof(ShadowTile),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(tiles.begin(), tiles.end(), static_cast<ShadowTile *>(_shadowTiles.mapped));
    return tiles;
}

// Draws the tiles one after another, each culled on the GPU against its
// own frustum (dropping casters under half a shadow texel) into the shared
// tile list, unless the atlas already holds exactly these tiles of this
// scene version (static scenes and lights: drawn once, e.g. for all of TAA's
// accumulation frames). Without tiles the atlas is only transitioned for
// its descriptor.
// ponytail: all or nothing; a moving caster redraws every light's tiles.
void Renderer::_RenderLocalShadows(const Scene::Gpu &gpu, const std::vector<ShadowTile> &tiles, bool tint)
{
    const VkImageSubresourceRange range = Range(VK_IMAGE_ASPECT_DEPTH_BIT);
    std::vector<float> key;
    for (const ShadowTile &t : tiles) {
        key.insert(key.end(), t.viewProj, t.viewProj + 16);
        key.insert(key.end(), t.rect, t.rect + 4);
    }
    key.push_back(tint ? float(_transparency.AtlasTint().size) : 0.0f);
    if (!tiles.empty() && gpu.version == _atlasVersion && key == _atlasKey) {
        return;  // still in SHADER_READ_ONLY_OPTIMAL from when it was drawn
    }
    _atlasVersion = tiles.empty() ? 0 : gpu.version;
    _atlasKey = std::move(key);
    if (tiles.empty()) {
        ImageBarrier(_cmd, _shadowAtlas.image, range, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return;
    }
    constexpr VkPipelineStageFlags2 kTests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    auto memoryBarrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                             VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(_cmd, &dep);
    };
    // Clear the whole atlas once; each tile then loads and draws its rect.
    ImageBarrier(_cmd, _shadowAtlas.image, range, VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearDepthStencilValue far{1.0f, 0};
    vkCmdClearDepthStencilImage(_cmd, _shadowAtlas.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &range);
    if (tint) {
        _transparency.BeginTint(_cmd, _transparency.AtlasTint(), 1);
    }
    ImageBarrier(_cmd, _shadowAtlas.image, range, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 kTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    const VkDeviceSize commandBytes = VkDeviceSize(gpu.drawCount) * sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize tileCommands = (1 + kCascades) * commandBytes;
    const uint32_t tileList = (1 + kCascades) * gpu.instanceCapacity;
    const VkDeviceSize vertexOffset = 0;
    for (const ShadowTile &t : tiles) {
        // The previous tile's draw is done with the list and commands.
        memoryBarrier(VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | kTests,
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | kTests,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        const VkBufferCopy reset{0, tileCommands, commandBytes};
        vkCmdCopyBuffer(_cmd, gpu.commandTemplate, _commands.buffer, 1, &reset);
        memoryBarrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        CullParams cull{};
        FrustumPlanes(t.viewProj, cull.planes);
        cull.instanceCapacity = gpu.instanceCapacity;
        cull.drawCount = gpu.drawCount;
        cull.pixelScale = 1.0f / t.params[0];  // shadow texels per unit at distance 1
        cull.nearDistance = t.params[1];
        cull.minRadius = 0.5f;
        cull.listBase = tileList;
        cull.commandBase = (1 + kCascades) * gpu.drawCount;
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(cull), &cull);
        vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);
        memoryBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        const int32_t x = int32_t(std::lround(t.rect[0] * kShadowAtlasSize));
        const int32_t y = int32_t(std::lround(t.rect[1] * kShadowAtlasSize));
        const uint32_t size = uint32_t(std::lround(t.rect[2] * kShadowAtlasSize));
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = _shadowAtlas.view;
        att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{x, y}, {size, size}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &att;
        vkCmdBeginRendering(_cmd, &ri);
        const VkViewport vp{float(x), float(y), float(size), float(size), 0, 1};
        vkCmdSetViewport(_cmd, 0, 1, &vp);
        vkCmdSetScissor(_cmd, 0, 1, &ri.renderArea);
        // GTAO bound its own set; the shadow pass needs the scene's.
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline[0]);
        vkCmdSetDepthBias(_cmd, 0.0f, 0.0f, 1.5f);
        vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
        vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
        PushDraw(_cmd, _layout, t.viewProj, tileList);
        DrawChunks(_cmd, gpu, _commands.buffer, tileCommands, _shadowPipeline, _shadowCutoutPipeline, 3);
        vkCmdEndRendering(_cmd);
        if (tint) {
            const Transparency::TintMap &map = _transparency.AtlasTint();
            const float scale = float(map.size) / float(kShadowAtlasSize);
            const int32_t tx = int32_t(std::lround(x * scale)), ty = int32_t(std::lround(y * scale));
            const uint32_t tsize = std::max(uint32_t(std::lround(size * scale)), 1u);
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = map.color.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingAttachmentInfo depth = color;
            depth.imageView = map.depth.view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            VkRenderingInfo tri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            tri.renderArea = {{tx, ty}, {tsize, tsize}};
            tri.layerCount = 1;
            tri.colorAttachmentCount = 1;
            tri.pColorAttachments = &color;
            tri.pDepthAttachment = &depth;
            vkCmdBeginRendering(_cmd, &tri);
            const VkViewport tvp{float(tx), float(ty), float(tsize), float(tsize), 0, 1};
            vkCmdSetViewport(_cmd, 0, 1, &tvp);
            vkCmdSetScissor(_cmd, 0, 1, &tri.renderArea);
            _DrawTintCasters(gpu, tileCommands);
            vkCmdEndRendering(_cmd);
        }
    }
    if (tint) {
        _transparency.EndTint(_cmd, _transparency.AtlasTint(), 1);
    }
    ImageBarrier(_cmd, _shadowAtlas.image, range, kTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Translucent meshes after the opaque pass and the sky, back to front by
// their bounds' centres; they test the opaque depth but write none.
// Frame::oit sums them, weighted by distance, into the Transparency
// targets and resolves that over the colour; otherwise each is blended
// over the last. Both draw back to front, so the nearest mesh's ids win
// picking.
// ponytail: sorted per mesh, not per instance or triangle (OIT's weights
// need no order but only approximate it).
void Renderer::_RenderTranslucency(const Scene::Gpu &gpu, const Frame &frame, FrameData *data,
                                   const float *viewProj, uint32_t motion, VkRenderingAttachmentInfo depth)
{
    std::vector<std::pair<float, size_t>> order;
    for (size_t i = 0; i < gpu.translucent.size(); ++i) {
        const float *c = &gpu.translucentCenters[i * 3];
        const Vec3 d{c[0] - frame.cameraPos[0], c[1] - frame.cameraPos[1], c[2] - frame.cameraPos[2]};
        order.push_back({Dot(d, d), i});
    }
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    data->translucency[2] = 1.0f / std::max(std::sqrt(order.back().first), 1e-6f);
    data->translucency[3] = std::max(frame.oitDepthWeight, 0.0f);

    // The opaque pass's colour, ids and (without a prepass) depth are
    // written before this pass loads them.
    VkMemoryBarrier2 opaque{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    opaque.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    opaque.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    opaque.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    opaque.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &opaque;
    vkCmdPipelineBarrier2(_cmd, &dep);

    if (data->features[0] & kFeatureRefraction) {
        _transparency.RecordBackground(_cmd, _color.image);
    }
    const bool oit = frame.oit;
    if (oit) {
        _transparency.BeginAccumulation(_cmd);
    }
    const VkImageView views[4] = {oit ? _transparency.Accum().view : _color.view, _primId.view, _instanceId.view,
                                  oit ? _transparency.Revealage().view : _velocity.view};
    VkRenderingAttachmentInfo atts[4] = {};
    for (int i = 0; i < 4; ++i) {
        atts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        atts[i].imageView = views[i];
        atts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[i].loadOp = oit && (i == 0 || i == 3) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    atts[3].clearValue.color.float32[0] = 1.0f;  // revealage: nothing in front yet
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = frame.depthPrepass ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {_width, _height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 4;
    ri.pColorAttachments = atts;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(_cmd, &ri);
    const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
    vkCmdSetViewport(_cmd, 0, 1, &vp);
    vkCmdSetScissor(_cmd, 0, 1, &ri.renderArea);
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    PushDraw(_cmd, _layout, viewProj, 0, motion);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
    vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
    const VkPipeline *pipelines = oit ? _oitPipeline : _translucentPipeline;
    VkPipeline bound = VK_NULL_HANDLE;
    for (const auto &[distance, i] : order) {
        if (pipelines[gpu.translucentVariant[i]] != bound) {
            bound = pipelines[gpu.translucentVariant[i]];
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
        }
        vkCmdDrawIndexedIndirect(_cmd, _commands.buffer,
                                 VkDeviceSize(gpu.translucent[i]) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }
    vkCmdEndRendering(_cmd);
    if (oit) {
        _transparency.Resolve(_cmd, _color.view);
    }
}

// The translucent shadow casters of one command list (commands[first] at
// offset `commands`): their nearest depth, then their colours multiplied
// (in a rendering into a tint map; push constants and buffers bound).
void Renderer::_DrawTintCasters(const Scene::Gpu &gpu, VkDeviceSize commands)
{
    vkCmdSetDepthBias(_cmd, 0.0f, 0.0f, 1.5f);
    for (const VkPipeline *pipelines : {_tintDepthPipeline, _tintColorPipeline}) {
        VkPipeline bound = VK_NULL_HANDLE;
        for (size_t i = 0; i < gpu.translucent.size(); ++i) {
            if (pipelines[gpu.translucentVariant[i] & 3] != bound) {
                bound = pipelines[gpu.translucentVariant[i] & 3];
                vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
            }
            vkCmdDrawIndexedIndirect(_cmd, _commands.buffer,
                                     commands + VkDeviceSize(gpu.translucent[i]) * sizeof(VkDrawIndexedIndirectCommand),
                                     1, sizeof(VkDrawIndexedIndirectCommand));
        }
    }
}

// Subsurface meshes shaded again after the opaque pass, their subsurface
// lobe only, into Subsurface's targets (testing the opaque depth), which
// it then blurs into the colour.
void Renderer::_RenderSubsurface(const Scene::Gpu &gpu, const Frame &frame, const float *viewProj,
                                 const float *invViewProj, VkRenderingAttachmentInfo depth)
{
    // The opaque pass's ids, velocity and colour are written before this
    // pass loads (and the blur adds to) them.
    VkMemoryBarrier2 opaque{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    opaque.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    opaque.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    opaque.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    opaque.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &opaque;
    vkCmdPipelineBarrier2(_cmd, &dep);

    _subsurface.BeginLighting(_cmd);
    const VkImageView views[4] = {_subsurface.Light().view, _subsurface.Id().view, _instanceId.view, _velocity.view};
    VkRenderingAttachmentInfo atts[4] = {};
    for (int i = 0; i < 4; ++i) {
        atts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        atts[i].imageView = views[i];
        atts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[i].loadOp = i < 2 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {_width, _height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 4;
    ri.pColorAttachments = atts;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(_cmd, &ri);
    const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
    vkCmdSetViewport(_cmd, 0, 1, &vp);
    vkCmdSetScissor(_cmd, 0, 1, &ri.renderArea);
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    PushDraw(_cmd, _layout, viewProj, 0);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
    vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
    VkPipeline bound = VK_NULL_HANDLE;
    for (size_t i = 0; i < gpu.subsurface.size(); ++i) {
        if (_subsurfacePipeline[gpu.subsurfaceVariant[i]] != bound) {
            bound = _subsurfacePipeline[gpu.subsurfaceVariant[i]];
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
        }
        vkCmdDrawIndexedIndirect(_cmd, _commands.buffer,
                                 VkDeviceSize(gpu.subsurface[i]) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }
    vkCmdEndRendering(_cmd);
    _subsurface.Blur(_cmd, _color.view, invViewProj, frame.subsurfaceSamples);
}

bool Renderer::IsConverged() const
{
    return (!_adapting || _post.ExposureConverged()) && _taaConverged;
}

uint32_t Renderer::VisibleInstances() const
{
    const auto *commands = static_cast<const VkDrawIndexedIndirectCommand *>(_commands.mapped);
    uint32_t total = 0;
    for (uint32_t i = 0; i < _lastDrawCount; ++i) {
        total += commands[i].instanceCount;
    }
    return total;
}

// ponytail: synchronous, one frame in flight, CPU readback every frame.
// GL interop (step 7) removes the readback; frames in flight come with it.
void Renderer::Render(const Scene &scene, const Frame &frame, const Targets &out)
{
    if (frame.width == 0 || frame.height == 0) {
        return;
    }
    const auto cpuStart = std::chrono::steady_clock::now();
    if (frame.width != _width || frame.height != _height) {
        _Resize(frame.width, frame.height);
    }
    const Scene::Gpu &gpu = scene.GetGpu();
    const uint32_t shadowSize = std::clamp(frame.shadowResolution, 256u, 8192u);
    if (shadowSize != _shadowSize) {
        _CreateShadowMap(shadowSize);
    }
    const float anisotropy = float(std::clamp(frame.anisotropy, 1u, 16u));
    if (anisotropy != _anisotropy) {
        for (uint32_t wrap = 0; wrap < kWrapModes * kWrapModes; ++wrap) {
            vkDestroySampler(_dev.device, _samplers[wrap], nullptr);
            _samplers[wrap] = CreateMaterialSampler(_dev.device, anisotropy, wrap);
        }
        _anisotropy = anisotropy;
    }

    FrameData &fd = *static_cast<FrameData *>(_frameData.mapped);
    fd = FrameData{};
    fd.features[0] = (frame.normalMaps ? kFeatureNormalMaps : 0) | (frame.materialGraphs ? kFeatureMaterialGraphs : 0) |
                     (frame.clusteredLights ? kFeatureClusters : 0);
    fd.features[0] |= frame.proceduralTextures ? kFeatureProcedurals : 0;
    fd.features[1] = std::clamp(frame.ssrSteps, 1u, 256u);
    fd.features[2] = std::clamp(frame.softShadowSamples, 1u, 16u);
    std::memcpy(fd.viewProj, frame.viewProj, sizeof(fd.viewProj));
    std::memcpy(fd.invViewProj, frame.invViewProj, sizeof(fd.invViewProj));

    // TAA: offset the main pass by a sub-pixel Halton jitter. The camera
    // matrices used for culling, shadows and reprojection stay unjittered.
    const bool taa = frame.postProcess && frame.taa;
    float jitteredViewProj[16];
    std::memcpy(jitteredViewProj, frame.viewProj, sizeof(jitteredViewProj));
    Mat4 unjitter;          // jittered NDC -> unjittered NDC
    const uint32_t taaSamples = std::clamp(frame.taaSamples, 1u, 256u);
    uint32_t sample = 0;    // jitter (and GTAO noise) index, 1..taaSamples
    if (taa) {
        sample = (_haltonIndex++ % taaSamples) + 1;
        const float jx = (Halton(sample, 2) - 0.5f) * 2.0f / float(_width);
        const float jy = (Halton(sample, 3) - 0.5f) * 2.0f / float(_height);
        for (int r = 0; r < 4; ++r) {  // clip.xy += jitter * clip.w
            jitteredViewProj[r * 4 + 0] += jx * jitteredViewProj[r * 4 + 3];
            jitteredViewProj[r * 4 + 1] += jy * jitteredViewProj[r * 4 + 3];
        }
        unjitter.m[3][0] = -jx;
        unjitter.m[3][1] = -jy;
        // The sky reconstructs rays per pixel, so it follows the jitter too.
        const Mat4 invJittered = unjitter * ToMat4(frame.invViewProj);
        std::memcpy(fd.invViewProj, invJittered.m, sizeof(fd.invViewProj));
    }
    std::memcpy(fd.viewProj, jitteredViewProj, sizeof(fd.viewProj));

    // Last frame's camera (motion vectors; without history: this one).
    // Reflections shade hits with last frame's resolved image, which exists
    // once post (TAA resolve) ran at this size.
    std::memcpy(fd.prevViewProj, _historyValid ? _prevViewProj : frame.viewProj, sizeof(fd.prevViewProj));
    if (frame.ssr && frame.postProcess && frame.depthPrepass && _historyValid) {
        fd.ssr[0] = 1.0f;
        fd.ssr[1] = float(sample);
    }

    std::memcpy(fd.cameraPos, frame.cameraPos, sizeof(frame.cameraPos));
    fd.viewport[0] = float(_width);
    fd.viewport[1] = float(_height);
    const bool shadows = frame.shadows && gpu.drawCount > 0 && gpu.instanceCapacity > 0 &&
                         _FitCascades(gpu, frame, &fd);
    const uint32_t cascades = shadows ? std::clamp(frame.shadowCascades, 1u, kCascades) : 0;
    const uint32_t localLights = frame.localLights ? gpu.localLightCount : 0;
    fd.counts[0] = gpu.lightCount;
    fd.localLights[0] = localLights;
    fd.localLights[1] = sample;
    if (localLights > 0) {
        _FitClusters(gpu, frame, &fd);
    }
    const std::vector<ShadowTile> shadowTiles = _PlanLocalShadows(gpu, frame, fd.clusters[3]);
    fd.counts[1] = shadows ? gpu.shadowLight : kNone;
    fd.counts[2] = gpu.environmentLevels;
    fd.counts[3] = gpu.environmentLatlong;
    const bool ibl = gpu.environment && frame.imageBasedLighting;
    if (ibl) {
        for (int i = 0; i < 9; ++i) {
            std::memcpy(fd.envSh[i], gpu.environmentSh[i], sizeof(gpu.environmentSh[i]));
        }
        std::memcpy(fd.envWorldToLocal, gpu.environmentWorldToLocal, sizeof(fd.envWorldToLocal));
        std::memcpy(fd.envRadiance, gpu.environmentRadiance, sizeof(gpu.environmentRadiance));
        fd.envRadiance[3] = 1.0f;
    }
    const bool sky = gpu.environment && frame.showEnvironment && gpu.environmentLatlong != kNone;
    // Culling output; host-visible so VisibleInstances() can read it back.
    // One set of commands for the camera, one per cascade, one for atlas tiles.
    _dev.Reserve(_commands, VkDeviceSize(gpu.drawCount) * (2 + kCascades) * sizeof(VkDrawIndexedIndirectCommand),
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // Last frame's joints exist when the pool is still the buffer they were
    // built in (it is rebuilt by every frame's skin.comp).
    const bool prevJoints = gpu.joints && gpu.jointsBytes > 0 && _prevJointsSource == gpu.joints &&
                            _prevJoints.size >= gpu.jointsBytes;
    if (gpu.joints && gpu.jointsBytes > _prevJoints.size) {
        // Device-local: only GPU copies fill it, and every skinned vertex
        // reads it in the main pass.
        _dev.Destroy(_prevJoints);
        _prevJoints = _dev.CreateDeviceBuffer(gpu.jointsBytes,
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    _prevJointsSource = gpu.joints;
    const bool draw = gpu.drawCount > 0 && gpu.instanceCapacity > 0;
    // Refraction reads a copy of the opaque colour and the prepass depth.
    const bool refraction = draw && frame.translucency && frame.refraction && frame.depthPrepass && gpu.refraction;
    // Translucent shadows: tint maps at a fraction of the shadow maps'
    // size (1 x 1 while unused); new ones invalidate the cached atlas.
    const bool translucentShadows = draw && frame.translucency && frame.translucentShadows &&
                                    !gpu.translucent.empty();
    const uint32_t divisor = std::clamp(frame.translucentShadowDivisor, 1u, 4u);
    if (_transparency.EnsureTintMaps(translucentShadows ? std::max(_shadowSize / divisor, 1u) : 1u, kCascades,
                                     translucentShadows ? kShadowAtlasSize / divisor : 1u)) {
        _atlasVersion = 0;
    }
    if (translucentShadows) {
        fd.features[0] |= kFeatureTranslucentShadows;
    }
    // Subsurface scattering blurs by the prepass depth.
    if (draw && frame.subsurface && frame.depthPrepass && !gpu.subsurface.empty()) {
        fd.features[0] |= kFeatureSubsurface;
    }
    _UpdateDescriptors(gpu, frame.depthPrepass, refraction ? _transparency.EnsureBackground() : VK_NULL_HANDLE);
    if (refraction) {
        fd.features[0] |= kFeatureRefraction;
        fd.translucency[0] = float(std::clamp(frame.refractionSamples, 1u, 16u));
        fd.translucency[1] = float(_transparency.BackgroundLevels());
    }
    _lastDrawCount = draw ? gpu.drawCount : 0;

    Check(vkResetCommandBuffer(_cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(_cmd, &bi), "vkBeginCommandBuffer");
    // Timestamps when everything recorded before them finished: the time
    // between two is the pass in between (`name`).
    _passes.clear();
    if (_timestamps) {
        vkCmdResetQueryPool(_cmd, _timestamps, 0, kMaxTimestamps);
        vkCmdWriteTimestamp2(_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _timestamps, 0);
    }
    auto pass = [&](const char *name) {
        if (_timestamps && _passes.size() + 1 < kMaxTimestamps) {
            _passes.push_back(name);
            vkCmdWriteTimestamp2(_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _timestamps, uint32_t(_passes.size()));
        }
    };

    const VkDeviceSize vertexOffset = 0;
    if (draw) {
        // GPU culling: reset the commands from the scene's template, then one
        // invocation per instance slot appends survivors and counts them,
        // for the camera and for each shadow cascade.
        const VkDeviceSize commandBytes = VkDeviceSize(gpu.drawCount) * sizeof(VkDrawIndexedIndirectCommand);
        VkBufferCopy resets[1 + kCascades];
        for (uint32_t k = 0; k <= kCascades; ++k) {
            resets[k] = {0, k * commandBytes, commandBytes};
        }
        vkCmdCopyBuffer(_cmd, gpu.commandTemplate, _commands.buffer, 1 + cascades, resets);
        if (prevJoints) {
            const VkBufferCopy all{0, 0, gpu.jointsBytes};
            vkCmdCopyBuffer(_cmd, gpu.joints, _prevJoints.buffer, 1, &all);
        }
        VkMemoryBarrier2 toCull{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toCull.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toCull.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toCull.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        toCull.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &toCull;
        vkCmdPipelineBarrier2(_cmd, &dep);

        // Joint matrices of all skinned instances, read by culling (bounds)
        // and every vertex stage.
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _skinPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);
        VkMemoryBarrier2 jointsBuilt{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        jointsBuilt.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        jointsBuilt.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        jointsBuilt.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        jointsBuilt.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        dep.pMemoryBarriers = &jointsBuilt;
        vkCmdPipelineBarrier2(_cmd, &dep);

        // Without frustum culling the planes stay zero (everything is inside)
        // and so does the minimum size.
        CullParams cull{};
        if (frame.frustumCulling) {
            FrustumPlanes(frame.viewProj, cull.planes);
            cull.minRadius = std::max(frame.cullPixels, 0.0f);
        }
        cull.instanceCapacity = gpu.instanceCapacity;
        cull.drawCount = gpu.drawCount;
        // Screen size of an instance: its world radius over the size of a
        // pixel at its depth (which grows linearly with depth unless ortho).
        const Mat4 inv = ToMat4(frame.invViewProj);
        const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
        const Vec3 nearCenter = TransformPoint(inv, {0, 0, -1}), mid = TransformPoint(inv, {0, 0, 0});
        const Vec3 forward = Normalize(mid - nearCenter);
        const float pixel = Length(TransformPoint(inv, {0, 2.0f / float(_height), 0}) - mid);
        cull.orthographic = std::abs(frame.viewProj[3]) + std::abs(frame.viewProj[7]) +
                            std::abs(frame.viewProj[11]) < 1e-9f ? 1.0f : 0.0f;
        cull.pixelScale = (cull.orthographic > 0 ? 1.0f : Dot(mid - eye, forward)) / std::max(pixel, 1e-30f);
        cull.nearDistance = Dot(nearCenter - eye, forward);
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(cull), &cull);
        vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);
        // Each cascade's ortho box (which spans the scene's whole depth
        // towards the light, so every caster in it stays); instances under
        // half a shadow texel are dropped.
        for (uint32_t c = 0; c < cascades; ++c) {
            FrustumPlanes(fd.cascadeViewProj[c], cull.planes);
            cull.orthographic = 1.0f;
            cull.minRadius = 0.5f;
            cull.pixelScale = 1.0f / fd.cascadeTexel[c];
            cull.listBase = (1 + c) * gpu.instanceCapacity;
            cull.commandBase = (1 + c) * gpu.drawCount;
            vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(cull), &cull);
            vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);
        }

        VkMemoryBarrier2 toDraw{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toDraw.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toDraw.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toDraw.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                              VK_PIPELINE_STAGE_2_HOST_BIT;
        toDraw.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                               VK_ACCESS_2_HOST_READ_BIT;
        dep.pMemoryBarriers = &toDraw;
        vkCmdPipelineBarrier2(_cmd, &dep);
        pass("culling");
    }
    if (localLights > 0 && frame.clusteredLights) {
        // Light clusters, read by the shading pass.
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _lightsPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdDispatch(_cmd, (16 * 9 * 24 + 63) / 64, 1, 1);
        VkMemoryBarrier2 toShading{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toShading.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toShading.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toShading.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toShading.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &toShading;
        vkCmdPipelineBarrier2(_cmd, &dep);
        pass("lightClusters");
    }
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);

    // Shadow cascades. Without a shadow light the map is only transitioned so
    // its descriptor is valid; the shader never samples it.
    const VkImageSubresourceRange shadowRange = Range(VK_IMAGE_ASPECT_DEPTH_BIT, kCascades);
    if (shadows) {
        ImageBarrier(_cmd, _shadowMap.image, shadowRange, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        for (uint32_t c = 0; c < cascades; ++c) {
            VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = _shadowLayers[c];
            att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.clearValue.depthStencil.depth = 1.0f;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {_shadowSize, _shadowSize}};
            ri.layerCount = 1;
            ri.pDepthAttachment = &att;
            vkCmdBeginRendering(_cmd, &ri);
            const VkViewport vp{0, 0, float(_shadowSize), float(_shadowSize), 0, 1};
            const VkRect2D scissor{{0, 0}, {_shadowSize, _shadowSize}};
            vkCmdSetViewport(_cmd, 0, 1, &vp);
            vkCmdSetScissor(_cmd, 0, 1, &scissor);
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline[0]);
            vkCmdSetDepthBias(_cmd, 0.0f, 0.0f, 1.5f);
            PushDraw(_cmd, _layout, fd.cascadeViewProj[c], (1 + c) * gpu.instanceCapacity);
            vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
            vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
            DrawChunks(_cmd, gpu, _commands.buffer,
                       VkDeviceSize(1 + c) * gpu.drawCount * sizeof(VkDrawIndexedIndirectCommand),
                       _shadowPipeline, _shadowCutoutPipeline, 3);
            vkCmdEndRendering(_cmd);
        }
        // Translucent casters into the sun's tint map, cascade by cascade.
        if (translucentShadows) {
            const Transparency::TintMap &map = _transparency.SunTint();
            _transparency.BeginTint(_cmd, map, kCascades);
            for (uint32_t c = 0; c < cascades; ++c) {
                VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                color.imageView = map.colorLayers[c];
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfo depth = color;
                depth.imageView = map.depthLayers[c];
                depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                VkRenderingInfo tri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                tri.renderArea = {{0, 0}, {map.size, map.size}};
                tri.layerCount = 1;
                tri.colorAttachmentCount = 1;
                tri.pColorAttachments = &color;
                tri.pDepthAttachment = &depth;
                vkCmdBeginRendering(_cmd, &tri);
                const VkViewport tvp{0, 0, float(map.size), float(map.size), 0, 1};
                vkCmdSetViewport(_cmd, 0, 1, &tvp);
                vkCmdSetScissor(_cmd, 0, 1, &tri.renderArea);
                PushDraw(_cmd, _layout, fd.cascadeViewProj[c], (1 + c) * gpu.instanceCapacity);
                _DrawTintCasters(gpu, VkDeviceSize(1 + c) * gpu.drawCount * sizeof(VkDrawIndexedIndirectCommand));
                vkCmdEndRendering(_cmd);
            }
            _transparency.EndTint(_cmd, map, kCascades);
        }
        ImageBarrier(_cmd, _shadowMap.image, shadowRange,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        pass("sunShadows");
    } else {
        ImageBarrier(_cmd, _shadowMap.image, shadowRange, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // Depth prepass: the main pass then shades each pixel once, and GTAO
    // reads the finished depth before it. From here on depth is read-only.
    // Without it (Frame::depthPrepass) the main pass writes depth itself.
    constexpr VkPipelineStageFlags2 kDepthTests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                 VK_PIPELINE_STAGE_2_NONE, 0, kDepthTests,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = _depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil.depth = frame.clearDepth;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {_width, _height}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depthAtt;
    // No Y flip: NDC y=-1 lands in memory row 0, which is the bottom row
    // in GL/Hydra order, so readback needs no flip either.
    const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
    const VkRect2D scissor{{0, 0}, {_width, _height}};
    if (frame.depthPrepass) {
        vkCmdBeginRendering(_cmd, &ri);
        vkCmdSetViewport(_cmd, 0, 1, &vp);
        vkCmdSetScissor(_cmd, 0, 1, &scissor);
        if (draw) {
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _prepassPipeline[0]);
            PushDraw(_cmd, _layout, jitteredViewProj, 0);
            vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
            vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
            DrawChunks(_cmd, gpu, _commands.buffer, 0, _prepassPipeline, _prepassCutoutPipeline, 3);
        }
        vkCmdEndRendering(_cmd);
        ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                     kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     kDepthTests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        pass("depthPrepass");
    }

    // AO only darkens the dome lighting, so without a dome it is skipped.
    if (frame.ambientOcclusion && frame.depthPrepass && draw && ibl) {
        _gtao.Record(_cmd, fd.invViewProj, frame.cameraPos, frame.aoRadius, sample,
                     std::clamp(frame.aoSlices, 1u, 8u));
        pass("ambientOcclusion");
    } else {
        _gtao.Clear(_cmd);
    }

    _RenderLocalShadows(gpu, draw ? shadowTiles : std::vector<ShadowTile>(), translucentShadows);
    if (!shadowTiles.empty()) {
        pass("localShadows");
    }

    const Image *colorTargets[3] = {&_color, &_primId, &_instanceId};
    for (const Image *img : colorTargets) {
        ImageBarrier(_cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    ImageBarrier(_cmd, _velocity.image, Range(VK_IMAGE_ASPECT_COLOR_BIT), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo colorAtts[4] = {};
    for (int i = 0; i < 3; ++i) {
        colorAtts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtts[i].imageView = colorTargets[i]->view;
        colorAtts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtts[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::fill(colorAtts[i].clearValue.color.int32, colorAtts[i].clearValue.color.int32 + 4, -1);
    }
    // With post on, the scene is premultiplied by coverage (cleared to
    // transparent) and the tonemap composites the app's colour behind it.
    const float transparent[4] = {0, 0, 0, 0};
    std::memcpy(colorAtts[0].clearValue.color.float32, frame.postProcess ? transparent : frame.clearColor,
                sizeof(frame.clearColor));
    colorAtts[3].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtts[3].imageView = _velocity.view;
    colorAtts[3].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtts[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtts[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // clearValue zero: no object motion where nothing is drawn.
    if (frame.depthPrepass) {
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    }
    ri.colorAttachmentCount = 4;
    ri.pColorAttachments = colorAtts;
    vkCmdBeginRendering(_cmd, &ri);
    vkCmdSetViewport(_cmd, 0, 1, &vp);
    vkCmdSetScissor(_cmd, 0, 1, &scissor);
    // GTAO bound its own set; the sky and shading need the scene's again.
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    // Motion vectors: 1 with last frame's joints, 2 without, 0 none (mesh.vert).
    const uint32_t motion = !frame.objectMotion ? 0 : prevJoints ? 1 : 2;
    vkCmdSetDepthWriteEnable(_cmd, frame.depthPrepass ? VK_FALSE : VK_TRUE);
    if (draw) {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline[0]);
        PushDraw(_cmd, _layout, jitteredViewProj, 0, motion);
        vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
        vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
        DrawChunks(_cmd, gpu, _commands.buffer, 0, _pipeline, _pipeline, 15);
    }
    vkCmdSetDepthWriteEnable(_cmd, VK_FALSE);
    if (sky) {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skyPipeline);
        vkCmdDraw(_cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(_cmd);
    pass("shading");
    if (fd.features[0] & kFeatureSubsurface) {
        _RenderSubsurface(gpu, frame, jitteredViewProj, fd.invViewProj, depthAtt);
        pass("subsurface");
    }
    if (draw && frame.translucency && !gpu.translucent.empty()) {
        _RenderTranslucency(gpu, frame, &fd, jitteredViewProj, motion, depthAtt);
        pass("translucency");
    }
    if (!frame.depthPrepass) {
        ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                     kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    }
    ImageBarrier(_cmd, _velocity.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    constexpr VkPipelineStageFlags2 kPostStages =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    _adapting = frame.postProcess && frame.post.autoExposure;
    // The camera's lens: depth of field when it has an fStop; motion blur
    // with an open shutter when the camera or the scene moved. That frame
    // is final as it is: still frames after it would only average the blur
    // away (usdrecord renders until converged).
    const bool dof = frame.postProcess && frame.depthOfField && frame.fStop > 0 && frame.focalLength > 0 &&
                     frame.sensorWidth > 0 && frame.focusDistance > frame.focalLength;
    const bool cameraMoved = _historyValid && std::memcmp(frame.viewProj, _prevViewProj, sizeof(_prevViewProj)) != 0;
    const bool motionBlur = frame.postProcess && frame.motionBlur && frame.shutter > 0 && _historyValid &&
                            (cameraMoved || gpu.moved);
    Lens::Params lens{};
    if (dof || motionBlur) {
        const Mat4 back = motionBlur ? ToMat4(frame.invViewProj) * ToMat4(_prevViewProj) : Mat4();
        std::memcpy(lens.reprojection, &back.m[0][0], sizeof(lens.reprojection));
        std::copy(frame.depthToDistance, frame.depthToDistance + 4, lens.depthToDistance);
        if (dof) {
            // Thin lens: a circle of confusion f^2 / (N (S - f)) * |D - S| / D
            // across on the sensor, as a radius in pixels.
            const float f = frame.focalLength, s = frame.focusDistance;
            lens.cocScale = 0.5f * f * f / (frame.fStop * (s - f)) * float(frame.width) / frame.sensorWidth;
            lens.focusDistance = s;
        }
        // Blurs reach at most a tile (32 pixels) beyond the 3x3 tiles' max.
        lens.maxRadius = std::min(0.04f * float(frame.width), 32.0f);
        lens.frame = float(_haltonIndex % 64);
        lens.shutter = motionBlur ? frame.shutter : 0.0f;
        lens.maxHalfBlur = std::min(0.025f * float(frame.width), 32.0f);
        _lens.RecordTiles(_cmd, lens);
    }
    if (dof) {
        _lens.RecordDepthOfField(_cmd, lens);
        pass("depthOfField");
    }
    if (frame.postProcess) {
        // HDR color feeds TAA and post (TAA also reads the read-only depth).
        ImageBarrier(_cmd, _color.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     kPostStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // TAA weight: 1 without usable history; 1/(n+1) over n history
        // frames, capped at kTaaMoving while the camera moves (0.1) and at
        // kTaaSamples - 1 while it is still (a 16-sample average).
        float alpha = 1.0f;
        Mat4 reprojection;
        if (taa) {
            const bool moved = _taaValid && std::memcmp(frame.viewProj, _prevViewProj, sizeof(_prevViewProj)) != 0;
            if (_taaValid) {
                reprojection = unjitter * ToMat4(frame.invViewProj) * ToMat4(_prevViewProj);
            }
            if (!_taaValid) {
                _taaFrames = 0;
                _taaWarmup = true;
            } else if (moved) {
                _taaFrames = std::min(_taaFrames, kTaaMoving);
                _taaWarmup = false;
            }
            alpha = 1.0f / float(_taaFrames + 1);
            _taaConverged = _taaValid && !moved && _taaFrames >= taaSamples - 1;  // last sample of a still camera
            _taaFrames = std::min(_taaFrames + 1, taaSamples - 1);
            _taaValid = true;
        } else {
            _taaValid = false;
            _taaWarmup = false;
            _taaConverged = true;
        }

        // Adaptation runs on wall-clock time. The first frame, toggling
        // auto-exposure and a fresh TAA history snap straight to the target:
        // while TAA accumulates a new image its histogram still shifts, and
        // the settled image should be exposed for itself.
        const auto now = std::chrono::steady_clock::now();
        const float dt = _hasLastFrame
            ? std::min(std::chrono::duration<float>(now - _lastFrame).count(), 0.1f) : 0.0f;
        const bool reset = !_hasLastFrame || frame.post.autoExposure != _lastAutoExposure || _taaWarmup;
        _lastFrame = now;
        _hasLastFrame = true;
        _lastAutoExposure = frame.post.autoExposure;
        if (_taaConverged) {
            _taaWarmup = false;
        }

        _taa.Record(_cmd, &reprojection.m[0][0], alpha);
        pass("taa");
        if (motionBlur) {
            _lens.RecordMotionBlur(_cmd, lens);
            _taaConverged = true;
            pass("motionBlur");
        }
        _post.Record(_cmd, frame.post, dt, reset, frame.clearColor);
        pass("post");
        _stats.taaSamples = uint32_t(1.0f / alpha + 0.5f);
        _stats.taaTarget = taa ? taaSamples : 1;
        std::memcpy(_prevViewProj, frame.viewProj, sizeof(_prevViewProj));
        _historyValid = true;
    } else {
        _taaValid = false;
        _taaConverged = true;
        _historyValid = false;
        _stats.taaSamples = _stats.taaTarget = 1;
    }

    // Everything that may be read back goes to TRANSFER_SRC (with post on,
    // color comes from the post output, already in TRANSFER_SRC).
    for (const Image *img : colorTargets) {
        if (img == &_color && frame.postProcess) {
            continue;
        }
        ImageBarrier(_cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                 kDepthTests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(_cmd, _velocity.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // Copy only what the caller asked for, packed one after another.
    struct Output {
        VkImage image;
        VkImageAspectFlags aspect;
        VkDeviceSize bytesPerPixel;
        void *dst;
        VkDeviceSize offset;
    };
    Output outputs[5] = {
        {frame.postProcess ? _post.Output() : _color.image, VK_IMAGE_ASPECT_COLOR_BIT, 8, out.color, 0},
        {_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 4, out.depth, 0},
        {_primId.image, VK_IMAGE_ASPECT_COLOR_BIT, 4, out.primId, 0},
        {_instanceId.image, VK_IMAGE_ASPECT_COLOR_BIT, 4, out.instanceId, 0},
        {_velocity.image, VK_IMAGE_ASPECT_COLOR_BIT, 4, out.velocity, 0},
    };
    VkDeviceSize offset = 0;
    for (Output &o : outputs) {
        if (!o.dst) {
            continue;
        }
        o.offset = offset;
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {o.aspect, 0, 0, 1};
        copy.imageExtent = {_width, _height, 1};
        vkCmdCopyImageToBuffer(_cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               _readback.buffer, 1, &copy);
        offset += o.bytesPerPixel * _width * _height;
    }

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
    pass("readback");

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
    for (const Output &o : outputs) {
        if (o.dst) {
            std::memcpy(o.dst, src + o.offset, o.bytesPerPixel * _width * _height);
        }
    }

    // What the camera drew: the culled commands' instances and triangles.
    const auto *commands = static_cast<const VkDrawIndexedIndirectCommand *>(_commands.mapped);
    _stats.instances = 0;
    _stats.triangles = 0;
    for (uint32_t i = 0; i < _lastDrawCount; ++i) {
        _stats.instances += commands[i].instanceCount;
        _stats.triangles += uint64_t(commands[i].instanceCount) * (commands[i].indexCount / 3);
    }
    _stats.drawCalls = 0;
    for (size_t i = 0; draw && i + 1 < gpu.drawChunks.size(); ++i) {
        _stats.drawCalls += (gpu.chunkKind[i] & (Scene::Gpu::kWide - 1)) != Scene::Gpu::kTranslucent;
    }
    _stats.drawCalls += draw ? uint32_t(gpu.translucent.size()) : 0;
    _stats.exposure = frame.postProcess ? _post.Exposure() : 1.0;
    _stats.passMs.clear();
    _stats.gpuMs = 0;
    if (_timestamps && !_passes.empty()) {
        std::vector<uint64_t> ticks(_passes.size() + 1);
        if (vkGetQueryPoolResults(_dev.device, _timestamps, 0, uint32_t(ticks.size()), ticks.size() * sizeof(uint64_t),
                                  ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (size_t i = 0; i < _passes.size(); ++i) {
                _stats.passMs.push_back({_passes[i], double(ticks[i + 1] - ticks[i]) * _timestampMs});
            }
            _stats.gpuMs = double(ticks.back() - ticks.front()) * _timestampMs;
        }
    }
    _stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
}

} // namespace vkr
