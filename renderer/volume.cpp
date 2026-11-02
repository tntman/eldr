#include "volume.h"

#include "fullscreen.h"
#include "renderer.h"
#include "scene.h"
#include "transparency.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

#define PNANOVDB_C
#include "PNanoVDB.h"

namespace vkr {

namespace {

const uint32_t kVolumeFrag[] =
#include "volume.frag.inc"
;
const uint32_t kShapesFrag[] =
#include "light_shapes.frag.inc"
;
const uint32_t kHalfFrag[] =  // volume.frag with HALF
#include "volume_half.frag.inc"
;
const uint32_t kUpsampleFrag[] =
#include "volume_upsample.frag.inc"
;
const uint32_t kShadowFrag[] =
#include "volume_shadow.frag.inc"
;
const uint32_t kGridComp[] =
#include "volume_grid.comp.inc"
;

constexpr VkFormat kHalfFormat = VK_FORMAT_R32G32B32A32_UINT;  // half floats: radiance, coloured transmittance

struct VolumeParams {  // mirror of the push block in volume.frag
    float stepSize;
    uint32_t maxSteps, shadowSteps;
    float density, emission, anisotropy;
    uint32_t count;
    uint32_t scale;  // 1: full resolution, 2: half
    uint32_t part;   // VolumePass::Part
    uint32_t cascade, mapSize;  // RecordShadow's
    uint32_t originX, originY;
    uint32_t mutual;  // Settings::mutualShadows
    uint32_t defaultFields;  // Settings::defaultFields
    float volumeLimit;       // Settings::volumeLimit: volumes without their own (GpuVolume::medium[3])
    int32_t shadowLink;      // volume_shadow.frag: the light's shadow link bit (-1: every volume casts)
    uint32_t localShadowStride;      // Settings::localShadowStride
    uint32_t gridSize, gridLights;   // the shadow grid's (VolumePass::EnsureShadowGrid), 0: none
};

struct GridParams {  // mirror of the push block in volume_grid.comp
    uint32_t count, size, lights, mutual;
    float density;
    uint32_t tables;
};

// Shadow grids: at most this many cells (8 bytes each) and lights.
constexpr uint64_t kGridCells = uint64_t(8) << 20;
constexpr uint32_t kGridMaxLights = 16;

void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
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

// A shadow grid image (3D, RGBA16F), in GENERAL from here on (storage and sampled alike), cleared.
Image CreateGrid(Device &dev, const uint32_t extent[3])
{
    constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    Image img;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = kFormat;
    ci.extent = {extent[0], extent[1], extent[2]};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Check(vkCreateImage(dev.device, &ci, nullptr, &img.image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev.device, img.image, &req);
    img.memory = dev.Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindImageMemory(dev.device, img.image, img.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = kFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Check(vkCreateImageView(dev.device, &vci, nullptr, &img.view), "vkCreateImageView");
    dev.Submit([&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ImageBarrier(cmd, img.image, range, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        const VkClearColorValue clear{};
        vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        Barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    return img;
}

VolumeParams Params(const VolumePass::Settings &s, uint32_t count)
{
    VolumeParams p{};
    p.stepSize = std::max(s.stepSize, 0.05f);
    p.maxSteps = std::clamp(s.maxSteps, 1u, 4096u);
    p.shadowSteps = std::min(s.shadowSteps, 256u);
    p.density = std::max(s.density, 0.0f);
    p.emission = std::max(s.emission, 0.0f);
    p.anisotropy = std::clamp(s.anisotropy, -0.95f, 0.95f);
    p.count = count;
    p.mutual = s.mutualShadows;
    p.defaultFields = s.defaultFields;
    p.localShadowStride = std::clamp(s.localShadowStride, 1u, 16u);
    p.volumeLimit = std::max(s.volumeLimit, 0.0f);
    return p;
}

// y = m (x, 1): three rows of an affine map.
struct Affine {
    double m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
};

Affine operator*(const Affine &a, const Affine &b)  // b first
{
    Affine r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0);
        }
    }
    return r;
}

bool Invert(const Affine &a, Affine *out)
{
    const auto &m = a.m;
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!(std::abs(det) > 1e-300)) {
        return false;
    }
    Affine r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {  // adjugate: cofactor of (j, i)
            const int r0 = (j + 1) % 3, r1 = (j + 2) % 3, c0 = (i + 1) % 3, c1 = (i + 2) % 3;
            r.m[i][j] = (m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]) / det;
        }
    }
    for (int i = 0; i < 3; ++i) {
        r.m[i][3] = -(r.m[i][0] * m[0][3] + r.m[i][1] * m[1][3] + r.m[i][2] * m[2][3]);
    }
    *out = r;
    return true;
}

void Store(const Affine &a, float rows[3][4])
{
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            rows[i][j] = float(a.m[i][j]);
        }
    }
}

