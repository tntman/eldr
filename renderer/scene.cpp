#include "scene.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <limits>

namespace vkr {

namespace {

// std430 mirrors of the structs in mesh.frag.
struct GpuTexRef {
    int32_t index;    // bindless texture, or with udim the first of its kUdimTiles in the UDIM table
    int32_t channel;
    int32_t udim;
    int32_t pad;
    float scale[4];
    float bias[4];
    float uvRows[2][4];  // TextureRef::uvTransform, one row each (w unused)
};
struct GpuMaterial {
    float baseColor[4]; // rgb, w = 1: use the mesh's displayColor instead
    float emissive[4];
    float params[4];  // metallic, roughness, occlusion, opacity
    GpuTexRef tex[7]; // baseColor, emissive, metallic, roughness, occlusion, normal, opacity
    // MaterialProgram: first op (uvec4) in programCode, ops, first constant
    // (vec4) in programConstants, its images' GpuTexRefs (5 vec4 each) there.
    uint32_t program[4];
    float specular[4];  // Material::specular
    // Lobes (shaders/brdf.glsl).
    float specularColor[4];  // specular workflow F0, w = 1 when used
    float coat[4];           // weight, roughness, F0
    float coatColor[4];
};
static_assert(sizeof(GpuTexRef) == 5 * 16, "kImage reads GpuTexRefs as five vec4 constants");
struct GpuLight {        // mirror of Light in shaders/lights.glsl
    float position[4];   // distant: direction to the light; else center. w: type
    float color[4];      // distant: irradiance; else surface radiance
    float axis[4];       // emission axis; w: sphere radius
    float halfWidth[4];  // rect; w: sphere shading radius (0 = point)
    float halfHeight[4]; // rect; w: diffuse multiplier
    float shaping[4];    // soft cone angle, cone angle (radians), focus, specular multiplier
    float focusTint[4];
};

constexpr VkBufferUsageFlags kPoolUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

VkFormat ToVk(Texture::Format f)
{
    switch (f) {
    case Texture::Format::R8:         return VK_FORMAT_R8_UNORM;
    case Texture::Format::RG8:        return VK_FORMAT_R8G8_UNORM;
    case Texture::Format::RGBA8:      return VK_FORMAT_R8G8B8A8_UNORM;
    case Texture::Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Texture::Format::R16:        return VK_FORMAT_R16_UNORM;
    case Texture::Format::RG16:       return VK_FORMAT_R16G16_UNORM;
    case Texture::Format::RGBA16:     return VK_FORMAT_R16G16B16A16_UNORM;
    case Texture::Format::R16F:       return VK_FORMAT_R16_SFLOAT;
    case Texture::Format::RG16F:      return VK_FORMAT_R16G16_SFLOAT;
    case Texture::Format::RGBA16F:    return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Texture::Format::RGBA32F:    return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

size_t BytesPerPixel(Texture::Format f)
{
    return size_t(Channels(f)) * BytesPerChannel(f);
}

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

// Hashable bit pattern of one vertex, for deduplicating corners.
struct VertexKey {
    Vertex v;
    SkinVertex skin, skin2;  // skin2: influences 4..7, zero with four
    float primvars[4];       // vertex-rate primvars, zero without
    bool operator==(const VertexKey &o) const { return std::memcmp(this, &o, sizeof(VertexKey)) == 0; }
};
struct VertexKeyHash {
    size_t operator()(const VertexKey &k) const
    {
        uint64_t h = 1469598103934665603ull;  // FNV-1a
        const auto *p = reinterpret_cast<const uint8_t *>(&k);
        for (size_t i = 0; i < sizeof(VertexKey); ++i) {
            h = (h ^ p[i]) * 1099511628211ull;
        }
        return size_t(h);
    }
};

// Float to IEEE half, rounded; tiny values flush to zero.
uint16_t ToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exponent = int32_t((x >> 23) & 0xFF) - 127 + 15;
    if (exponent <= 0) {
        return uint16_t(sign);
    }
    if (exponent >= 31) {
        return uint16_t(sign | 0x7C00u);
    }
    return uint16_t(sign | ((uint32_t(exponent) << 10) + (((x & 0x7FFFFFu) + 0x1000u) >> 13)));
}

uint32_t PackUnorm16(float a, float b)
{
    auto q = [](float x) { return uint32_t(std::lround(std::clamp(x, 0.0f, 1.0f) * 65535.0f)); };
    return q(a) | (q(b) << 16);
}

// World AABB of an object-space box under a row-vector matrix.
void TransformBounds(const float *lo, const float *hi, const float *x, float *outLo, float *outHi)
{
    for (int k = 0; k < 3; ++k) {
        const float c[3] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2])};
        const float e[3] = {0.5f * (hi[0] - lo[0]), 0.5f * (hi[1] - lo[1]), 0.5f * (hi[2] - lo[2])};
        const float wc = c[0] * x[k] + c[1] * x[4 + k] + c[2] * x[8 + k] + x[12 + k];
        const float we = e[0] * std::abs(x[k]) + e[1] * std::abs(x[4 + k]) + e[2] * std::abs(x[8 + k]);
        outLo[k] = wc - we;
        outHi[k] = wc + we;
    }
}

} // namespace

// ponytail: first fit over a sorted free list, O(free ranges) per call.
// Size classes or a TLSF allocator if heavy churn ever shows in profiles.
bool RangeAllocator::Allocate(uint32_t count, uint32_t *offset)
{
    if (count == 0) {
        *offset = 0;
        return true;
    }
    for (auto it = _free.begin(); it != _free.end(); ++it) {
        if (it->second >= count) {
            *offset = it->first;
            const uint32_t restOffset = it->first + count, rest = it->second - count;
            _free.erase(it);
            if (rest > 0) {
                _free.emplace(restOffset, rest);
            }
            return true;
        }
    }
    return false;
}

void RangeAllocator::Free(uint32_t offset, uint32_t count)
{
    if (count == 0) {
        return;
    }
    auto next = _free.lower_bound(offset);
    if (next != _free.begin()) {
        const auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            offset = prev->first;
            count += prev->second;
            _free.erase(prev);
        }
    }
    if (next != _free.end() && offset + count == next->first) {
        count += next->second;
        _free.erase(next);
    }
    _free.emplace(offset, count);
}

void RangeAllocator::Grow(uint32_t capacity)
{
    if (capacity > _capacity) {
        const uint32_t old = _capacity;
        _capacity = capacity;
        Free(old, capacity - old);
    }
}

template <typename T>
void Scene::_RemoveSlot(std::vector<std::unique_ptr<T>> &items, T *item)
{
    const size_t slot = item->_slot;
    std::swap(items[slot], items.back());
    items[slot]->_slot = slot;
    items.pop_back();
}

Texture::~Texture()
{
    if (_device) {
        vkDestroyImageView(_device->device, _view, nullptr);
        vkDestroyImage(_device->device, _image, nullptr);
        _device->Free(_memory);
    }
}

Scene::Scene(Device *device)
    : _device(device)
{
}

Scene::~Scene()
{
    if (_device) {
        for (Buffer *b : {&_vertices, &_indices, &_instances, &_visible, &_skin, &_joints, &_skinXforms,
                          &_jointBoundsPool, &_blendOffsets, &_blendWeights, &_draws, &_commandTemplate,
                          &_materials, &_udimTiles, &_motion, &_lightBuffer, &_staging, &_primvarVertices,
                          &_programCode, &_programConstants, &_primvarTable, &_primvarData}) {
            _device->Destroy(*b);
        }
        _DestroyEnvironment();
    }
}

