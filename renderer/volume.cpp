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
const uint32_t kUpsampleFrag[] =
#include "volume_upsample.frag.inc"
;
const uint32_t kShadowFrag[] =
#include "volume_shadow.frag.inc"
;

constexpr VkFormat kHalfFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // premultiplied radiance, 1 - transmittance

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
};

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
}

// When a volume or light shape first draws: a scene without them doesn't
// compile them (seconds with a cold driver cache).
void VolumePass::_EnsurePipelines()
{
    if (_pipeline) {
        return;
    }
    _pipeline = CreateFullscreenPipeline(_dev, _layout, kVolumeFrag, sizeof(kVolumeFrag), Renderer::kColorFormat,
                                         false, true, false, VK_FORMAT_UNDEFINED, true);
    _halfPipeline = CreateFullscreenPipeline(_dev, _layout, kVolumeFrag, sizeof(kVolumeFrag), kHalfFormat);
    _upsamplePipeline = CreateFullscreenPipeline(_dev, _layout, kUpsampleFrag, sizeof(kUpsampleFrag),
                                                 Renderer::kColorFormat, false, true);
    _shapePipeline = CreateFullscreenPipeline(_dev, _layout, kShapesFrag, sizeof(kShapesFrag), Renderer::kColorFormat,
                                              false, true);
    _shadowPipeline = CreateFullscreenPipeline(_dev, _layout, kShadowFrag, sizeof(kShadowFrag),
                                               Transparency::kTintFormat, false, false, true,
                                               Transparency::kTintDepthFormat);
}

VolumePass::~VolumePass()
{
    vkDeviceWaitIdle(_dev.device);
    for (VkPipeline p : {_pipeline, _halfPipeline, _upsamplePipeline, _shadowPipeline, _shapePipeline}) {
        vkDestroyPipeline(_dev.device, p, nullptr);
    }
    vkDestroyPipelineLayout(_dev.device, _layout, nullptr);
    _dev.Destroy(_half);
    _dev.Destroy(_translucentDepth);
}

const Image &VolumePass::EnsureTranslucentDepth(uint32_t width, uint32_t height)
{
    if (width != _depthWidth || height != _depthHeight) {
        _dev.Destroy(_translucentDepth);
        _translucentDepth = _dev.CreateImage(VK_FORMAT_D32_SFLOAT, width, height, 1, 1,
                                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                             VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
        _depthWidth = width;
        _depthHeight = height;
    }
    return _translucentDepth;
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
                        uint32_t width, uint32_t height, const Settings &s, Part part)
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

    VolumeParams p = Params(s, volumeCount);
    p.part = part;
    p.scale = s.resolution >= 2 && _half.image ? 2 : 1;
    if (p.scale == 1) {
        DrawFullscreen(cmd, _pipeline, _layout, set, color, width, height, true, VK_SHADER_STAGE_FRAGMENT_BIT, &p,
                       sizeof(p));
        return true;
    }
    // Half resolution: every pixel of the target is written (zero where
    // no volume is), then upsampled over the image.
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ImageBarrier(cmd, _half.image, range, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    DrawFullscreen(cmd, _halfPipeline, _layout, set, _half.view, _halfWidth, _halfHeight, false,
                   VK_SHADER_STAGE_FRAGMENT_BIT, &p, sizeof(p));
    ImageBarrier(cmd, _half.image, range, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    DrawFullscreen(cmd, _upsamplePipeline, _layout, set, color, width, height, true, VK_SHADER_STAGE_FRAGMENT_BIT,
                   &p, sizeof(p));
    return true;
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
                              uint32_t mapSize, const Settings &s)
{
    _EnsurePipelines();
    VolumeParams p = Params(s, volumeCount);
    p.cascade = cascade;
    p.mapSize = mapSize;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, _layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void VolumePass::RecordTileShadow(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, uint32_t tile,
                                  uint32_t x, uint32_t y, uint32_t size, const Settings &s)
{
    _EnsurePipelines();
    VolumeParams p = Params(s, volumeCount);
    p.part = 1;
    p.cascade = tile;
    p.mapSize = size;
    p.originX = x;
    p.originY = y;
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
        d.grids[0] = field(d.shading.densityField);
        if (!d.grids[0] && !v->fields.empty()) {
            d.grids[0] = v->fields.begin()->second;
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
        for (const auto &g : d.grids) {
            if (Nano(g.get()).type != 0 &&
                std::none_of(used.begin(), used.end(), [&](const auto &u) { return u.first == g; })) {
                used.push_back({g, g->version});
            }
        }
        drawn.push_back(std::move(d));
    }
    // Byte offsets in the pool, 32-byte aligned as NanoVDB grids are.
    std::vector<uint32_t> offsets;
    VkDeviceSize bytes = 0;
    for (const auto &u : used) {
        offsets.push_back(uint32_t(bytes));
        bytes += (u.first->words.size() * 4 + 31) & ~VkDeviceSize(31);
    }
    bool changed = used != _poolGrids || !_volumeGrids.buffer;
    if (changed) {
        _Flush();
        _device->Destroy(_volumeGrids);
        _volumeGrids = _device->CreateDeviceBuffer(std::max<VkDeviceSize>(bytes, 256),
                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for (size_t i = 0; i < used.size(); ++i) {
            const std::vector<uint32_t> &w = used[i].first->words;
            _copies.push_back({_volumeGrids.buffer, offsets[i], _Stage(w.data(), w.size() * 4), w.size() * 4});
        }
        _Flush();
        _poolGrids = std::move(used);
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
        }
        // The material's coefficients per channel (the march's
        // transmittance is coloured); the shadow maps and traced shadows
        // take their mean.
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
}

} // namespace vkr