// A grid's bytes as PNanoVDB reads them on the CPU.
struct Nano {
    pnanovdb_buf_t buf;
    pnanovdb_grid_handle_t grid;
    pnanovdb_root_handle_t root;
    uint32_t type = 0;  // PNANOVDB_GRID_TYPE_*; 0: not a grid we read

    explicit Nano(const VolumeGrid *g)
    {
        buf = pnanovdb_make_buf(g ? const_cast<uint32_t *>(g->words.data()) : nullptr, g ? g->words.size() : 0);
        grid.address = pnanovdb_address_null();
        constexpr size_t kHeaderWords = (PNANOVDB_GRID_SIZE + PNANOVDB_TREE_SIZE) / 4;
        if (!g || g->words.size() < kHeaderWords || pnanovdb_grid_get_magic(buf, grid) != PNANOVDB_MAGIC_NUMBER ||
            pnanovdb_grid_get_grid_size(buf, grid) > g->words.size() * 4) {
            return;
        }
        root = pnanovdb_tree_get_root(buf, pnanovdb_grid_get_tree(buf, grid));
        const uint32_t t = pnanovdb_grid_get_grid_type(buf, grid);
        type = t == PNANOVDB_GRID_TYPE_FLOAT || t == PNANOVDB_GRID_TYPE_VEC3F ? t : 0;
    }

    Affine IndexToObject() const
    {
        const pnanovdb_map_handle_t map = pnanovdb_grid_get_map(buf, grid);
        Affine a;
        for (uint32_t i = 0; i < 3; ++i) {
            for (uint32_t j = 0; j < 3; ++j) {
                a.m[i][j] = pnanovdb_map_get_matd(buf, map, i * 3 + j);
            }
            a.m[i][3] = pnanovdb_map_get_vecd(buf, map, i);
        }
        return a;
    }
};

} // namespace

VolumePass::VolumePass(Device &device, VkDescriptorSetLayout sceneLayout)
    : _dev(device)
{
    const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VolumeParams)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &sceneLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    Check(vkCreatePipelineLayout(_dev.device, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};  // filters nothing: integer texels
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_halfSampler), "vkCreateSampler");
    const VkPushConstantRange gridRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GridParams)};
    lci.pPushConstantRanges = &gridRange;
    Check(vkCreatePipelineLayout(_dev.device, &lci, nullptr, &_gridLayout), "vkCreatePipelineLayout");
    _gridExtent[0] = _gridExtent[1] = _gridExtent[2] = 1;  // bound before any volume
    _grid = CreateGrid(_dev, _gridExtent);
}

// When a volume or light shape first draws: a scene without them doesn't
// compile them (seconds with a cold driver cache).
void VolumePass::_EnsurePipelines()
{
    if (_pipeline[0]) {
        return;
    }
    for (uint32_t factors = 0; factors < 2; ++factors) {
        const VkSpecializationMapEntry entry{0, 0, sizeof(uint32_t)};  // kFactors
        const VkSpecializationInfo spec{1, &entry, sizeof(factors), &factors};
        _pipeline[factors] = CreateFullscreenPipeline(_dev, _layout, kVolumeFrag, sizeof(kVolumeFrag),
                                                      Renderer::kColorFormat, false, true, false, VK_FORMAT_UNDEFINED,
                                                      true, &spec);
        _halfPipeline[factors] = CreateFullscreenPipeline(_dev, _layout, kHalfFrag, sizeof(kHalfFrag), kHalfFormat,
                                                          false, false, false, VK_FORMAT_UNDEFINED, false, &spec);
    }
    _upsamplePipeline = CreateFullscreenPipeline(_dev, _layout, kUpsampleFrag, sizeof(kUpsampleFrag),
                                                 Renderer::kColorFormat, false, true, false, VK_FORMAT_UNDEFINED, true);
    _shapePipeline = CreateFullscreenPipeline(_dev, _layout, kShapesFrag, sizeof(kShapesFrag), Renderer::kColorFormat,
                                              false, true);
    _shadowPipeline = CreateFullscreenPipeline(_dev, _layout, kShadowFrag, sizeof(kShadowFrag),
                                               Transparency::kTintFormat, false, false, true,
                                               Transparency::kTintDepthFormat);
    VkShaderModule module = CreateShaderModule(_dev.device, kGridComp, sizeof(kGridComp));
    VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module,
                 "main"};
    cci.layout = _gridLayout;
    const VkResult r = vkCreateComputePipelines(_dev.device, _dev.pipelineCache, 1, &cci, nullptr, &_gridPipeline);
    vkDestroyShaderModule(_dev.device, module, nullptr);
    Check(r, "vkCreateComputePipelines");
}