bool Scene::SetTagMask(uint32_t mask)
{
    const bool changed = mask != _tagMask;
    _tagMask = mask;
    return changed;
}

Mesh *Scene::AddMesh()
{
    _changed = true;
    _meshes.push_back(std::make_unique<Mesh>());
    _meshes.back()->_slot = _meshes.size() - 1;
    return _meshes.back().get();
}

VkDeviceSize Scene::VertexBytes() const
{
    VkDeviceSize bytes = 0;
    for (const Buffer *b : {&_vertices, &_instances, &_visible, &_skin, &_joints, &_skinXforms, &_jointBoundsPool,
                            &_blendOffsets, &_blendWeights, &_primvarVertices}) {
        bytes += b->size;
    }
    return bytes;
}

void Scene::RemoveMesh(Mesh *mesh)
{
    _changed = true;
    // Only bookkeeping here; the freed instance slots are invalidated on the
    // GPU at the next Commit.
    if (_device) {
        _vertexAlloc.Free(mesh->_vertexOffset, mesh->_vertexCount);
        _indexAlloc.Free(mesh->_firstIndex, mesh->_indexCount);
        _skinAlloc.Free(mesh->_skinOffset, mesh->_skinCount);
        _jointAlloc.Free(mesh->_jointBase, mesh->_jointSlots);
        _skinXformAlloc.Free(mesh->_skinXformBase, mesh->_skinXformSlots);
        _boundsAlloc.Free(mesh->_boundsBase, mesh->_boundsSlots);
        _blendAlloc.Free(mesh->_blendBase, mesh->_blendSlots);
        _weightAlloc.Free(mesh->_weightBase, mesh->_weightSlots);
        _primvarVertexAlloc.Free(mesh->_primvarVertexOffset, mesh->_primvarVertexCount);
        _FreeInstances(*mesh);
    }
    const size_t slot = mesh->_slot;
    _RemoveSlot(_meshes, mesh);
    if (slot < _meshes.size()) {
        _meshes[slot]->transformsDirty = true;  // moved: its instances must name the new slot
    }
}

Light *Scene::AddLight()
{
    _lights.push_back(std::make_unique<Light>());
    _lights.back()->_slot = _lights.size() - 1;
    return _lights.back().get();
}

void Scene::RemoveLight(Light *light)
{
    _RemoveSlot(_lights, light);
}

Dome *Scene::AddDome()
{
    _domes.push_back(std::make_unique<Dome>());
    _domes.back()->_slot = _domes.size() - 1;
    return _domes.back().get();
}

void Scene::RemoveDome(Dome *dome)
{
    _RemoveSlot(_domes, dome);
}

VkDeviceSize Scene::_Stage(const void *data, VkDeviceSize size)
{
    const VkDeviceSize offset = (_stagingBytes.size() + 15) & ~VkDeviceSize(15);
    _stagingBytes.resize(size_t(offset + size));
    std::memcpy(_stagingBytes.data() + offset, data, size_t(size));
    return offset;
}

