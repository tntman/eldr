#include "volume.h"

#include "fullscreen.h"
#include "renderer.h"
#include "scene.h"

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

struct VolumeParams {  // mirror of the push block in volume.frag
    float stepSize;
    uint32_t maxSteps, shadowSteps;
    float density;
    uint32_t count;
};

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
    _pipeline = CreateFullscreenPipeline(_dev.device, _layout, kVolumeFrag, sizeof(kVolumeFrag),
                                         Renderer::kColorFormat, false, true);
}

VolumePass::~VolumePass()
{
    vkDestroyPipeline(_dev.device, _pipeline, nullptr);
    vkDestroyPipelineLayout(_dev.device, _layout, nullptr);
}

bool VolumePass::Record(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t volumeCount, VkImageView color,
                        uint32_t width, uint32_t height, const Settings &s)
{
    if (volumeCount == 0) {
        return false;
    }
    // The shading pass's colour writes land before the blend reads them.
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    VolumeParams p{};
    p.stepSize = std::max(s.stepSize, 0.05f);
    p.maxSteps = std::clamp(s.maxSteps, 1u, 4096u);
    p.shadowSteps = std::min(s.shadowSteps, 256u);
    p.density = std::max(s.density, 0.0f);
    p.count = volumeCount;
    DrawFullscreen(cmd, _pipeline, _layout, set, color, width, height, true, VK_SHADER_STAGE_FRAGMENT_BIT, &p,
                   sizeof(p));
    return true;
}

// Builds the volume table (a GpuVolume per drawn instance) and, when the
// grids they read or their versions changed, the grid pool.
// ponytail: the whole pool is uploaded again whenever any grid changes; keep
// grids in sub-allocated ranges if animated volumes share scenes with large
// static ones.
void Scene::_CommitVolumes()
{
    std::vector<std::pair<std::shared_ptr<VolumeGrid>, uint64_t>> used;
    auto use = [&](const std::shared_ptr<VolumeGrid> &g) {
        if (Nano(g.get()).type == 0) {
            return;
        }
        if (std::none_of(used.begin(), used.end(), [&](const auto &u) { return u.first == g; })) {
            used.push_back({g, g->version});
        }
    };
    std::vector<const Volume *> drawn;
    for (const auto &v : _volumes) {
        if (v->visible && (v->tag & _tagMask) && Nano(v->density.get()).type == PNANOVDB_GRID_TYPE_FLOAT) {
            drawn.push_back(v.get());
            for (const auto *g : {&v->density, &v->temperature, &v->color, &v->emission}) {
                use(*g);
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
    if (used != _poolGrids || !_volumeGrids.buffer) {
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
    for (const Volume *v : drawn) {
        const Nano density(v->density.get());
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
        const std::shared_ptr<VolumeGrid> *others[3] = {&v->temperature, &v->color, &v->emission};
        g.grids[0] = offsetOf(v->density);
        g.types[0] = PNANOVDB_GRID_TYPE_FLOAT;
        for (int k = 0; k < 3; ++k) {
            const Nano other(others[k]->get());
            Affine toOther;
            const bool usable = other.type != 0 && (k != 0 || other.type == PNANOVDB_GRID_TYPE_FLOAT) &&
                                Invert(other.IndexToObject(), &toOther);
            g.grids[1 + k] = usable ? offsetOf(*others[k]) : kNone;
            g.types[1 + k] = other.type;
            Store(toOther * indexToObject, g.toGrid[k]);
        }
        // Temperature in Kelvin, or normalized (Houdini's pyro: ~1 burning)
        // to 5000 K as Arnold's blackbody does.
        // ponytail: decided by the grid's maximum; a per-volume setting
        // (material) would be the real thing.
        g.params[0] = 1.0f;
        if (g.grids[1] != kNone) {
            const Nano t(v->temperature.get());
            const float hottest = pnanovdb_read_float(
                t.buf, pnanovdb_root_get_max_address(PNANOVDB_GRID_TYPE_FLOAT, t.buf, t.root));
            g.params[0] = hottest > 100.0f ? 1.0f : 5000.0f;
        }
        for (size_t i = 0; i + 16 <= v->transforms.size(); i += 16) {
            const float *x = &v->transforms[i];  // row-vector: world = (p, 1) X
            Affine objectToWorld;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    objectToWorld.m[r][c] = x[c * 4 + r];
                }
                objectToWorld.m[r][3] = x[12 + r];
            }
            Affine worldToIndex;
            if (Invert(objectToWorld * indexToObject, &worldToIndex)) {
                Store(worldToIndex, g.worldToIndex);
                table.push_back(g);
            }
        }
    }
    _device->Reserve(_volumeTable, std::max<size_t>(table.size(), 1) * sizeof(GpuVolume),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(table.begin(), table.end(), static_cast<GpuVolume *>(_volumeTable.mapped));
    _gpu.volumes = _volumeTable.buffer;
    _gpu.volumeGrids = _volumeGrids.buffer;
    _gpu.volumeCount = uint32_t(table.size());
}

} // namespace vkr