VolumePass::~VolumePass()
{
    vkDeviceWaitIdle(_dev.device);
    for (VkPipeline p : {_pipeline[0], _pipeline[1], _halfPipeline[0], _halfPipeline[1], _upsamplePipeline,
                         _shadowPipeline, _shapePipeline, _gridPipeline}) {
        vkDestroyPipeline(_dev.device, p, nullptr);
    }
    vkDestroyPipelineLayout(_dev.device, _layout, nullptr);
    vkDestroyPipelineLayout(_dev.device, _gridLayout, nullptr);
    _dev.Destroy(_grid);
    vkDestroySampler(_dev.device, _halfSampler, nullptr);
    _dev.Destroy(_half);
    _DestroyLayers();
}

void VolumePass::_DestroyLayers()
{
    for (VkImageView &v : _layerViews) {
        vkDestroyImageView(_dev.device, v, nullptr);
        v = VK_NULL_HANDLE;
    }
    _dev.Destroy(_translucentDepth);
    _dev.Destroy(_peelDepth);
    _dev.Destroy(_layers);
    _layerCount = 0;
}

void VolumePass::EnsureTranslucentLayers(uint32_t width, uint32_t height, uint32_t layers)
{
    layers = std::clamp(layers, 1u, kMaxLayers);
    if (width == _depthWidth && height == _depthHeight && layers == _layerCount) {
        return;
    }
    _DestroyLayers();
    _translucentDepth = _dev.CreateImage(VK_FORMAT_D32_SFLOAT, width, height, 1, 1,
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                         VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _peelDepth = _dev.CreateImage(VK_FORMAT_D32_SFLOAT, width, height, 1, 1,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _layers = _dev.CreateImage(kLayerFormat, width, height, layers, 1,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    for (uint32_t layer = 0; layer < layers; ++layer) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.image = _layers.image;
        vci.format = kLayerFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
        Check(vkCreateImageView(_dev.device, &vci, nullptr, &_layerViews[layer]), "vkCreateImageView");
    }
    _depthWidth = width;
    _depthHeight = height;
    _layerCount = layers;
}

VkImageView VolumePass::EnsureHalf(uint32_t width, uint32_t height)
{
    const uint32_t w = (width + 1) / 2, h = (height + 1) / 2;
    if (w != _halfWidth || h != _halfHeight) {
        _dev.Destroy(_half);
        _half = _dev.CreateImage(kHalfFormat, w, h, 1, 1,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
        _halfWidth = w;
        _halfHeight = h;
    }
    return _half.view;
}

bool VolumePass::Record(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, VkImageView color,
                        uint32_t width, uint32_t height, const Settings &s, Part part, bool factors)
{
    if (volumeCount == 0) {
        return false;
    }
    _EnsurePipelines();
    // The shading pass's colour writes land before the blend reads them.
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    StorageBarrier(cmd);  // the light splits volume.frag attenuates (aov.h)

    const bool grid = _gridSize > 0 && _gridCount == volumeCount;
    if (grid && _gridBuilt != _gridWanted) {
        _BuildShadowGrid(cmd, set);
    }
    VolumeParams p = Params(s, volumeCount);
    p.part = part;
    p.scale = s.resolution >= 2 && _half.image ? 2 : 1;
    p.gridSize = grid ? _gridSize : 0;
    p.gridLights = grid ? _gridLights : 0;
    if (p.scale == 1) {
        DrawFullscreen(cmd, _pipeline[factors], _layout, set, color, width, height, true, VK_SHADER_STAGE_FRAGMENT_BIT,
                       &p, sizeof(p));
        return true;
    }
    // Half resolution: every pixel of the target is written (zero where
    // no volume is), then upsampled over the image.
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ImageBarrier(cmd, _half.image, range, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    DrawFullscreen(cmd, _halfPipeline[factors], _layout, set, _half.view, _halfWidth, _halfHeight, false,
                   VK_SHADER_STAGE_FRAGMENT_BIT, &p, sizeof(p));
    ImageBarrier(cmd, _half.image, range, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    DrawFullscreen(cmd, _upsamplePipeline, _layout, set, color, width, height, true, VK_SHADER_STAGE_FRAGMENT_BIT,
                   &p, sizeof(p));
    return true;
}

void VolumePass::EnsureShadowGrid(uint32_t count, uint32_t distant, uint32_t local, bool dome, const Settings &s,
                                  bool tables, uint64_t version)
{
    const bool mutual = s.mutualShadows && count > 1;
    const uint32_t lights = 1 + std::min(distant + (mutual ? local : 0), kGridMaxLights);
    const uint64_t slots = uint64_t(count) * lights;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(_dev.physical, &props);
    uint32_t size = s.shadowSteps > 0 && count > 0 ? std::min(s.shadowGrid, 128u) : 0;
    while (size > 0 && (uint64_t(size) * size * size * slots > kGridCells ||
                        uint64_t(size) * slots > props.limits.maxImageDimension3D)) {
        --size;
    }
    size = size >= 8 ? size : 0;  // coarser than that: march
    const uint32_t extent[3] = {std::max(size, 1u), std::max(size, 1u), std::max(uint32_t(size * slots), 1u)};
    if (!std::equal(extent, extent + 3, _gridExtent)) {
        _dev.Destroy(_grid);
        _grid = CreateGrid(_dev, extent);
        std::copy(extent, extent + 3, _gridExtent);
        _gridBuilt = 0;
    }
    _gridSize = size;
    _gridLights = lights;
    _gridCount = count;
    _gridMutual = mutual ? 1 : 0;
    _gridTables = tables ? 1 : 0;
    _gridDensity = std::max(s.density, 0.0f);
    // What the grid holds (FNV-1a); never 0 (_gridBuilt's "nothing").
    uint64_t hash = 1469598103934665603ull;
    float density = _gridDensity;
    uint32_t bits;
    std::memcpy(&bits, &density, 4);
    for (uint64_t v : {version, uint64_t(size), uint64_t(lights), uint64_t(count), uint64_t(_gridMutual),
                       uint64_t(_gridTables), uint64_t(bits), uint64_t(dome), uint64_t(distant), uint64_t(local)}) {
        hash = (hash ^ v) * 1099511628211ull;
    }
    _gridWanted = hash | 1;
}

void VolumePass::_BuildShadowGrid(VkCommandBuffer cmd, VkDescriptorSet set)
{
    _EnsurePipelines();
    // After last frame's march read it.
    Barrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const GridParams p{_gridCount, _gridSize, _gridLights, _gridMutual, _gridDensity, _gridTables};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gridPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _gridLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, _gridLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, (_gridExtent[0] + 3) / 4, (_gridExtent[1] + 3) / 4, (_gridExtent[2] + 3) / 4);
    Barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    _gridBuilt = _gridWanted;
}

void VolumePass::RecordLightShapes(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t first, uint32_t count,
                                   VkImageView color, uint32_t width, uint32_t height)
{
    _EnsurePipelines();
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    const uint32_t push[2] = {first, count};
    DrawFullscreen(cmd, _shapePipeline, _layout, set, color, width, height, true, VK_SHADER_STAGE_FRAGMENT_BIT, push,
                   sizeof(push));
}

void VolumePass::RecordShadow(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, uint32_t cascade,
                              uint32_t mapSize, const Settings &s, int32_t shadowLink)
{
    _EnsurePipelines();
    VolumeParams p = Params(s, volumeCount);
    p.cascade = cascade;
    p.mapSize = mapSize;
    p.shadowLink = shadowLink;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, _layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void VolumePass::RecordTileShadow(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, uint32_t tile,
                                  uint32_t x, uint32_t y, uint32_t size, const Settings &s, int32_t shadowLink)
{
    _EnsurePipelines();
    VolumeParams p = Params(s, volumeCount);
    p.part = 1;
    p.cascade = tile;
    p.mapSize = size;
    p.originX = x;
    p.originY = y;
    p.shadowLink = shadowLink;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, _layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// Builds the volume table (a GpuVolume per drawn instance) and, when the
// grids they read or their versions changed, the grid pool.
// ponytail: the whole pool is uploaded again whenever any grid changes; keep
// grids in sub-allocated ranges if animated volumes share scenes with large
// static ones.
void Scene::_CommitVolumes()
{
    // Each drawn volume's shading and its fields by role: density,
    // temperature, colour, emission.
    struct Drawn {
        const Volume *volume;
        VolumeShading shading;
        std::shared_ptr<VolumeGrid> grids[4];
        uint32_t roles = 0;       // GpuVolume::factorMasks[3]
        uint32_t slots[3] = {4, 4, 4};  // each factor's grid; 4: none
        uint32_t lutOffsets[4];         // GpuVolume's, from the first table (the extinction's: floats)
        float lutLow[4] = {}, lutScale[4] = {};
        std::shared_ptr<VolumeGrid> velocity[3];  // a Vec3f grid, or three float grids
        uint32_t velocityMode = 0;                // GpuVolume::velocity[3]
    };
    std::vector<Drawn> drawn;
    std::vector<std::pair<std::shared_ptr<VolumeGrid>, uint64_t>> used;
    for (const auto &v : _volumes) {
        if (!v->visible || !(v->tag & _tagMask)) {
            continue;
        }
        Drawn d{v.get(), v->shading ? *v->shading : VolumeShading{}, {}};
        auto field = [&](const std::string &name) {
            const auto it = name.empty() ? v->fields.end() : v->fields.find(name);
            return it == v->fields.end() ? nullptr : it->second;
        };
        // Velocity blur: vel, else v; a Vec3f grid or three float grids
        // (Houdini's vector volumes: vel.x, vel.y, vel.z; USD names _x ...).
        for (const std::string name : {"vel", "v"}) {
            if (v->velocityScale == 0.0f || d.velocityMode != 0) {
                break;
            }
            if (const auto g = field(name); Nano(g.get()).type == PNANOVDB_GRID_TYPE_VEC3F) {
                d.velocity[0] = g;
                d.velocityMode = 1;
            }
            for (const char *dot : {".", "_"}) {
                std::shared_ptr<VolumeGrid> c[3];
                int found = 0;
                for (int k = 0; k < 3 && d.velocityMode == 0; ++k) {
                    c[k] = field(name + dot + "xyz"[k]);
                    found += Nano(c[k].get()).type == PNANOVDB_GRID_TYPE_FLOAT;
                }
                if (found == 3) {
                    std::copy(c, c + 3, d.velocity);
                    d.velocityMode = 3;
                }
            }
        }
        d.grids[0] = field(d.shading.densityField);
        // Without it: a nameless field (field_N: what SOP Import names an
        // unnamed volume), else the first float field that plays no other
        // role (nor is a velocity).
        for (int pass = 0; pass < 2 && !d.grids[0]; ++pass) {
            for (const auto &[name, g] : v->fields) {
                const bool nameless = name.rfind("field_", 0) == 0;
                const bool role = name == d.shading.temperatureField || name == d.shading.colorField ||
                                  name == d.shading.emissionField || name.rfind("vel", 0) == 0 || name == "v";
                if ((pass == 0 ? nameless : !role) && Nano(g.get()).type == PNANOVDB_GRID_TYPE_FLOAT) {
                    d.grids[0] = g;
                    break;
                }
            }
        }
        d.grids[1] = field(d.shading.temperatureField);
        d.grids[2] = field(d.shading.colorField);
        if (!d.grids[2] && d.shading.colorField == "Cd") {
            d.grids[2] = field("color");
        }
        d.grids[3] = field(d.shading.emissionField);
        if (Nano(d.grids[0].get()).type != PNANOVDB_GRID_TYPE_FLOAT) {
            continue;
        }
        for (int k = 1; k < 4; ++k) {
            d.roles |= d.grids[k] ? 1u << (k - 1) : 0u;
        }
        // Each factor's field: the density's or a role's grid, else a free
        // role slot (its role stays off). Missing: the factor of 0.
        // ponytail: with all slots taken a factor reads 0; slots per factor if that bites.
        for (size_t k = 0; k < d.shading.factors.size() && k < VolumeShading::kMaxFactors; ++k) {
            const std::shared_ptr<VolumeGrid> g = field(d.shading.factors[k].field);
            uint32_t slot = 0;
            while (g && slot < 4 && d.grids[slot] != g) {
                ++slot;
            }
            for (uint32_t free = 1; g && slot == 4 && free < 4; ++free) {
                if (!d.grids[free]) {
                    d.grids[free] = g;
                    slot = free;
                }
            }
            d.slots[k] = g ? slot : 4;
        }
        for (const auto &g : {d.grids[0], d.grids[1], d.grids[2], d.grids[3], d.velocity[0], d.velocity[1],
                              d.velocity[2]}) {
            if (Nano(g.get()).type != 0 &&
                std::none_of(used.begin(), used.end(), [&](const auto &u) { return u.first == g; })) {
                used.push_back({g, g->version});
            }
        }
        drawn.push_back(std::move(d));
    }
    // The factors' tables (after the grids in the pool): kVolumeLut values
    // over the field's range (0 included, as empty space reads 0), linear
    // in between. With factors of the density's grid in scattering or
    // absorption (or neither per unit of density), a table of the mean
    // extinction over the density's range too (per channel and their mean,
    // 4 floats an entry): the shadow marches step it.
    std::vector<float> luts;
    for (Drawn &d : drawn) {
        const VolumeShading &s = d.shading;
        auto range = [&](uint32_t slot, float *low, float *scale) {
            const Nano n(slot < 4 ? d.grids[slot].get() : nullptr);
            float lo = 0.0f, hi = 0.0f;
            if (n.type == PNANOVDB_GRID_TYPE_FLOAT) {
                lo = std::min(lo, pnanovdb_read_float(n.buf, pnanovdb_root_get_min_address(n.type, n.buf, n.root)));
                hi = std::max(hi, pnanovdb_read_float(n.buf, pnanovdb_root_get_max_address(n.type, n.buf, n.root)));
            }
            *low = lo;
            *scale = hi > lo ? float(kVolumeLut - 1) / (hi - lo) : 0.0f;  // 0: one value, f(0)
        };
        auto at = [&](float low, float scale, uint32_t i) { return scale > 0 ? low + float(i) / scale : low; };
        std::fill(d.lutOffsets, d.lutOffsets + 4, kNone);
        for (size_t k = 0; k < s.factors.size() && k < VolumeShading::kMaxFactors; ++k) {
            range(d.slots[k], &d.lutLow[k], &d.lutScale[k]);
            d.lutOffsets[k] = uint32_t(luts.size());
            for (uint32_t i = 0; i < kVolumeLut; ++i) {
                const std::array<float, 3> v = s.factors[k].value(at(d.lutLow[k], d.lutScale[k], i));
                luts.insert(luts.end(), {v[0], v[1], v[2], 0.0f});
            }
        }
        auto ofDensity = [&](uint32_t mask) {
            bool any = !(mask & VolumeShading::kDensity);
            for (size_t k = 0; k < s.factors.size() && k < VolumeShading::kMaxFactors; ++k) {
                any = any || ((mask >> k & 1) && d.slots[k] == 0);
            }
            return any;
        };
        if (ofDensity(s.scatteringFactors) || ofDensity(s.absorptionFactors)) {
            range(0, &d.lutLow[3], &d.lutScale[3]);
            d.lutOffsets[3] = uint32_t(luts.size());
            for (uint32_t i = 0; i < kVolumeLut; ++i) {
                const float rho = at(d.lutLow[3], d.lutScale[3], i);
                float extinction[4] = {};
                for (int c = 0; c < 3; ++c) {
                    for (const auto &[mask, sigma] : {std::pair{s.scatteringFactors, s.scattering[c]},
                                                       std::pair{s.absorptionFactors, s.absorption[c]}}) {
                        float v = std::max(sigma, 0.0f) * s.densityScale * (mask & VolumeShading::kDensity ? rho : 1.0f);
                        for (size_t k = 0; k < s.factors.size() && k < VolumeShading::kMaxFactors; ++k) {
                            v *= (mask >> k & 1) && d.slots[k] == 0 ? s.factors[k].value(rho)[c] : 1.0f;
                        }
                        extinction[c] += std::max(v, 0.0f);
                    }
                    extinction[3] += extinction[c] / 3.0f;
                }
                luts.insert(luts.end(), extinction, extinction + 4);
            }
        }
    }
    // Byte offsets in the pool, 32-byte aligned as NanoVDB grids are.
    std::vector<uint32_t> offsets;
    VkDeviceSize bytes = 0;
    for (const auto &u : used) {
        offsets.push_back(uint32_t(bytes));
        bytes += (u.first->words.size() * 4 + 31) & ~VkDeviceSize(31);
    }
    const uint32_t lutBase = uint32_t(bytes / 4);
    bool changed = used != _poolGrids || luts != _poolLuts || !_volumeGrids.buffer;
    bytes += luts.size() * 4;
    if (changed) {
        _Flush();
        _device->Destroy(_volumeGrids);
        _volumeGrids = _device->CreateDeviceBuffer(std::max<VkDeviceSize>(bytes, 256),
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for (size_t i = 0; i < used.size(); ++i) {
            const std::vector<uint32_t> &w = used[i].first->words;
            _copies.push_back({_volumeGrids.buffer, offsets[i], _Stage(w.data(), w.size() * 4), w.size() * 4});
        }
        if (!luts.empty()) {
            _copies.push_back({_volumeGrids.buffer, lutBase * 4ull, _Stage(luts.data(), luts.size() * 4), luts.size() * 4});
        }
        _Flush();
        _poolGrids = std::move(used);
        _poolLuts = std::move(luts);
    }
    auto offsetOf = [&](const std::shared_ptr<VolumeGrid> &g) {
        for (size_t i = 0; i < _poolGrids.size(); ++i) {
            if (g && _poolGrids[i].first == g) {
                return offsets[i];
            }
        }
        return kNone;
    };

    std::vector<GpuVolume> table;
    std::fill(_gpu.volumeMin, _gpu.volumeMin + 3, std::numeric_limits<float>::max());
    std::fill(_gpu.volumeMax, _gpu.volumeMax + 3, std::numeric_limits<float>::lowest());
    for (const Drawn &d : drawn) {
        const Nano density(d.grids[0].get());
        const pnanovdb_coord_t lo = pnanovdb_root_get_bbox_min(density.buf, density.root);
        const pnanovdb_coord_t hi = pnanovdb_root_get_bbox_max(density.buf, density.root);
        if (lo.x > hi.x || lo.y > hi.y || lo.z > hi.z) {
            continue;  // no active voxels
        }
        GpuVolume g{};
        // One voxel beyond the active ones: trilinear filtering reaches it.
        const int32_t lower[3] = {lo.x, lo.y, lo.z}, upper[3] = {hi.x, hi.y, hi.z};
        for (int k = 0; k < 3; ++k) {
            g.indexMin[k] = float(lower[k] - 1);
            g.indexMax[k] = float(upper[k] + 1);
        }
        const uint32_t linkMask = _LinkMask(d.volume->linkCategories);
        std::memcpy(&g.indexMin[3], &linkMask, sizeof(linkMask));  // as bits (volume.frag's floatBitsToUint)
        const Affine indexToObject = density.IndexToObject();
        g.grids[0] = offsetOf(d.grids[0]);
        g.types[0] = PNANOVDB_GRID_TYPE_FLOAT;
        for (int k = 0; k < 3; ++k) {
            const Nano other(d.grids[1 + k].get());
            Affine toOther;
            const bool usable = other.type != 0 && (k != 0 || other.type == PNANOVDB_GRID_TYPE_FLOAT) &&
                                Invert(other.IndexToObject(), &toOther);
            g.grids[1 + k] = usable ? offsetOf(d.grids[1 + k]) : kNone;
            g.types[1 + k] = other.type;
            Store(toOther * indexToObject, g.toGrid[k]);
            g.factorMasks[3] |= usable ? d.roles & (1u << k) : 0u;
        }
        g.factorMasks[0] = d.shading.scatteringFactors;
        g.factorMasks[1] = d.shading.absorptionFactors;
        g.factorMasks[2] = d.shading.emissionFactors;
        for (int k = 0; k < 4; ++k) {
            g.lutOffsets[k] = d.lutOffsets[k] == kNone ? kNone : lutBase + d.lutOffsets[k];
            // A factor whose grid isn't usable reads 0 (slot 4).
            g.lutSlots[k] = k < 3 && d.slots[k] < 4 && (d.slots[k] == 0 || g.grids[d.slots[k]] != kNone) ? d.slots[k] : 4;
            g.lutLow[k] = d.lutLow[k];
            g.lutScale[k] = g.lutSlots[k] == 4 && k < 3 ? 0.0f : d.lutScale[k];
        }
        // The material's coefficients per channel (the march's
        // transmittance and the shadows it casts are coloured); medium.x is
        // their mean, the optical depth the march steps towards the lights.
        const VolumeShading &s = d.shading;
        float extinction = 0.0f;
        for (int c = 0; c < 3; ++c) {
            extinction += (std::max(s.scattering[c], 0.0f) + std::max(s.absorption[c], 0.0f)) / 3.0f;
        }
        for (int c = 0; c < 3; ++c) {
            g.scattering[c] = std::max(s.scattering[c], 0.0f) * s.densityScale;
            g.absorption[c] = std::max(s.absorption[c], 0.0f) * s.densityScale;
            g.emission[c] = s.emission[c] * s.densityScale;
        }
        const bool anisotropy = !std::isnan(s.anisotropy);
        g.scattering[3] = anisotropy ? std::clamp(s.anisotropy, -0.95f, 0.95f) : 0.0f;
        g.medium[0] = extinction * s.densityScale;
        g.medium[1] = anisotropy ? 1.0f : 0.0f;
        g.medium[2] = s.material ? 0.0f : 1.0f;
        g.medium[3] = d.volume->volumeLimit;
        for (int c = 0; c < 3; ++c) {
            g.fieldEmission[c] = s.emissionScale * s.emissionTint[c];
        }
        // Temperature in Kelvin, or normalized (Houdini's pyro: ~1 burning)
        // to 5000 K as Arnold's blackbody does, unless the material says.
        // ponytail: without a material, decided by the grid's maximum.
        g.params[0] = s.kelvinScale > 0 ? s.kelvinScale : 1.0f;
        if (g.grids[1] != kNone && !(s.kelvinScale > 0)) {
            const Nano t(d.grids[1].get());
            const float hottest = pnanovdb_read_float(
                t.buf, pnanovdb_root_get_max_address(PNANOVDB_GRID_TYPE_FLOAT, t.buf, t.root));
            g.params[0] = hottest > 100.0f ? 1.0f : 5000.0f;
        }
        g.params[1] = s.blackbodyIntensity;
        g.params[2] = s.physical ? 1.0f : 0.0f;
        // Velocity blur: the velocity at a density index point, through
        // advect, is how far the fields moved there per frame (index units).
        // The box grows by a frame of the fastest motion.
        // ponytail: three float grids are read through the first's map (Houdini's
        // staggered components are off by half a voxel); shutters longer than a
        // frame can clip at the box.
        Affine toVelocity, objectToIndex;
        if (d.velocityMode != 0 && Invert(Nano(d.velocity[0].get()).IndexToObject(), &toVelocity) &&
            Invert(indexToObject, &objectToIndex) &&
            std::all_of(d.velocity, d.velocity + (d.velocityMode == 1 ? 1 : 3),
                        [&](const auto &c) { return offsetOf(c) != kNone; })) {
            // The speed's bound (field units): per component the larger
            // magnitude of the grid's minimum and maximum.
            float fastest = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const bool vec = d.velocityMode == 1;
                g.velocity[k] = vec && k > 0 ? kNone : offsetOf(d.velocity[k]);
                const Nano c(d.velocity[vec ? 0 : k].get());
                const uint32_t at = vec ? 4 * k : 0;  // the component's bytes in a value
                const float lo = pnanovdb_read_float(
                    c.buf, pnanovdb_address_offset(pnanovdb_root_get_min_address(c.type, c.buf, c.root), at));
                const float hi = pnanovdb_read_float(
                    c.buf, pnanovdb_address_offset(pnanovdb_root_get_max_address(c.type, c.buf, c.root), at));
                const float m = std::max(std::abs(lo), std::abs(hi));
                fastest += m * m;
            }
            fastest = std::sqrt(fastest);
            g.velocity[3] = d.velocityMode;
            Store(toVelocity * indexToObject, g.toVelocity);
            for (int r = 0; r < 3; ++r) {
                double reach = 0.0;
                for (int c = 0; c < 3; ++c) {
                    objectToIndex.m[r][c] *= d.volume->velocityScale;
                    reach += objectToIndex.m[r][c] * objectToIndex.m[r][c];
                }
                objectToIndex.m[r][3] = 0.0;
                reach = std::sqrt(reach) * fastest;
                g.indexMin[r] -= float(reach);
                g.indexMax[r] += float(reach);
            }
            Store(objectToIndex, g.advect);
        } else {
            std::fill(g.velocity, g.velocity + 3, kNone);
        }
        const std::vector<float> &transforms = d.volume->transforms;
        for (size_t i = 0; i + 16 <= transforms.size(); i += 16) {
            const float *x = &transforms[i];  // row-vector: world = (p, 1) X
            Affine objectToWorld;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    objectToWorld.m[r][c] = x[c * 4 + r];
                }
                objectToWorld.m[r][3] = x[12 + r];
            }
            Affine worldToIndex;
            const Affine indexToWorld = objectToWorld * indexToObject;
            if (Invert(indexToWorld, &worldToIndex)) {
                Store(worldToIndex, g.worldToIndex);
                table.push_back(g);
                for (int k = 0; k < 8; ++k) {  // the index box's corners
                    const double p[3] = {(k & 1) ? g.indexMax[0] : g.indexMin[0],
                                         (k & 2) ? g.indexMax[1] : g.indexMin[1],
                                         (k & 4) ? g.indexMax[2] : g.indexMin[2]};
                    for (int r = 0; r < 3; ++r) {
                        const auto &m = indexToWorld.m[r];
                        const float w = float(m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]);
                        _gpu.volumeMin[r] = std::min(_gpu.volumeMin[r], w);
                        _gpu.volumeMax[r] = std::max(_gpu.volumeMax[r], w);
                    }
                }
            }
        }
    }
    // A new stamp when the table or the grids changed (the local lights' cached atlas holds volume shadows).
    changed = changed || table.size() != _gpu.volumeCount ||
              (!table.empty() && std::memcmp(table.data(), _volumeTable.mapped, table.size() * sizeof(GpuVolume)) != 0);
    if (changed) {
        static std::atomic<uint64_t> versions{0};
        _gpu.volumeVersion = ++versions;
    }
    _device->Reserve(_volumeTable, std::max<size_t>(table.size(), 1) * sizeof(GpuVolume),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(table.begin(), table.end(), static_cast<GpuVolume *>(_volumeTable.mapped));
    _gpu.volumes = _volumeTable.buffer;
    _gpu.volumeGrids = _volumeGrids.buffer;
    _gpu.volumeCount = uint32_t(table.size());
    // Volumes moving along their velocity: the time samples need a capture
    // of the shutter (Renderer), as moved meshes do.
    _gpu.volumeVelocity = std::any_of(table.begin(), table.end(), [](const GpuVolume &g) { return g.velocity[3] != 0; });
    _gpu.volumeFactors = std::any_of(table.begin(), table.end(), [](const GpuVolume &g) {
        return ((g.factorMasks[0] | g.factorMasks[1] | g.factorMasks[2]) & 7u) != 0 ||
               g.factorMasks[0] != VolumeShading::kDensity || g.factorMasks[1] != VolumeShading::kDensity ||
               g.factorMasks[2] != VolumeShading::kDensity;
    });
}

} // namespace vkr