// ponytail: one synchronous submit per Commit (plus one per pool growth).
// A persistent staging ring with frames in flight comes with GL interop.
void Scene::_Flush()
{
    if (_copies.empty() && _fills.empty()) {
        return;
    }
    if (!_stagingBytes.empty()) {
        _device->Reserve(_staging, _stagingBytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(_staging.mapped, _stagingBytes.data(), _stagingBytes.size());
    }
    _device->Submit([&](VkCommandBuffer cmd) {
        // Fills invalidate freed instance slots; copies may reuse them, so
        // they must land after the fills.
        for (const Fill &f : _fills) {
            vkCmdFillBuffer(cmd, f.dst, f.offset, f.size, 0xFFFFFFFFu);
        }
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        for (const Copy &c : _copies) {
            const VkBufferCopy region{c.srcOffset, c.dstOffset, c.size};
            vkCmdCopyBuffer(cmd, _staging.buffer, c.dst, 1, &region);
        }
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    });
    _stagingBytes.clear();
    _copies.clear();
    _fills.clear();
}

// Makes room for a further `needed` elements: grows the pool (doubling,
// or by a quarter once it is over 256 MB, since old and new coexist while
// copying), copies the old contents over on the GPU and, for the instance
// pool, marks the new slots free and resizes the visible lists.
void Scene::_EnsureCapacity(Buffer &buffer, RangeAllocator &alloc, uint32_t needed,
                            VkDeviceSize elementSize, VkBufferUsageFlags usage, bool instances)
{
    _Flush();  // pending copies/fills target the old buffer
    const uint32_t old = alloc.Capacity();
    const uint32_t step = VkDeviceSize(old) * elementSize > (VkDeviceSize(256) << 20) ? old / 4 : old;
    const uint32_t capacity = std::max({old + step, old + needed, 1024u});
    Buffer grown = _device->CreateDeviceBuffer(capacity * elementSize, usage | kPoolUsage);
    _device->Submit([&](VkCommandBuffer cmd) {
        if (old > 0) {
            const VkBufferCopy region{0, 0, VkDeviceSize(old) * elementSize};
            vkCmdCopyBuffer(cmd, buffer.buffer, grown.buffer, 1, &region);
        }
        if (instances) {
            vkCmdFillBuffer(cmd, grown.buffer, VkDeviceSize(old) * elementSize,
                            VkDeviceSize(capacity - old) * elementSize, 0xFFFFFFFFu);
        }
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    });
    _device->Destroy(buffer);
    buffer = grown;
    alloc.Grow(capacity);

    if (instances) {
        _device->Destroy(_visible);
        _visible = _device->CreateDeviceBuffer(VkDeviceSize(capacity) * kVisibleLists * sizeof(uint32_t),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | kPoolUsage);
    }
}

void Scene::_UploadGeometry(Mesh &m)
{
    _vertexAlloc.Free(m._vertexOffset, m._vertexCount);
    _indexAlloc.Free(m._firstIndex, m._indexCount);
    _skinAlloc.Free(m._skinOffset, m._skinCount);
    _blendAlloc.Free(m._blendBase, m._blendSlots);
    _primvarVertexAlloc.Free(m._primvarVertexOffset, m._primvarVertexCount);
    m._vertexCount = m._indexCount = m._skinCount = m._blendSlots = m._primvarVertexCount = 0;
    m._jointBounds.clear();
    m.geometryDirty = false;

    // Elements of the per-corner arrays: corners, or vertices with indices.
    const bool indexed = !m.indices.empty();
    const size_t elements = m.points.size() / 3;
    const size_t corners = indexed ? m.indices.size() : elements;
    const bool skinned = !m.jointIndices.empty();
    // Sizes come from scene data: skip a mesh whose arrays disagree rather
    // than read past them.
    const bool valid = m.points.size() % 3 == 0 && corners % 3 == 0 &&
                       (m.normals.empty() || m.normals.size() == m.points.size()) &&
                       (m.uvs.empty() || m.uvs.size() == elements * 2) &&
                       (m.colors.empty() || m.colors.size() == m.points.size()) &&
                       (!skinned || ((m.influences == 4 || m.influences == 8) &&
                                     m.jointIndices.size() == elements * m.influences &&
                                     m.jointWeights.size() == elements * m.influences && m.jointCount > 0)) &&
                       std::all_of(m.indices.begin(), m.indices.end(), [&](uint32_t i) { return i < elements; });
    if (!valid || corners == 0) {
        return;
    }
    // Vertex-rate primvars: the first that fit in four components.
    m._primvarComponents.assign(m.primvars.size(), kNone);
    uint32_t packed = 0;
    for (size_t p = 0; p < m.primvars.size(); ++p) {
        const Primvar &pv = m.primvars[p];
        if (pv.rate == Primvar::Rate::Vertex && pv.components >= 1 && packed + pv.components <= 4 &&
            pv.values.size() == elements * pv.components) {
            m._primvarComponents[p] = packed;
            packed += pv.components;
        }
    }
    // Blend shapes (skinned meshes only): offset ranges must stay inside the
    // offsets, otherwise they are dropped.
    const uint32_t offsets = uint32_t(m.blendOffsets.size() / 4);
    bool blend = skinned && m.blendRanges.size() == elements * 2 && offsets > 0;
    for (size_t i = 0; blend && i < elements; ++i) {
        blend = uint64_t(m.blendRanges[i * 2]) + m.blendRanges[i * 2 + 1] <= offsets;
    }
    if (blend) {
        while (!_blendAlloc.Allocate(offsets, &m._blendBase)) {
            _EnsureCapacity(_blendOffsets, _blendAlloc, offsets, 4 * sizeof(float),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        m._blendSlots = offsets;
        _copies.push_back({_blendOffsets.buffer, VkDeviceSize(m._blendBase) * 4 * sizeof(float),
                           _Stage(m.blendOffsets.data(), size_t(offsets) * 4 * sizeof(float)),
                           VkDeviceSize(offsets) * 4 * sizeof(float)});
    }
    if (skinned) {
        m._jointBounds.resize(size_t(m.jointCount) * 6);
        for (uint32_t j = 0; j < m.jointCount; ++j) {
            std::fill_n(&m._jointBounds[j * 6], 3, std::numeric_limits<float>::max());
            std::fill_n(&m._jointBounds[j * 6 + 3], 3, std::numeric_limits<float>::lowest());
        }
    }

    // Corners sharing position, normal and uv become one indexed vertex
    // (indexed meshes keep theirs).
    std::vector<Vertex> vertices;
    std::vector<SkinVertex> skin;
    std::vector<float> primvarVertices;  // 4 per vertex, with vertex-rate primvars
    std::vector<uint32_t> indices = indexed ? m.indices : std::vector<uint32_t>(corners);
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> unique;
    unique.reserve(indexed ? 0 : corners);
    std::fill(m._boundsMin, m._boundsMin + 3, std::numeric_limits<float>::max());
    std::fill(m._boundsMax, m._boundsMax + 3, std::numeric_limits<float>::lowest());
    for (size_t i = 0; i < elements; ++i) {
        VertexKey key{};
        std::memcpy(key.v.position, &m.points[i * 3], sizeof(key.v.position));
        if (!m.normals.empty()) {
            std::memcpy(key.v.normal, &m.normals[i * 3], sizeof(key.v.normal));
        }
        if (!m.uvs.empty()) {
            std::memcpy(key.v.uv, &m.uvs[i * 2], sizeof(key.v.uv));
        }
        if (!m.colors.empty()) {
            for (int c = 0; c < 3; ++c) {
                key.v.color[c] = ToHalf(m.colors[i * 3 + c]);
            }
        }
        for (size_t p = 0; packed > 0 && p < m.primvars.size(); ++p) {
            const Primvar &pv = m.primvars[p];
            if (m._primvarComponents[p] != kNone) {
                std::copy_n(&pv.values[i * pv.components], pv.components, key.primvars + m._primvarComponents[p]);
            }
        }
        if (skinned) {
            const uint16_t *j = &m.jointIndices[i * m.influences];
            const float *w = &m.jointWeights[i * m.influences];
            for (uint32_t k = 0; k < m.influences; k += 2) {
                SkinVertex &s = k < 4 ? key.skin : key.skin2;
                s.joints[(k % 4) / 2] = j[k] | (uint32_t(j[k + 1]) << 16);
                s.weights[(k % 4) / 2] = PackUnorm16(w[k], w[k + 1]);
            }
            // How far blend shapes can move this point (weights up to 1)
            // widens the bounds of the joints it follows.
            float reach = 0.0f;
            if (blend) {
                const uint32_t first = m.blendRanges[i * 2], count = m.blendRanges[i * 2 + 1];
                key.skin.blend[0] = m._blendBase + first;
                key.skin.blend[1] = count;
                for (uint32_t o = first; o < first + count; ++o) {
                    const float *d = &m.blendOffsets[size_t(o) * 4];
                    reach += std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                }
            }
            for (uint32_t k = 0; k < m.influences; ++k) {
                if (w[k] > 0.0f && j[k] < m.jointCount) {
                    float *b = &m._jointBounds[size_t(j[k]) * 6];
                    for (int c = 0; c < 3; ++c) {
                        b[c] = std::min(b[c], key.v.position[c] - reach);
                        b[3 + c] = std::max(b[3 + c], key.v.position[c] + reach);
                    }
                }
            }
        }
        bool inserted = true;
        if (!indexed) {
            const auto it = unique.emplace(key, uint32_t(vertices.size()));
            inserted = it.second;
            indices[i] = it.first->second;
        }
        if (inserted) {
            vertices.push_back(key.v);
            if (packed > 0) {
                primvarVertices.insert(primvarVertices.end(), key.primvars, key.primvars + 4);
            }
            if (skinned) {
                skin.push_back(key.skin);
                if (m.influences == 8) {
                    skin.push_back(key.skin2);
                }
            }
            for (int k = 0; k < 3; ++k) {
                m._boundsMin[k] = std::min(m._boundsMin[k], key.v.position[k]);
                m._boundsMax[k] = std::max(m._boundsMax[k], key.v.position[k]);
            }
        }
    }

    const uint32_t vertexCount = uint32_t(vertices.size()), indexCount = uint32_t(corners);
    uint32_t vertexOffset, firstIndex;
    while (!_vertexAlloc.Allocate(vertexCount, &vertexOffset)) {
        _EnsureCapacity(_vertices, _vertexAlloc, vertexCount, sizeof(Vertex),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    }
    while (!_indexAlloc.Allocate(indexCount, &firstIndex)) {
        _EnsureCapacity(_indices, _indexAlloc, indexCount, sizeof(uint32_t),
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
    }
    _copies.push_back({_vertices.buffer, VkDeviceSize(vertexOffset) * sizeof(Vertex),
                       _Stage(vertices.data(), vertices.size() * sizeof(Vertex)),
                       vertices.size() * sizeof(Vertex)});
    _copies.push_back({_indices.buffer, VkDeviceSize(firstIndex) * sizeof(uint32_t),
                       _Stage(indices.data(), indices.size() * sizeof(uint32_t)),
                       indices.size() * sizeof(uint32_t)});
    m._vertexOffset = vertexOffset;
    m._vertexCount = vertexCount;
    m._firstIndex = firstIndex;
    m._indexCount = indexCount;
    if (packed > 0) {
        while (!_primvarVertexAlloc.Allocate(vertexCount, &m._primvarVertexOffset)) {
            _EnsureCapacity(_primvarVertices, _primvarVertexAlloc, vertexCount, 4 * sizeof(float),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        m._primvarVertexCount = vertexCount;
        _copies.push_back({_primvarVertices.buffer, VkDeviceSize(m._primvarVertexOffset) * 4 * sizeof(float),
                           _Stage(primvarVertices.data(), primvarVertices.size() * sizeof(float)),
                           primvarVertices.size() * sizeof(float)});
    }
    if (skinned) {
        const uint32_t skinCount = uint32_t(skin.size());
        uint32_t skinOffset;
        while (!_skinAlloc.Allocate(skinCount, &skinOffset)) {
            _EnsureCapacity(_skin, _skinAlloc, skinCount, sizeof(SkinVertex),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        _copies.push_back({_skin.buffer, VkDeviceSize(skinOffset) * sizeof(SkinVertex),
                           _Stage(skin.data(), skin.size() * sizeof(SkinVertex)),
                           skin.size() * sizeof(SkinVertex)});
        m._skinOffset = skinOffset;
        m._skinCount = skinCount;
        m._skinStride = m.influences == 8 ? 2 : 1;

        _boundsAlloc.Free(m._boundsBase, m._boundsSlots);
        uint32_t boundsBase;
        while (!_boundsAlloc.Allocate(m.jointCount, &boundsBase)) {
            _EnsureCapacity(_jointBoundsPool, _boundsAlloc, m.jointCount, sizeof(JointBounds),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        std::vector<JointBounds> bounds(m.jointCount);
        for (uint32_t j = 0; j < m.jointCount; ++j) {
            std::copy_n(&m._jointBounds[size_t(j) * 6], 3, bounds[j].min);
            std::copy_n(&m._jointBounds[size_t(j) * 6 + 3], 3, bounds[j].max);
        }
        _copies.push_back({_jointBoundsPool.buffer, VkDeviceSize(boundsBase) * sizeof(JointBounds),
                           _Stage(bounds.data(), bounds.size() * sizeof(JointBounds)),
                           bounds.size() * sizeof(JointBounds)});
        m._boundsBase = boundsBase;
        m._boundsSlots = m.jointCount;
    }
}

void Scene::_FreeInstances(Mesh &m)
{
    if (m._instanceCount > 0) {
        _instanceAlloc.Free(m._instanceBase, m._instanceCount);
        _fills.push_back({_instances.buffer, VkDeviceSize(m._instanceBase) * sizeof(Instance),
                          VkDeviceSize(m._instanceCount) * sizeof(Instance)});
    }
    m._instanceCount = 0;
}

// Rewrites only this mesh's range of the instance pool; reallocates it when
// the instance count changed.
void Scene::_UploadInstances(Mesh &m)
{
    m.transformsDirty = false;
    const bool skinned = !m._jointBounds.empty();
    const uint32_t count = m.transforms.size() % 16 == 0 ? uint32_t(m.transforms.size() / 16) : 0;
    if (count != m._instanceCount) {
        _FreeInstances(m);
        uint32_t base;
        while (!_instanceAlloc.Allocate(count, &base)) {
            _EnsureCapacity(_instances, _instanceAlloc, count, sizeof(Instance),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        }
        m._instanceBase = base;
        m._instanceCount = count;
    }
    if (count == 0) {
        return;
    }
    const uint32_t jointSlots = skinned ? count * m.jointCount : 0;
    if (jointSlots != m._jointSlots) {
        _jointAlloc.Free(m._jointBase, m._jointSlots);
        uint32_t base;
        while (!_jointAlloc.Allocate(jointSlots, &base)) {
            _EnsureCapacity(_joints, _jointAlloc, jointSlots, 16 * sizeof(float),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        m._jointBase = base;
        m._jointSlots = jointSlots;
    }
    std::vector<Instance> instances(count);
    for (uint32_t i = 0; i < count; ++i) {
        Instance &inst = instances[i];
        const float *x = &m.transforms[size_t(i) * 16];  // USD row-vector order
        for (int r = 0; r < 3; ++r) {
            inst.rows[r][0] = x[r];
            inst.rows[r][1] = x[4 + r];
            inst.rows[r][2] = x[8 + r];
            inst.rows[r][3] = x[12 + r];
        }
        inst.draw = uint32_t(m._slot);
        inst.index = int32_t(i);
        inst.joints = skinned ? m._jointBase + i * m.jointCount : kNone;
    }
    _copies.push_back({_instances.buffer, VkDeviceSize(m._instanceBase) * sizeof(Instance),
                       _Stage(instances.data(), instances.size() * sizeof(Instance)),
                       instances.size() * sizeof(Instance)});
}

// The mesh's skinning transforms (the only per-frame upload of a skinned
// mesh) and, for the scene bounds, the object-space bounds of all its poses.
void Scene::_UploadJoints(Mesh &m)
{
    m.jointsDirty = false;
    const uint32_t joints = m.jointCount, slots = uint32_t(m.skinningTransforms.size() / 16);
    const bool valid = joints > 0 && slots % joints == 0 && m.skinningTransforms.size() % 16 == 0 &&
                       (slots == joints || slots == joints * m._instanceCount);
    if (!valid) {
        m.skinningTransforms.clear();  // draws nothing rather than garbage
    }
    const uint32_t used = valid ? slots : 0;
    if (used != m._skinXformSlots) {
        _skinXformAlloc.Free(m._skinXformBase, m._skinXformSlots);
        uint32_t base;
        while (!_skinXformAlloc.Allocate(used, &base)) {
            _EnsureCapacity(_skinXforms, _skinXformAlloc, used, 16 * sizeof(float),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        m._skinXformBase = base;
        m._skinXformSlots = used;
    }
    m._skinPerInstance = valid && slots != joints;

    // Blend shape weights for the same poses.
    const uint32_t shapes = m.blendShapeCount, weights = uint32_t(m.blendWeights.size());
    const bool blendValid = m._blendSlots > 0 && shapes > 0 &&
                            (weights == shapes || weights == shapes * m._instanceCount);
    const uint32_t usedWeights = blendValid ? weights : 0;
    if (usedWeights != m._weightSlots) {
        _weightAlloc.Free(m._weightBase, m._weightSlots);
        uint32_t base;
        while (!_weightAlloc.Allocate(usedWeights, &base)) {
            _EnsureCapacity(_blendWeights, _weightAlloc, usedWeights, sizeof(float),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        m._weightBase = base;
        m._weightSlots = usedWeights;
    }
    m._blendPerInstance = blendValid && weights != shapes;
    if (usedWeights > 0) {
        _copies.push_back({_blendWeights.buffer, VkDeviceSize(m._weightBase) * sizeof(float),
                           _Stage(m.blendWeights.data(), m.blendWeights.size() * sizeof(float)),
                           m.blendWeights.size() * sizeof(float)});
    }

    std::fill(m._posedMin, m._posedMin + 3, std::numeric_limits<float>::max());
    std::fill(m._posedMax, m._posedMax + 3, std::numeric_limits<float>::lowest());
    if (used == 0) {
        return;
    }
    _copies.push_back({_skinXforms.buffer, VkDeviceSize(m._skinXformBase) * 16 * sizeof(float),
                       _Stage(m.skinningTransforms.data(), m.skinningTransforms.size() * sizeof(float)),
                       m.skinningTransforms.size() * sizeof(float)});
    for (uint32_t s = 0; s < used; ++s) {
        const float *b = &m._jointBounds[size_t(s % joints) * 6];
        if (b[0] > b[3]) {
            continue;  // moves no vertex
        }
        float lo[3], hi[3];
        TransformBounds(b, b + 3, &m.skinningTransforms[size_t(s) * 16], lo, hi);
        for (int k = 0; k < 3; ++k) {
            m._posedMin[k] = std::min(m._posedMin[k], lo[k]);
            m._posedMax[k] = std::max(m._posedMax[k], hi[k]);
        }
    }
}

void Scene::_UpdateWorldBounds(Mesh &m)
{
    std::fill(m._worldMin, m._worldMin + 3, std::numeric_limits<float>::max());
    std::fill(m._worldMax, m._worldMax + 3, std::numeric_limits<float>::lowest());
    if (m._indexCount == 0) {
        return;
    }
    // Skinned meshes: the bounds of every pose instead of the rest pose.
    const bool skinned = !m._jointBounds.empty();
    if (skinned && m._posedMin[0] > m._posedMax[0]) {
        return;
    }
    const float *lo = skinned ? m._posedMin : m._boundsMin, *hi = skinned ? m._posedMax : m._boundsMax;
    float center[3], extent[3];
    for (int k = 0; k < 3; ++k) {
        center[k] = 0.5f * (lo[k] + hi[k]);
        extent[k] = 0.5f * (hi[k] - lo[k]);
    }
    for (uint32_t i = 0; i < m._instanceCount; ++i) {
        const float *x = &m.transforms[size_t(i) * 16];
        // World AABB of this instance (row-vector: p' = p * M).
        for (int k = 0; k < 3; ++k) {
            const float c = center[0] * x[k] + center[1] * x[4 + k] + center[2] * x[8 + k] + x[12 + k];
            const float e = extent[0] * std::abs(x[k]) + extent[1] * std::abs(x[4 + k]) +
                            extent[2] * std::abs(x[8 + k]);
            m._worldMin[k] = std::min(m._worldMin[k], c - e);
            m._worldMax[k] = std::max(m._worldMax[k], c + e);
        }
    }
}

uint32_t Channels(Texture::Format f)
{
    switch (f) {
    case Texture::Format::R8:
    case Texture::Format::R16:
    case Texture::Format::R16F: return 1;
    case Texture::Format::RG8:
    case Texture::Format::RG16:
    case Texture::Format::RG16F: return 2;
    default: return 4;
    }
}

uint32_t BytesPerChannel(Texture::Format f)
{
    switch (f) {
    case Texture::Format::R8:
    case Texture::Format::RG8:
    case Texture::Format::RGBA8:
    case Texture::Format::RGBA8_SRGB: return 1;
    case Texture::Format::RGBA32F: return 4;
    default: return 2;
    }
}

void Scene::_Upload(Texture &t)
{
    const VkFormat format = ToVk(t.format);
    const uint32_t levels = 1 + uint32_t(std::floor(std::log2(std::max(t.width, t.height))));
    const VkDevice device = _device->device;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {t.width, t.height, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Check(vkCreateImage(device, &ci, nullptr, &t._image), "vkCreateImage");
    t._device = _device;  // from here on ~Texture frees whatever exists
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, t._image, &req);
    t._memory = _device->Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true);
    Check(vkBindImageMemory(device, t._image, t._memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t._image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    if (Channels(t.format) < 4) {  // gray (+ alpha): rrr1 or rrrg
        vci.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                          Channels(t.format) == 2 ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_ONE};
    }
    Check(vkCreateImageView(device, &vci, nullptr, &t._view), "vkCreateImageView");

    _device->Reserve(_staging, t.pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(_staging.mapped, t.pixels.data(), t.pixels.size());

    _device->Submit([&](VkCommandBuffer cmd) {
        auto level = [](uint32_t i) {
            return VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, i, 1, 0, 1};
        };
        ImageBarrier(cmd, t._image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1},
                     VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {t.width, t.height, 1};
        vkCmdCopyBufferToImage(cmd, _staging.buffer, t._image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        // Each level is blitted from the previous one, then made shader-readable.
        for (uint32_t i = 1; i < levels; ++i) {
            ImageBarrier(cmd, t._image, level(i - 1),
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
            blit.srcOffsets[1] = {int32_t(std::max(1u, t.width >> (i - 1))),
                                  int32_t(std::max(1u, t.height >> (i - 1))), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            blit.dstOffsets[1] = {int32_t(std::max(1u, t.width >> i)),
                                  int32_t(std::max(1u, t.height >> i)), 1};
            vkCmdBlitImage(cmd, t._image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           t._image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);
            ImageBarrier(cmd, t._image, level(i - 1),
                         VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        ImageBarrier(cmd, t._image, level(levels - 1),
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
}

int32_t Scene::_TextureIndex(Texture *t)
{
    if (!t || t->width == 0 || t->height == 0 ||
        t->pixels.size() != size_t(t->width) * t->height * BytesPerPixel(t->format)) {
        return -1;
    }
    const auto found = _textureSlots.find(t);
    if (found != _textureSlots.end()) {
        return found->second;
    }
    // ponytail: textures past kMaxTextures fall back to constants.
    if (_gpu.textures.size() >= kMaxTextures) {
        return -1;
    }
    if (!t->_view) {
        _Upload(*t);
    }
    const int32_t index = int32_t(_gpu.textures.size());
    _gpu.textures.push_back(t->_view);
    _textureSlots.emplace(t, index);
    return index;
}

void Scene::_DestroyEnvironment()
{
    vkDestroyImageView(_device->device, _envView, nullptr);
    vkDestroyImage(_device->device, _envImage, nullptr);
    _device->Free(_envMemory);
    _envView = VK_NULL_HANDLE;
    _envImage = VK_NULL_HANDLE;
    _envMemory = VK_NULL_HANDLE;
    _envBuilt = false;
}

// Re-filters only when the dome's texture changes; intensity and rotation
// are applied at shading time.
void Scene::_UpdateEnvironment(const Dome *dome)
{
    const Texture *source = dome->texture.get();
    const bool usable = source && source->width > 0 && source->height > 0 &&
        source->pixels.size() == size_t(source->width) * source->height * BytesPerPixel(source->format);
    if (_envBuilt && _envSource.get() == (usable ? source : nullptr)) {
        return;
    }
    const EnvironmentMaps maps = BuildEnvironment(usable ? source : nullptr);
    _DestroyEnvironment();

    const VkDevice device = _device->device;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    ci.extent = {maps.size, maps.size, 1};
    ci.mipLevels = maps.levels;
    ci.arrayLayers = 6;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Check(vkCreateImage(device, &ci, nullptr, &_envImage), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, _envImage, &req);
    _envMemory = _device->Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindImageMemory(device, _envImage, _envMemory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = _envImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vci.format = ci.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, maps.levels, 0, 6};
    Check(vkCreateImageView(device, &vci, nullptr, &_envView), "vkCreateImageView");

    const size_t bytes = maps.data.size() * sizeof(float);
    _device->Reserve(_staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(_staging.mapped, maps.data.data(), bytes);
    _device->Submit([&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, maps.levels, 0, 6};
        ImageBarrier(cmd, _envImage, all, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        std::vector<VkBufferImageCopy> copies(maps.levels);
        VkDeviceSize offset = 0;
        for (uint32_t l = 0; l < maps.levels; ++l) {
            const uint32_t n = maps.size >> l;
            copies[l] = {};
            copies[l].bufferOffset = offset;
            copies[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 6};
            copies[l].imageExtent = {n, n, 1};
            offset += VkDeviceSize(6) * n * n * 16;
        }
        vkCmdCopyBufferToImage(cmd, _staging.buffer, _envImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               uint32_t(copies.size()), copies.data());
        ImageBarrier(cmd, _envImage, all,
                     VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    _envSource = usable ? dome->texture : nullptr;
    _envBuilt = true;
    _gpu.environmentLevels = maps.levels;
    std::memcpy(_gpu.environmentSh, maps.sh, sizeof(maps.sh));
}

void Scene::Commit()
{
    if (!_device) {
        return;
    }
    // First commit: create the pools so every descriptor has a buffer.
    if (!_vertices.buffer) {
        _EnsureCapacity(_vertices, _vertexAlloc, 0, sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
        _EnsureCapacity(_indices, _indexAlloc, 0, sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
        _EnsureCapacity(_instances, _instanceAlloc, 0, sizeof(Instance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        _EnsureCapacity(_skin, _skinAlloc, 0, sizeof(SkinVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        _EnsureCapacity(_joints, _jointAlloc, 0, 16 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        _EnsureCapacity(_skinXforms, _skinXformAlloc, 0, 16 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        false);
        _EnsureCapacity(_jointBoundsPool, _boundsAlloc, 0, sizeof(JointBounds), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        false);
        _EnsureCapacity(_blendOffsets, _blendAlloc, 0, 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        false);
        _EnsureCapacity(_blendWeights, _weightAlloc, 0, sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    }

    // Size the instance and index pools once for everything about to be
    // uploaded rather than growing them step by step: a huge instanced scene
    // (tens of millions of instances) cannot afford old and doubled pools
    // side by side.
    uint64_t instancesNeeded = 0, indicesNeeded = 0;
    for (const auto &m : _meshes) {
        instancesNeeded += m->transforms.size() / 16;
        indicesNeeded += !m->geometryDirty     ? m->_indexCount
                         : m->indices.empty() ? m->points.size() / 3
                                              : m->indices.size();
    }
    if (instancesNeeded > _instanceAlloc.Capacity()) {
        _EnsureCapacity(_instances, _instanceAlloc,
                        uint32_t(instancesNeeded + instancesNeeded / 16 - _instanceAlloc.Capacity()),
                        sizeof(Instance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
    if (indicesNeeded > _indexAlloc.Capacity()) {
        _EnsureCapacity(_indices, _indexAlloc, uint32_t(indicesNeeded + indicesNeeded / 16 - _indexAlloc.Capacity()),
                        sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);
    }

    // Upload only what changed. Rigid meshes that moved keep their previous
    // transforms for motion vectors (a new or re-flattened mesh has none).
    bool changed = _changed;
    _changed = false;
    _gpu.moved = false;
    std::vector<float> motion;
    std::vector<uint32_t> motionBase(_meshes.size(), kNone);
    for (const auto &m : _meshes) {
        const bool geometry = m->geometryDirty, transforms = m->transformsDirty || geometry;
        if (transforms && !geometry && m->jointIndices.empty() && !m->transforms.empty() &&
            m->prevTransforms.size() == m->transforms.size()) {
            motionBase[m->_slot] = uint32_t(motion.size() / 12);
            for (size_t i = 0; i < m->prevTransforms.size(); i += 16) {
                const float *x = &m->prevTransforms[i];  // as _UploadInstances' rows
                for (int r = 0; r < 3; ++r) {
                    motion.insert(motion.end(), {x[r], x[4 + r], x[8 + r], x[12 + r]});
                }
            }
        }
        m->prevTransforms.clear();
        const bool joints = !m->jointIndices.empty() && (m->jointsDirty || transforms);
        if (geometry) {
            _UploadGeometry(*m);
        }
        if (transforms) {
            _UploadInstances(*m);
        }
        if (joints) {
            _UploadJoints(*m);
        }
        if (transforms || joints) {
            _UpdateWorldBounds(*m);
        }
        changed = changed || geometry || transforms || joints;
        _gpu.moved = _gpu.moved || (joints && !geometry);
    }
    _gpu.moved = _gpu.moved || !motion.empty();
    _Flush();

    // Per-mesh tables, materials and textures are small: rebuilt every
    // commit from what visible meshes use (O(meshes), not O(instances)).
    _gpu.textures.clear();
    _textureSlots.clear();
    std::unordered_map<const Material *, uint32_t> materialSlots;
    std::vector<GpuMaterial> materials;
    std::vector<int32_t> udimTiles;
    std::vector<uint32_t> programCode;
    std::vector<float> programConstants;
    auto texRef = [&](const TextureRef &r) {
        GpuTexRef g{};
        if (r.udim.empty()) {
            g.index = _TextureIndex(r.texture.get());
        } else {
            g.index = int32_t(udimTiles.size());
            g.udim = 1;
            for (size_t t = 0; t < kUdimTiles; ++t) {
                udimTiles.push_back(t < r.udim.size() ? _TextureIndex(r.udim[t].get()) : -1);
            }
        }
        g.channel = r.channel;
        std::copy(r.scale, r.scale + 4, g.scale);
        std::copy(r.bias, r.bias + 4, g.bias);
        std::copy(r.uvTransform, r.uvTransform + 3, g.uvRows[0]);
        std::copy(r.uvTransform + 3, r.uvTransform + 6, g.uvRows[1]);
        return g;
    };
    auto materialIndex = [&](const Material &m) {
        const auto [it, inserted] = materialSlots.emplace(&m, uint32_t(materials.size()));
        if (inserted) {
            GpuMaterial g{};
            std::copy(m.baseColor, m.baseColor + 3, g.baseColor);
            g.baseColor[3] = m.baseColorFromDisplayColor ? 1.0f : 0.0f;
            std::copy(m.emissive, m.emissive + 3, g.emissive);
            g.emissive[3] = m.opacityThreshold;
            g.params[3] = m.opacity;
            g.params[0] = m.metallic;
            g.params[1] = m.roughness;
            g.params[2] = m.occlusion;
            const TextureRef *refs[7] = {&m.baseColorTex, &m.emissiveTex, &m.metallicTex, &m.roughnessTex,
                                         &m.occlusionTex,  &m.normalTex,   &m.opacityTex};
            for (int i = 0; i < 7; ++i) {
                g.tex[i] = texRef(*refs[i]);
            }
            std::copy(m.specular, m.specular + 3, g.specular);
            std::copy(m.specularColor, m.specularColor + 3, g.specularColor);
            g.specularColor[3] = m.specularWorkflow ? 1.0f : 0.0f;
            g.coat[0] = m.coat;
            g.coat[1] = m.coatRoughness;
            g.coat[2] = m.coatF0;
            std::copy(m.coatColor, m.coatColor + 3, g.coatColor);
            const MaterialProgram &p = m.program;
            if (!p.Empty() && p.Valid()) {
                g.program[0] = uint32_t(programCode.size() / 4);
                g.program[1] = uint32_t(p.code.size() / 4);
                g.program[2] = uint32_t(programConstants.size() / 4);
                g.program[3] = uint32_t((programConstants.size() + p.constants.size()) / 4);
                programCode.insert(programCode.end(), p.code.begin(), p.code.end());
                programConstants.insert(programConstants.end(), p.constants.begin(), p.constants.end());
                for (const TextureRef &image : p.images) {
                    const GpuTexRef r = texRef(image);
                    const size_t at = programConstants.size();
                    programConstants.resize(at + sizeof(GpuTexRef) / sizeof(float));
                    std::memcpy(&programConstants[at], &r, sizeof(r));
                }
            }
            materials.push_back(g);
        }
        return it->second;
    };

    const size_t slots = _meshes.size();
    const uint32_t capacity = _instanceAlloc.Capacity();
    _device->Reserve(_draws, slots * sizeof(DrawInfo), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    _device->Reserve(_commandTemplate, slots * sizeof(VkDrawIndexedIndirectCommand),
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto *draws = static_cast<DrawInfo *>(_draws.mapped);
    auto *templates = static_cast<VkDrawIndexedIndirectCommand *>(_commandTemplate.mapped);
    std::vector<uint32_t> drawn(slots);  // instances per slot before culling
    std::vector<uint32_t> primvarTable;  // uvec4 per program primvar slot
    std::vector<float> primvarData;
    std::vector<uint8_t> kind(slots, Gpu::kOpaque);
    _gpu.translucent.clear();
    _gpu.translucentVariant.clear();
    _gpu.translucentCenters.clear();
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};
    bool anyDrawable = false;
    for (size_t slot = 0; slot < slots; ++slot) {
        const Mesh &m = *_meshes[slot];
        // A skinned mesh without (valid) skinning transforms has nothing to draw.
        const bool drawable = m.visible && (m.tag & _tagMask) && m._indexCount > 0 && m._instanceCount > 0 &&
                              (m._skinCount == 0 || m._skinXformSlots > 0);
        DrawInfo &d = draws[slot];
        d = DrawInfo{};
        std::copy(m._boundsMin, m._boundsMin + 3, d.boundsMin);
        std::copy(m._boundsMax, m._boundsMax + 3, d.boundsMax);
        std::copy(m.color, m.color + 3, d.color);
        d.material = drawable && m.material ? materialIndex(*m.material) : kNoMaterial;
        d.motion = motionBase[slot];
        const bool program = drawable && m.material && !m.material->program.Empty() && m.material->program.Valid();
        const uint8_t variant = (m._skinStride == 2 ? Gpu::kWide : 0) |
                                (m._skinCount > 0 && m.dualQuaternion ? Gpu::kDualQuat : 0) |
                                (program ? Gpu::kProgram : 0) |
                                (drawable && m.material && m.material->Layered() ? Gpu::kLayers : 0);
        // Where each of the program's primvars is; a missing one reads the
        // program's fallback.
        d.primvars = kNone;
        d.primvarVertices = m._primvarVertexCount > 0 ? m._primvarVertexOffset : kNone;
        if (program && !m.material->program.primvars.empty()) {
            d.primvars = uint32_t(primvarTable.size() / 4);
            for (const std::string &name : m.material->program.primvars) {
                uint32_t e[4] = {0, 0, 0, 0};
                for (size_t p = 0; p < m.primvars.size(); ++p) {
                    const Primvar &pv = m.primvars[p];
                    if (pv.name != name) {
                        continue;
                    }
                    const size_t count = pv.rate == Primvar::Rate::Uniform    ? m._indexCount / 3
                                         : pv.rate == Primvar::Rate::Instance ? m._instanceCount
                                                                              : 1;
                    if (pv.rate == Primvar::Rate::Vertex) {
                        if (m._primvarVertexCount > 0 && p < m._primvarComponents.size() &&
                            m._primvarComponents[p] != kNone) {
                            const uint32_t v[4] = {uint32_t(pv.rate), pv.components, 0, m._primvarComponents[p]};
                            std::copy(v, v + 4, e);
                        }
                    } else if (pv.values.size() >= count * pv.components) {
                        const uint32_t v[4] = {uint32_t(pv.rate), pv.components, uint32_t(primvarData.size()), 0};
                        std::copy(v, v + 4, e);
                        primvarData.insert(primvarData.end(), pv.values.begin(),
                                           pv.values.begin() + count * pv.components);
                    }
                    break;
                }
                primvarTable.insert(primvarTable.end(), e, e + 4);
            }
        }
        if (drawable && m.material && m.material->opacityThreshold > 0) {
            kind[slot] = Gpu::kCutout;
        } else if (drawable && m.material &&
                   (m.material->opacity < 1 || m.material->opacityTex.texture ||
                    (program && m.material->program.Outputs(MaterialProgram::kOpacity)))) {
            kind[slot] = Gpu::kTranslucent;
            _gpu.translucent.push_back(uint32_t(slot));
            _gpu.translucentVariant.push_back(variant >> 2);
            for (int k = 0; k < 3; ++k) {
                _gpu.translucentCenters.push_back(0.5f * (m._worldMin[k] + m._worldMax[k]));
            }
        }
        kind[slot] |= variant;
        d.primId = m.primId;
        d.visible = drawable ? 1u : 0u;
        d.skin = m._skinCount > 0 ? m._skinOffset : kNone;
        d.vertexBase = m._vertexOffset;
        d.vertexColors = m.colors.empty() ? 0u : 1u;
        d.jointCount = m._skinCount > 0 ? m.jointCount : 0;
        d.skinXforms = m._skinXformBase;
        d.jointBounds = m._boundsBase;
        d.skinPerInstance = m._skinPerInstance ? 1u : 0u;
        d.blendWeights = m._weightBase;
        d.blendShapes = m._weightSlots > 0 ? m.blendShapeCount : 0;
        d.blendPerInstance = m._blendPerInstance ? 1u : 0u;
        // Culling fills in instanceCount.
        templates[slot] = {m._indexCount, 0, m._firstIndex, int32_t(m._vertexOffset), m._instanceBase};
        drawn[slot] = drawable ? m._instanceCount : 0;
        if (drawable) {
            anyDrawable = true;
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], m._worldMin[k]);
                hi[k] = std::max(hi[k], m._worldMax[k]);
            }
        }
    }
    // A call of 500 M triangles takes ~0.1 s on a GTX 1080 Ti; Moana's view
    // draws 11 G, which as one call ran ~2.2 s and reset the driver.
    // ponytail: a single slot over the budget stays one call; splitting it
    // needs instance ranges.
    constexpr double kChunkTriangles = 5e8;
    _gpu.drawChunks.assign(1, 0);
    _gpu.chunkKind.assign(1, Gpu::kOpaque);
    _gpu.instanceCount = _gpu.triangleCount = 0;
    double triangles = 0;
    for (size_t slot = 0; slot < slots; ++slot) {
        const double t = double(drawn[slot]) * double(_meshes[slot]->_indexCount / 3);
        _gpu.instanceCount += drawn[slot];
        _gpu.triangleCount += uint64_t(drawn[slot]) * (_meshes[slot]->_indexCount / 3);
        if (t == 0) {
            continue;  // draws nothing: fits any chunk
        }
        if (triangles > 0 && (triangles + t > kChunkTriangles || kind[slot] != _gpu.chunkKind.back())) {
            _gpu.drawChunks.push_back(uint32_t(slot));
            _gpu.chunkKind.push_back(kind[slot]);
            triangles = 0;
        }
        _gpu.chunkKind.back() = kind[slot];
        triangles += t;
    }
    _gpu.drawChunks.push_back(uint32_t(slots));

    // Visibility edits change what shadows draw without any upload.
    if (changed || drawn != _lastDrawn) {
        static std::atomic<uint64_t> versions{0};
        _gpu.version = ++versions;
        _lastDrawn = std::move(drawn);
    }
    _gpu.hasBounds = anyDrawable;
    std::copy(lo, lo + 3, _gpu.boundsMin);
    std::copy(hi, hi + 3, _gpu.boundsMax);

    _device->Reserve(_materials, materials.size() * sizeof(GpuMaterial),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(materials.begin(), materials.end(), static_cast<GpuMaterial *>(_materials.mapped));
    _device->Reserve(_udimTiles, std::max<size_t>(udimTiles.size(), 1) * sizeof(int32_t),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(udimTiles.begin(), udimTiles.end(), static_cast<int32_t *>(_udimTiles.mapped));
    _device->Reserve(_motion, std::max<size_t>(motion.size(), 4) * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(motion.begin(), motion.end(), static_cast<float *>(_motion.mapped));
    // ponytail: program primvars are gathered every commit (O(their
    // instances)); keep them per mesh if huge instanced programs show up.
    auto upload = [&](Buffer &b, const auto &v) {
        using T = typename std::decay_t<decltype(v)>::value_type;
        _device->Reserve(b, std::max<size_t>(v.size(), 4) * sizeof(T), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::copy(v.begin(), v.end(), static_cast<T *>(b.mapped));
    };
    upload(_programCode, programCode);
    upload(_programConstants, programConstants);
    upload(_primvarTable, primvarTable);
    upload(_primvarData, primvarData);
    if (!_primvarVertices.buffer) {  // bound even when no mesh has vertex-rate primvars
        _EnsureCapacity(_primvarVertices, _primvarVertexAlloc, 1, 4 * sizeof(float),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    }

    _device->Reserve(_lightBuffer, _lights.size() * sizeof(GpuLight),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto *lights = static_cast<GpuLight *>(_lightBuffer.mapped);
    uint32_t lightCount = 0, localLightCount = 0;
    _gpu.shadowLight = kNone;
    _gpu.shadowCasters.clear();
    for (const bool distant : {true, false}) {  // distant lights first
        for (const auto &l : _lights) {
            if (!l->visible || (l->type == Light::Type::Distant) != distant) {
                continue;
            }
            GpuLight &g = lights[lightCount + localLightCount];
            g = GpuLight{};
            g.halfHeight[3] = l->diffuse;
            g.shaping[3] = l->specular;
            if (distant) {
                if (l->castShadows && _gpu.shadowLight == kNone) {
                    _gpu.shadowLight = lightCount;
                    std::copy(l->direction, l->direction + 3, _gpu.shadowDirection);
                }
                std::copy(l->direction, l->direction + 3, g.position);
                std::copy(l->irradiance, l->irradiance + 3, g.color);
                ++lightCount;
                continue;
            }
            const bool sphere = l->type == Light::Type::Sphere;
            std::copy(l->position, l->position + 3, g.position);
            g.position[3] = sphere ? 1.0f : l->type == Light::Type::Rect ? 2.0f : 3.0f;
            std::copy(l->radiance, l->radiance + 3, g.color);
            std::copy(l->direction, l->direction + 3, g.axis);
            g.axis[3] = sphere ? l->radius : 0.0f;
            std::copy(l->halfWidth, l->halfWidth + 3, g.halfWidth);
            g.halfWidth[3] = sphere && !l->point ? l->radius : 0.0f;
            std::copy(l->halfHeight, l->halfHeight + 3, g.halfHeight);
            const float cone = l->coneAngle * 0.0174532925f;
            g.shaping[0] = cone * (1.0f - std::clamp(l->coneSoftness, 0.0f, 1.0f));
            g.shaping[1] = cone;
            g.shaping[2] = l->focus;
            std::copy(l->focusTint, l->focusTint + 3, g.focusTint);
            if (l->castShadows) {
                // Mirrors LightRange() in shaders/lights.glsl.
                Scene::Gpu::ShadowCaster c{};
                c.light = lightCount + localLightCount;
                std::copy(l->position, l->position + 3, c.position);
                std::copy(l->direction, l->direction + 3, c.axis);
                c.coneAngle = cone;
                const float radiance = std::max({l->radiance[0], l->radiance[1], l->radiance[2]}) *
                                       std::max(l->diffuse, l->specular);
                if (sphere) {
                    c.rangeIntensity = radiance * l->radius * l->radius;
                    c.radius = l->radius;
                    c.size = l->point ? 0.0f : l->radius;
                } else {
                    const float cx = l->halfWidth[1] * l->halfHeight[2] - l->halfWidth[2] * l->halfHeight[1];
                    const float cy = l->halfWidth[2] * l->halfHeight[0] - l->halfWidth[0] * l->halfHeight[2];
                    const float cz = l->halfWidth[0] * l->halfHeight[1] - l->halfWidth[1] * l->halfHeight[0];
                    const float area = (l->type == Light::Type::Rect ? 4.0f : 3.14159265f) *
                                       std::sqrt(cx * cx + cy * cy + cz * cz);
                    c.rangeIntensity = radiance * area / 3.14159265f;
                    c.size = std::sqrt(area / 3.14159265f);
                    const float ex = l->halfWidth[0] + l->halfHeight[0], ey = l->halfWidth[1] + l->halfHeight[1],
                                ez = l->halfWidth[2] + l->halfHeight[2];
                    c.extent = std::sqrt(ex * ex + ey * ey + ez * ez);
                }
                _gpu.shadowCasters.push_back(c);
            }
            ++localLightCount;
        }
    }

    const auto dome = std::find_if(_domes.begin(), _domes.end(),
                                   [](const auto &d) { return d->visible; });
    _gpu.environment = VK_NULL_HANDLE;
    _gpu.environmentLatlong = kNone;
    if (dome != _domes.end()) {
        _UpdateEnvironment(dome->get());
        _gpu.environment = _envView;
        std::copy((*dome)->radiance, (*dome)->radiance + 3, _gpu.environmentRadiance);
        std::copy((*dome)->worldToLocal, (*dome)->worldToLocal + 16, _gpu.environmentWorldToLocal);
        const int32_t index = _TextureIndex((*dome)->texture.get());
        _gpu.environmentLatlong = index < 0 ? kNone : uint32_t(index);
    }

    _gpu.vertices = _vertices.buffer;
    _gpu.indices = _indices.buffer;
    _gpu.instances = _instances.buffer;
    _gpu.visible = _visible.buffer;
    _gpu.draws = _draws.buffer;
    _gpu.commandTemplate = _commandTemplate.buffer;
    _gpu.materials = _materials.buffer;
    _gpu.programCode = _programCode.buffer;
    _gpu.programConstants = _programConstants.buffer;
    _gpu.primvarTable = _primvarTable.buffer;
    _gpu.primvarData = _primvarData.buffer;
    _gpu.primvarVertices = _primvarVertices.buffer;
    _gpu.udimTiles = _udimTiles.buffer;
    _gpu.lights = _lightBuffer.buffer;
    _gpu.skin = _skin.buffer;
    _gpu.joints = _joints.buffer;
    _gpu.jointsBytes = _joints.size;
    _gpu.motion = _motion.buffer;
    _gpu.skinXforms = _skinXforms.buffer;
    _gpu.jointBounds = _jointBoundsPool.buffer;
    _gpu.blendOffsets = _blendOffsets.buffer;
    _gpu.blendWeights = _blendWeights.buffer;
    _gpu.drawCount = uint32_t(slots);
    _gpu.instanceCapacity = capacity;
    _gpu.lightCount = lightCount;
    _gpu.localLightCount = localLightCount;
}

} // namespace vkr
