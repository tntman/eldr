#include "scene.h"

#include "vkmath.h"
#include "lod.h"

#include <algorithm>
#include <tuple>
#include <chrono>
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
    float sheen[4];          // weight, roughness
    float sheenColor[4];
    float thinFilm[4];       // weight, thickness (nm), ior
    float anisotropy[4];     // amount, rotation (turns), 1: OpenPBR's mapping
    float toon[4];           // bands, softness
    float transmission[4];  // Material::transmissionColor, transmission
    float refraction[4];    // Material::ior, thinWalled, transmissionDepth, opacityRefracts
    float subsurface[4];        // Material::subsurfaceColor, subsurface
    float subsurfaceRadius[4];  // Material::subsurfaceRadius
};
static_assert(sizeof(GpuTexRef) == 5 * 16, "kImage reads GpuTexRefs as five vec4 constants");
struct GpuDisplacement {  // mirror of Displacement in shaders/displace.glsl
    float params[4];      // Material::displacement, displacementBound
    GpuTexRef tex;
    uint32_t program[4];  // Material::displacementProgram, as GpuMaterial::program (count 0: none)
};
struct GpuLod {           // mirror of Lod in shaders/cull.comp
    uint32_t slots[4];    // level 1's command slot (2 and 3 follow), levels
    uint32_t regions[4];  // where levels 1, 2, 3 fill from (Scene::_ReserveLodInstances)
    float errors[4];      // levels 1..3, relative to the bounding radius
};
struct GpuLight {        // mirror of Light in shaders/lights.glsl
    float position[4];   // distant: direction to the light; else center. w: type
    float color[4];      // distant: irradiance; else surface radiance
    float axis[4];       // emission axis; w: sphere/cylinder radius, distant: tan of the sun's angular radius
    float halfWidth[4];  // rect; w: sphere shading radius (0 = point)
    float halfHeight[4]; // rect; w: diffuse multiplier
    float shaping[4];    // soft cone angle, cone angle (radians), focus, specular multiplier
    float focusTint[4] = {0, 0, 0, -1};  // w: rect/disk: the texture's bindless index (-1: none)
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
    float velocity[3];       // Mesh::velocities, zero without
    bool operator==(const VertexKey &o) const { return std::memcmp(this, &o, sizeof(VertexKey)) == 0; }
};
struct VertexKeyHash {
    // Eight bytes a step (FNV-1a byte by byte was a good part of a big
    // scene's upload time).
    size_t operator()(const VertexKey &k) const
    {
        uint64_t h = 1469598103934665603ull;
        const auto *p = reinterpret_cast<const uint8_t *>(&k);
        for (size_t i = 0; i < sizeof(VertexKey); i += 8) {
            uint64_t w = 0;
            std::memcpy(&w, p + i, std::min<size_t>(8, sizeof(VertexKey) - i));
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
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

// ToHalf's inverse (which has no tiny values).
float FromHalf(uint16_t h)
{
    const int exponent = h >> 10 & 0x1F;
    const float m = exponent == 0 ? 0.0f : std::ldexp(float((h & 0x3FF) | 0x400), exponent - 25);
    return h & 0x8000 ? -m : m;
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
        _device->WaitFrame();
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
                          &_materials, &_udimTiles, &_motion, &_lightBuffer, &_staging, &_primvarVertices, &_velocities,
                          &_programCode, &_programConstants, &_primvarTable, &_primvarData, &_volumeTable,
                          &_volumeGrids}) {
            _device->Destroy(*b);
        }
        _device->Destroy(_displacements);
        _device->Destroy(_lodTable);
        _DestroyEnvironment();
    }
}

bool Scene::SetTagMask(uint32_t mask)
{
    const bool changed = mask != _tagMask;
    _tagMask = mask;
    return changed;
}

std::vector<Scene::SkinnedInstance> Scene::LargestSkinned(const float eye[3], uint32_t count) const
{
    std::vector<std::pair<float, SkinnedInstance>> found;
    for (const auto &mp : _meshes) {
        const Mesh &m = *mp;
        if (m._skinCount == 0 || m._slot >= _gpu.slotGeometry.size() || _gpu.slotGeometry[m._slot].version == 0 ||
            m._posedMin[0] > m._posedMax[0]) {
            continue;
        }
        const uint32_t flags = (m._skinStride == 2 ? 1u : 0u) | (m.dualQuaternion ? 2u : 0u);
        float c[3], r2 = 0;
        for (int k = 0; k < 3; ++k) {
            c[k] = 0.5f * (m._posedMin[k] + m._posedMax[k]);
            r2 += 0.25f * (m._posedMax[k] - m._posedMin[k]) * (m._posedMax[k] - m._posedMin[k]);
        }
        for (uint32_t i = 0; i < m._instanceCount; ++i) {
            const float *x = &m.transforms[size_t(i) * 16];  // row-vector order
            float d2 = 0;
            for (int k = 0; k < 3; ++k) {
                const float w = c[0] * x[k] + c[1] * x[4 + k] + c[2] * x[8 + k] + x[12 + k] - eye[k];
                d2 += w * w;
            }
            const float scale2 = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
            found.push_back({r2 * scale2 / std::max(d2, 1e-12f), {m._instanceBase + i, uint32_t(m._slot), flags}});
        }
    }
    const size_t n = std::min<size_t>(count, found.size());
    std::partial_sort(found.begin(), found.begin() + n, found.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<SkinnedInstance> largest;
    for (size_t i = 0; i < n; ++i) {
        largest.push_back(found[i].second);
    }
    return largest;
}

std::vector<Scene::DisplacedInstance> Scene::LargestDisplacedInstances(const float eye[3], uint32_t count) const
{
    std::vector<std::pair<float, DisplacedInstance>> found;
    for (const auto &mp : _meshes) {
        const Mesh &m = *mp;
        if (!m._displacePerInstance || m._slot >= _gpu.slotGeometry.size() ||
            _gpu.slotGeometry[m._slot].version == 0) {
            continue;
        }
        float c[3], r2 = 0;
        for (int k = 0; k < 3; ++k) {
            c[k] = 0.5f * (m._boundsMin[k] + m._boundsMax[k]);
            r2 += 0.25f * (m._boundsMax[k] - m._boundsMin[k]) * (m._boundsMax[k] - m._boundsMin[k]);
        }
        for (uint32_t i = 0; i < m._instanceCount; ++i) {
            const float *x = &m.transforms[size_t(i) * 16];  // row-vector order
            float d2 = 0;
            for (int k = 0; k < 3; ++k) {
                const float w = c[0] * x[k] + c[1] * x[4 + k] + c[2] * x[8 + k] + x[12 + k] - eye[k];
                d2 += w * w;
            }
            const float scale2 = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
            // FNV-1a over the bits of its transform and its instance-rate
            // primvars' values (what a per-instance graph reads).
            uint64_t key = 1469598103934665603ull;
            auto hash = [&](const float *v, size_t n) {
                for (size_t k = 0; k < n; ++k) {
                    uint32_t bits;
                    std::memcpy(&bits, &v[k], sizeof(bits));
                    key = (key ^ bits) * 1099511628211ull;
                }
            };
            hash(x, 16);
            for (const Primvar &pv : m.primvars) {
                if (pv.rate == Primvar::Rate::Instance && pv.values.size() >= size_t(i + 1) * pv.components) {
                    hash(&pv.values[size_t(i) * pv.components], pv.components);
                }
            }
            found.push_back({r2 * scale2 / std::max(d2, 1e-12f), {m._instanceBase + i, uint32_t(m._slot), key}});
        }
    }
    const size_t n = std::min<size_t>(count, found.size());
    std::partial_sort(found.begin(), found.begin() + n, found.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<DisplacedInstance> largest;
    for (size_t i = 0; i < n; ++i) {
        largest.push_back(found[i].second);
    }
    return largest;
}

bool Scene::SetDomeSun(bool on, float threshold)
{
    const bool changed = on != _domeSunOn || (on && threshold != _domeSunThreshold);
    _domeSunOn = on;
    _domeSunThreshold = threshold;
    return changed;
}

bool Scene::SetEmissiveLights(bool on, float threshold)
{
    const bool changed = on != _emissiveLights || threshold != _emissiveThreshold;
    _emissiveLights = on;
    _emissiveThreshold = threshold;
    return changed;
}

// An instance of an emissive mesh as lights: a flat one (a panel) as a
// rect light on each face, of its radiance; any other as a sphere light
// around it of the same power.
struct Scene::Emitter {
    const Mesh *mesh;
    bool flat;
    float center[3];
    float radius;                          // sphere
    float thin[3], wide[3], high[3];       // flat: half axes, thin one the faces' normal
    float radiance[3];                     // of the light's surface
    float power;                           // luminous, for picking the brightest
};

// Emissive meshes promoted to lights (SetEmissiveLights), at most
// kMaxEmitters instances, the most powerful first. Small means a bounding
// sphere under a quarter of the scene's diagonal: big emitters light the
// scene through GI only. The lights never see the emitter itself: a
// sphere bounds it (its shadow maps' near plane and its shadow rays, aimed
// at the cap facing the receiver, stay outside), rects lie just off its
// faces.
// ponytail: constant emissive colours only (textured and program-driven
// emission is left to GI), rigid meshes only, panels' edges don't emit,
// and the lights are diffuse (highlights come from reflections of the
// emitter).
std::vector<Scene::Emitter> Scene::_FindEmitters()
{
    constexpr size_t kMaxEmitters = 32;
    std::vector<Emitter> found;
    if (!_emissiveLights || !_gpu.hasBounds) {
        return found;
    }
    float diagonal = 0;
    for (int k = 0; k < 3; ++k) {
        diagonal += (_gpu.boundsMax[k] - _gpu.boundsMin[k]) * (_gpu.boundsMax[k] - _gpu.boundsMin[k]);
    }
    const float maxRadius = 0.25f * std::sqrt(diagonal);
    for (const auto &mp : _meshes) {
        Mesh &m = *mp;
        const Material *mat = m.material.get();
        if (!mat || mat->emissiveTex.texture || !mat->emissiveTex.udim.empty() ||
            (!mat->program.Empty() && mat->program.Outputs(MaterialProgram::kEmissive)) ||
            std::max({mat->emissive[0], mat->emissive[1], mat->emissive[2]}) * mat->opacity < _emissiveThreshold ||
            !m.visible || !(m.tag & _tagMask) || m._indexCount == 0 || m._instanceCount == 0 || m._skinCount > 0) {
            continue;
        }
        if (m._emitterAreaVersion != m._geometryVersion) {
            // Object-space area of the triangles, per corner or indexed.
            const bool indexed = !m.indices.empty();
            const size_t corners = indexed ? m.indices.size() : m.points.size() / 3;
            double area = 0;
            for (size_t t = 0; t + 2 < corners; t += 3) {
                const float *p[3];
                for (int k = 0; k < 3; ++k) {
                    p[k] = &m.points[size_t(indexed ? m.indices[t + k] : t + k) * 3];
                }
                const float a[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
                const float b[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
                const float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
                area += 0.5 * std::sqrt(double(c[0]) * c[0] + double(c[1]) * c[1] + double(c[2]) * c[2]);
            }
            m._emitterArea = float(area);
            m._emitterAreaVersion = m._geometryVersion;
        }
        float emitted[3];
        for (int k = 0; k < 3; ++k) {
            emitted[k] = mat->emissive[k] * mat->opacity;
        }
        const float luminance = 0.2126f * emitted[0] + 0.7152f * emitted[1] + 0.0722f * emitted[2];
        for (uint32_t i = 0; i < m._instanceCount; ++i) {
            // The object bounds as an oriented box: center and half axes
            // (row-vector transform: p' = p * M).
            const float *x = &m.transforms[size_t(i) * 16];
            Emitter e{&m};
            float axes[3][3], length[3];
            for (int k = 0; k < 3; ++k) {
                const float c = 0.5f * (m._boundsMin[k] + m._boundsMax[k]), h = 0.5f * (m._boundsMax[k] - m._boundsMin[k]);
                for (int j = 0; j < 3; ++j) {
                    e.center[j] += c * x[k * 4 + j];
                    axes[k][j] = h * x[k * 4 + j];
                }
                length[k] = std::sqrt(axes[k][0] * axes[k][0] + axes[k][1] * axes[k][1] + axes[k][2] * axes[k][2]);
            }
            for (int j = 0; j < 3; ++j) {
                e.center[j] += x[12 + j];
            }
            e.radius = std::sqrt(length[0] * length[0] + length[1] * length[1] + length[2] * length[2]);
            if (!(e.radius > 0.0f) || e.radius > maxRadius) {
                continue;
            }
            // Flat: the thinnest axis under a fifth of the next one.
            const int thin = int(std::min_element(length, length + 3) - length);
            const int wide = (thin + 1) % 3, high = (thin + 2) % 3;
            e.flat = length[thin] < 0.2f * std::min(length[wide], length[high]);
            float area;
            if (e.flat) {
                std::copy(axes[thin], axes[thin] + 3, e.thin);
                std::copy(axes[wide], axes[wide] + 3, e.wide);
                std::copy(axes[high], axes[high] + 3, e.high);
                area = 8.0f * length[wide] * length[high];  // both faces
                std::copy(emitted, emitted + 3, e.radiance);
            } else {
                // World area: scaled by the transform's area scale (det^(2/3)).
                const float det = x[0] * (x[5] * x[10] - x[6] * x[9]) - x[1] * (x[4] * x[10] - x[6] * x[8]) +
                                  x[2] * (x[4] * x[9] - x[5] * x[8]);
                area = m._emitterArea * std::pow(std::abs(det), 2.0f / 3.0f);
                for (int k = 0; k < 3; ++k) {
                    e.radiance[k] = emitted[k] * area / (4.0f * 3.14159265f * e.radius * e.radius);
                }
            }
            e.power = luminance * area;
            found.push_back(e);
        }
    }
    if (found.size() > kMaxEmitters) {
        std::nth_element(found.begin(), found.begin() + kMaxEmitters, found.end(),
                         [](const Emitter &a, const Emitter &b) { return a.power > b.power; });
        found.resize(kMaxEmitters);
    }
    return found;
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
                            &_blendOffsets, &_blendWeights, &_primvarVertices, &_velocities}) {
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
        _velocityAlloc.Free(mesh->_velocityOffset, mesh->_velocityCount);
        _indexAlloc.Free(mesh->_lodFirstIndex, mesh->_lodIndexCount);
        _FreeInstances(*mesh);
        for (PendingLods &p : _pendingLods) {
            p.mesh = p.mesh == mesh ? nullptr : p.mesh;  // finishes, then is dropped
        }
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

Volume *Scene::AddVolume()
{
    _volumes.push_back(std::make_unique<Volume>());
    _volumes.back()->_slot = _volumes.size() - 1;
    return _volumes.back().get();
}

void Scene::RemoveVolume(Volume *volume)
{
    _RemoveSlot(_volumes, volume);
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
    if (&buffer == &_vertices || &buffer == &_indices) {
        usage |= _device->rayTracingInputUsage;  // BLAS inputs
    }
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
    _velocityAlloc.Free(m._velocityOffset, m._velocityCount);
    _indexAlloc.Free(m._lodFirstIndex, m._lodIndexCount);
    m._vertexCount = m._indexCount = m._skinCount = m._blendSlots = m._primvarVertexCount = m._lodIndexCount = 0;
    m._velocityCount = 0;
    m._lods.clear();
    ++m._lodGeneration;  // levels still building for the last upload don't fit
    m._jointBounds.clear();
    m.geometryDirty = false;
    static std::atomic<uint64_t> geometryVersions{0};
    m._geometryVersion = ++geometryVersions;

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
    const bool moving = !skinned && m.velocities.size() == m.points.size();
    // Planar reflections: is the mesh flat? Its first proper triangle,
    // and every point within a thousandth of its size of that plane.
    m._flat = false;
    if (!skinned && m.blendRanges.empty()) {
        auto point = [&](size_t corner) {
            const size_t e = indexed ? m.indices[corner] : corner;
            return Vec3{m.points[e * 3], m.points[e * 3 + 1], m.points[e * 3 + 2]};
        };
        float lo[3] = {m.points[0], m.points[1], m.points[2]}, hi[3] = {lo[0], lo[1], lo[2]};
        for (size_t i = 0; i < m.points.size(); ++i) {
            lo[i % 3] = std::min(lo[i % 3], m.points[i]);
            hi[i % 3] = std::max(hi[i % 3], m.points[i]);
        }
        const float size = Length(Vec3{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
        for (size_t t = 0; t < corners && !m._flat; t += 3) {
            const Vec3 a = point(t), b = point(t + 1), c = point(t + 2);
            const Vec3 n = Cross(b - a, c - a);
            if (Length(n) > 1e-6f * size * size) {
                const Vec3 u = Normalize(n);
                m._flat = true;
                for (size_t k = 0; k < m.points.size() && m._flat; k += 3) {
                    m._flat = std::abs(Dot(Vec3{m.points[k], m.points[k + 1], m.points[k + 2]} - a, u)) <= 1e-3f * size;
                }
                const Vec3 tri[3] = {a, b, c};
                for (int k = 0; k < 3; ++k) {
                    m._flatTriangle[k * 3] = tri[k].x;
                    m._flatTriangle[k * 3 + 1] = tri[k].y;
                    m._flatTriangle[k * 3 + 2] = tri[k].z;
                }
                break;
            }
        }
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
    std::vector<float> velocities;       // 4 per vertex, with Mesh::velocities
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
        if (moving) {
            std::copy_n(&m.velocities[i * 3], 3, key.velocity);
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
            if (moving) {
                velocities.insert(velocities.end(), {key.velocity[0], key.velocity[1], key.velocity[2], 0.0f});
            }
            if (skinned) {
                skin.push_back(key.skin);
                if (m.influences == 8) {
                    skin.push_back(key.skin2);
                }
            }
            // Camera-facing: as far as the half width or radius reaches.
            // Moving points: a frame's velocity either way.
            // ponytail: a shutter past one frame from this one leaves the bounds.
            const float reach = m.facing == Mesh::kNotFacing
                                    ? Length(Vec3{key.velocity[0], key.velocity[1], key.velocity[2]})
                                    : Length(Vec3{key.v.normal[0], key.v.normal[1], key.v.normal[2]});
            for (int k = 0; k < 3; ++k) {
                m._boundsMin[k] = std::min(m._boundsMin[k], key.v.position[k] - reach);
                m._boundsMax[k] = std::max(m._boundsMax[k], key.v.position[k] + reach);
            }
        }
    }

    // The mean uv length of an edge: a texture's footprint at a vertex
    // (displace.glsl's mip level).
    double uvLength = 0.0;
    for (size_t c = 0; !m.uvs.empty() && c < corners; ++c) {
        const float *a = vertices[indices[c]].uv, *b = vertices[indices[c % 3 == 2 ? c - 2 : c + 1]].uv;
        uvLength += std::hypot(double(b[0]) - a[0], double(b[1]) - a[1]);
    }
    m._uvSpacing = corners > 0 ? float(uvLength / double(corners)) : 0.0f;
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
    // Levels of detail of heavy meshes (lod.h), built on other threads; a
    // later Commit stores them (the mesh draws whole until then).
    if (indexCount / 3 >= kLodMinTriangles && m.facing == Mesh::kNotFacing) {
        // Position, then the shading attributes as distances (see BuildLods):
        // normals and colours relative to the bounding radius (a turn of
        // 1/64 radian on an object 64 pixels in radius costs a pixel), uvs
        // by how far they slide the texture over the surface (its area per
        // uv area).
        const float radius = 0.5f * Length(Vec3{m._boundsMax[0] - m._boundsMin[0], m._boundsMax[1] - m._boundsMin[1],
                                                m._boundsMax[2] - m._boundsMin[2]});
        double area = 0.0, uvArea = 0.0;
        for (size_t t = 0; !m.uvs.empty() && t + 2 < indices.size(); t += 3) {
            const Vertex &a = vertices[indices[t]], &b = vertices[indices[t + 1]], &c = vertices[indices[t + 2]];
            const Vec3 pa{a.position[0], a.position[1], a.position[2]};
            area += Length(Cross(Vec3{b.position[0], b.position[1], b.position[2]} - pa,
                                 Vec3{c.position[0], c.position[1], c.position[2]} - pa));
            uvArea += std::abs((b.uv[0] - a.uv[0]) * (c.uv[1] - a.uv[1]) - (b.uv[1] - a.uv[1]) * (c.uv[0] - a.uv[0]));
        }
        const float uvScale = uvArea > 0.0 ? float(std::sqrt(area / uvArea)) : 0.0f;
        const size_t stride = 3 + (m.normals.empty() ? 0 : 3) + (uvScale > 0.0f ? 2 : 0) + (m.colors.empty() ? 0 : 3);
        std::vector<float> lodVertices(vertices.size() * stride);
        for (size_t v = 0; v < vertices.size(); ++v) {
            float *out = std::copy_n(vertices[v].position, 3, &lodVertices[v * stride]);
            for (int k = 0; k < 3 && !m.normals.empty(); ++k) {
                *out++ = vertices[v].normal[k] * radius * kLodNormalWeight;
            }
            for (int k = 0; k < 2 && uvScale > 0.0f; ++k) {
                *out++ = vertices[v].uv[k] * uvScale;
            }
            for (int k = 0; k < 3 && !m.colors.empty(); ++k) {
                *out++ = FromHalf(vertices[v].color[k]) * radius * kLodColorWeight;
            }
        }
        // Skinned: each vertex's influences (joint, weight), m.influences each.
        std::vector<std::pair<uint32_t, float>> influences;
        for (size_t v = 0; skinned && v < vertices.size(); ++v) {
            for (uint32_t k = 0; k < m.influences; ++k) {
                const SkinVertex &sv = skin[v * (m.influences == 8 ? 2 : 1) + k / 4];
                const uint32_t pair = sv.weights[(k % 4) / 2] >> (16 * (k % 2)) & 0xFFFFu;
                influences.push_back({sv.joints[(k % 4) / 2] >> (16 * (k % 2)) & 0xFFFFu, float(pair) / 65535.0f});
            }
        }
        const uint32_t n = m.influences;
        _pendingLods.push_back(
            {&m, m._lodGeneration,
             std::async(std::launch::async, [lodVertices = std::move(lodVertices), stride, indices,
                                             influences = std::move(influences), n] {
                 // Half the L1 distance of two vertices' joint weights.
                 auto difference = [&](uint32_t a, uint32_t b) {
                     float d = 0.0f;
                     for (uint32_t i = 0; i < n; ++i) {
                         const auto [joint, weight] = influences[size_t(a) * n + i];
                         float other = 0.0f;
                         for (uint32_t k = 0; k < n; ++k) {
                             other += influences[size_t(b) * n + k].first == joint ? influences[size_t(b) * n + k].second
                                                                                   : 0.0f;
                         }
                         d += std::abs(weight - other);
                     }
                     for (uint32_t k = 0; k < n; ++k) {  // b's joints a lacks
                         const auto [joint, weight] = influences[size_t(b) * n + k];
                         bool shared = false;
                         for (uint32_t i = 0; i < n; ++i) {
                             shared = shared || influences[size_t(a) * n + i].first == joint;
                         }
                         d += shared ? 0.0f : weight;
                     }
                     return 0.5f * d;
                 };
                 return BuildLods(lodVertices.data(), stride, lodVertices.size() / stride, indices, kMaxLods, kLodMaxError,
                                  influences.empty() ? std::function<float(uint32_t, uint32_t)>() : difference);
             })});
    }
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
    if (moving) {
        while (!_velocityAlloc.Allocate(vertexCount, &m._velocityOffset)) {
            _EnsureCapacity(_velocities, _velocityAlloc, vertexCount, 4 * sizeof(float),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        }
        m._velocityCount = vertexCount;
        _copies.push_back({_velocities.buffer, VkDeviceSize(m._velocityOffset) * 4 * sizeof(float),
                           _Stage(velocities.data(), velocities.size() * sizeof(float)),
                           velocities.size() * sizeof(float)});
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

// A mesh's levels of detail (BuildLods' output): their triangles after its
// own in the index pool.
void Scene::_StoreLods(Mesh &m, std::vector<LodLevel> levels)
{
    std::vector<uint32_t> indices;
    for (const LodLevel &level : levels) {
        m._lods.push_back({uint32_t(indices.size()), uint32_t(level.indices.size()), level.error});
        indices.insert(indices.end(), level.indices.begin(), level.indices.end());
    }
    m._lodIndexCount = uint32_t(indices.size());
    while (!_indexAlloc.Allocate(m._lodIndexCount, &m._lodFirstIndex)) {
        _EnsureCapacity(_indices, _indexAlloc, m._lodIndexCount, sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        false);
    }
    for (Mesh::Lod &lod : m._lods) {
        lod.firstIndex += m._lodFirstIndex;
    }
    if (!indices.empty()) {
        _copies.push_back({_indices.buffer, VkDeviceSize(m._lodFirstIndex) * sizeof(uint32_t),
                           _Stage(indices.data(), indices.size() * sizeof(uint32_t)),
                           indices.size() * sizeof(uint32_t)});
    }
    _ReserveLodInstances(m);
}

// Each level of detail fills a range of the visible lists of its own
// (Gpu::lods), room for every instance (a crossfading one is in two
// levels): instance slots that stay invalid.
// ponytail: past kLodMaxInstances instances a mesh has no levels, so huge
// instancing doesn't multiply its pools.
void Scene::_ReserveLodInstances(Mesh &m)
{
    const uint32_t slots = m._instanceCount <= kLodMaxInstances ? m._instanceCount * uint32_t(m._lods.size()) : 0;
    if (slots != m._lodInstanceCount) {
        _instanceAlloc.Free(m._lodInstanceBase, m._lodInstanceCount);
        while (!_instanceAlloc.Allocate(slots, &m._lodInstanceBase)) {
            _EnsureCapacity(_instances, _instanceAlloc, slots, sizeof(Instance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            true);
        }
        m._lodInstanceCount = slots;
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
    _instanceAlloc.Free(m._lodInstanceBase, m._lodInstanceCount);  // never written: still invalid
    m._lodInstanceCount = 0;
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
    _ReserveLodInstances(m);
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

// A texture read with other wrap modes takes another bindless slot (same
// view, another sampler).
int32_t Scene::_TextureIndex(Texture *t, uint32_t wrap)
{
    if (!t || t->width == 0 || t->height == 0 ||
        t->pixels.size() != size_t(t->width) * t->height * BytesPerPixel(t->format)) {
        return -1;
    }
    const auto found = _textureSlots.find({t, wrap});
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
    _gpu.textureWraps.push_back(uint8_t(wrap));
    _textureSlots.emplace(std::make_pair(t, wrap), index);
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
    // The sun's threshold in texture units (the dome multiplies them).
    const float scale = 0.2126f * dome->radiance[0] + 0.7152f * dome->radiance[1] + 0.0722f * dome->radiance[2];
    const float threshold = _domeSunOn && usable && scale > 0.0f ? _domeSunThreshold / scale : 0.0f;
    if (_envBuilt && _envSource.get() == (usable ? source : nullptr) && threshold == _envThreshold) {
        return;
    }
    const EnvironmentMaps maps = BuildEnvironment(usable ? source : nullptr, threshold, &_domeSun);
    _envThreshold = threshold;
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

void Scene::Commit(bool keepMotion)
{
    if (!_device) {
        return;
    }
    _device->WaitFrame();  // the pools are host memory the frame in flight reads
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
        if (transforms && (!geometry || m->shutterMotion) && m->jointIndices.empty() && !m->transforms.empty() &&
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
        m->shutterMotion = false;
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
        // New points with velocities (deformation blur) move too.
        _gpu.moved = _gpu.moved || (joints && !geometry) || (geometry && m->_velocityCount > 0);
    }
    _gpu.moved = _gpu.moved || !motion.empty();
    // A second commit for the same frame (keepMotion) keeps the first's
    // motion when it moved nothing itself.
    if (keepMotion && !_gpu.moved && _keptMotionBase.size() == motionBase.size()) {
        motion = _keptMotion;
        motionBase = _keptMotionBase;
        _gpu.moved = _keptMoved;
    }
    _keptMotion = motion;
    _keptMotionBase = motionBase;
    _keptMoved = _gpu.moved;
    // Levels of detail that finished building, for meshes they still fit.
    for (auto it = _pendingLods.begin(); it != _pendingLods.end();) {
        if (it->levels.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        std::vector<LodLevel> levels = it->levels.get();
        if (it->mesh && it->generation == it->mesh->_lodGeneration) {
            _StoreLods(*it->mesh, std::move(levels));
            changed = true;
        }
        it = _pendingLods.erase(it);
    }
    _Flush();

    // Per-mesh tables, materials and textures are small: rebuilt every
    // commit from what visible meshes use (O(meshes), not O(instances)).
    _gpu.textures.clear();
    _gpu.textureWraps.clear();
    _textureSlots.clear();
    std::unordered_map<const Material *, uint32_t> materialSlots;
    // The least roughness any material may have (default 0.5; a texture or
    // graph may reach the clamp's 0.045), coat included: whether the sun's
    // disc matters for them (Gpu::sunRegion).
    float minPerceptual = 0.5f;
    std::vector<GpuMaterial> materials;
    std::vector<int32_t> udimTiles;
    std::vector<uint32_t> programCode;
    std::vector<float> programConstants;
    auto texRef = [&](const TextureRef &r) {
        GpuTexRef g{};
        if (r.udim.empty()) {
            g.index = _TextureIndex(r.texture.get(), uint32_t(r.wrap[0]) + kWrapModes * uint32_t(r.wrap[1]));
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
    // A program's code, constants and images (shaders/program.glsl); where
    // they went in `out` (GpuMaterial::program).
    auto appendProgram = [&](const MaterialProgram &p, uint32_t out[4]) {
        if (p.Empty() || !p.Valid()) {
            return;
        }
        out[0] = uint32_t(programCode.size() / 4);
        out[1] = uint32_t(p.code.size() / 4);
        out[2] = uint32_t(programConstants.size() / 4);
        out[3] = uint32_t((programConstants.size() + p.constants.size()) / 4);
        programCode.insert(programCode.end(), p.code.begin(), p.code.end());
        programConstants.insert(programConstants.end(), p.constants.begin(), p.constants.end());
        for (const TextureRef &image : p.images) {
            const GpuTexRef r = texRef(image);
            const size_t at = programConstants.size();
            programConstants.resize(at + sizeof(GpuTexRef) / sizeof(float));
            std::memcpy(&programConstants[at], &r, sizeof(r));
        }
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
            minPerceptual = std::min(minPerceptual, m.roughnessTex.texture || m.program.Valid() ? 0.045f : m.roughness);
            minPerceptual = std::min(minPerceptual, m.coat > 0 ? m.coatRoughness : 1.0f);
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
            g.sheen[0] = m.sheen;
            g.sheen[1] = m.sheenRoughness;
            std::copy(m.sheenColor, m.sheenColor + 3, g.sheenColor);
            g.thinFilm[0] = m.thinFilm;
            g.thinFilm[1] = m.thinFilmThickness;
            g.thinFilm[2] = m.thinFilmIor;
            g.anisotropy[0] = m.anisotropy;
            g.anisotropy[1] = m.anisotropyRotation;
            g.anisotropy[2] = m.openPbrAnisotropy ? 1.0f : 0.0f;
            g.toon[0] = m.toonBands;
            g.toon[1] = m.toonSoftness;
            std::copy(m.transmissionColor, m.transmissionColor + 3, g.transmission);
            g.transmission[3] = m.transmission;
            g.refraction[0] = m.ior;
            g.refraction[1] = m.thinWalled ? 1.0f : 0.0f;
            g.refraction[2] = m.transmissionDepth;
            g.refraction[3] = m.opacityRefracts ? 1.0f : 0.0f;
            std::copy(m.subsurfaceColor, m.subsurfaceColor + 3, g.subsurface);
            g.subsurface[3] = m.subsurface;
            std::copy(m.subsurfaceRadius, m.subsurfaceRadius + 3, g.subsurfaceRadius);
            appendProgram(m.program, g.program);
            materials.push_back(g);
        }
        return it->second;
    };

    const size_t slots = _meshes.size();
    size_t commandSlots = slots;  // levels of detail have commands after the meshes'
    for (const auto &m : _meshes) {
        commandSlots += m->_lods.size();
    }
    const uint32_t capacity = _instanceAlloc.Capacity();
    _device->Reserve(_draws, slots * sizeof(DrawInfo), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    _device->Reserve(_commandTemplate, commandSlots * sizeof(VkDrawIndexedIndirectCommand),
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto *draws = static_cast<DrawInfo *>(_draws.mapped);
    auto *templates = static_cast<VkDrawIndexedIndirectCommand *>(_commandTemplate.mapped);
    std::vector<uint32_t> drawn(slots);  // instances per slot before culling
    std::vector<uint32_t> primvarTable;  // uvec4 per program primvar slot
    std::vector<float> primvarData;
    std::vector<uint8_t> kind(slots, Gpu::kOpaque);
    _gpu.translucent.clear();
    _gpu.translucentVariant.clear();
    _gpu.translucentFlags.clear();
    _gpu.translucentCenters.clear();
    _gpu.slotGeometry.resize(slots);
    _gpu.refraction = false;
    _gpu.holdouts = false;
    _gpu.subsurface.clear();
    _gpu.subsurfaceVariant.clear();
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};
    bool anyDrawable = false, facing = false;
    for (size_t slot = 0; slot < slots; ++slot) {
        const Mesh &m = *_meshes[slot];
        // A skinned mesh without (valid) skinning transforms has nothing to draw.
        const bool drawable = m.visible && (m.tag & _tagMask) && m._indexCount > 0 && m._instanceCount > 0 &&
                              (m._skinCount == 0 || m._skinXformSlots > 0) && !m.reflectionProbe;
        DrawInfo &d = draws[slot];
        d = DrawInfo{};
        std::copy(m._boundsMin, m._boundsMin + 3, d.boundsMin);
        std::copy(m._boundsMax, m._boundsMax + 3, d.boundsMax);
        d.boundsMax[3] = drawable && m.material ? m.material->displacementBound : 0.0f;
        std::copy(m.color, m.color + 3, d.color);
        d.material = drawable && m.material ? materialIndex(*m.material) : kNoMaterial;
        d.motion = motionBase[slot];
        d.hair = m.hair ? 1 : 0;
        d.facing = m.facing;
        d.uvSpacing = m._uvSpacing;
        facing = facing || (drawable && m.facing != Mesh::kNotFacing);
        const bool program = drawable && m.material && !m.material->program.Empty() && m.material->program.Valid();
        const uint8_t variant = (m._skinStride == 2 ? Gpu::kWide : 0) |
                                (m._skinCount > 0 && m.dualQuaternion ? Gpu::kDualQuat : 0) |
                                (program ? Gpu::kProgram : 0) |
                                (program && m.material->program.Procedural() ? Gpu::kProcedural : 0) |
                                (m.hair || (drawable && m.material && m.material->Layered()) ? Gpu::kLayers : 0) |
                                ((drawable && m.material && m.material->displacementBound > 0) ||
                                         m.facing != Mesh::kNotFacing
                                     ? Gpu::kDisplaced
                                     : 0);
        // Where each of the program's primvars is; a missing one reads the
        // program's fallback.
        d.primvars = kNone;
        d.primvarVertices = m._primvarVertexCount > 0 ? m._primvarVertexOffset : kNone;
        d.velocities = m._velocityCount > 0 ? m._velocityOffset : kNone;
        const bool displacementProgram = drawable && m.material && !m.material->displacementProgram.Empty();
        if ((program || displacementProgram) && !m.material->program.primvars.empty()) {
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
                   (m.material->opacity < 1 || m.material->opacityTex.texture || m.material->Transmits() ||
                    (program && m.material->program.Outputs(MaterialProgram::kOpacity)))) {
            kind[slot] = Gpu::kTranslucent;
            _gpu.refraction = _gpu.refraction || m.material->Transmits() || m.material->opacityRefracts;
            _gpu.translucent.push_back(uint32_t(slot));
            _gpu.translucentVariant.push_back(variant >> 2);
            _gpu.translucentFlags.push_back(
                (m.material->transmission > 0 || m.material->opacityRefracts ? Gpu::kRefracts : 0) |
                (m.holdout ? Gpu::kHoldout : 0));
            for (int k = 0; k < 3; ++k) {
                _gpu.translucentCenters.push_back(0.5f * (m._worldMin[k] + m._worldMax[k]));
            }
        }
        if (kind[slot] != Gpu::kTranslucent && drawable && m.material && m.material->Scatters()) {
            _gpu.subsurface.push_back(uint32_t(slot));
            _gpu.subsurfaceVariant.push_back(variant >> 2);
        }
        kind[slot] |= variant;
        d.primId = m.primId;
        d.holdout = m.holdout ? 1u : 0u;
        _gpu.holdouts = _gpu.holdouts || (drawable && m.holdout);
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
        // A displacement graph reading per-instance inputs, with instances
        // to differ: ray tracing displaces each (LargestDisplacedInstances).
        m._displacePerInstance =
            drawable && m.material && m._instanceCount > 1 && m._skinCount == 0 && (variant & Gpu::kDisplaced) &&
            m.material->displacementProgram.PerInstance([&](uint32_t slot) {
                const std::vector<std::string> &names = m.material->program.primvars;
                for (const Primvar &pv : m.primvars) {
                    if (slot < names.size() && pv.name == names[slot]) {
                        return pv.rate == Primvar::Rate::Instance;
                    }
                }
                return false;
            });
        // Translucent meshes are traced too, seen by shadow rays only (tints).
        const uint8_t blend = kind[slot] & (Gpu::kWide - 1);
        _gpu.slotGeometry[slot] = {m._vertexOffset, m._vertexCount, m._firstIndex, m._indexCount,
                                   drawable ? m._geometryVersion : 0, blend == Gpu::kCutout, false, m._skinCount > 0,
                                   (variant & Gpu::kDisplaced) != 0, blend == Gpu::kTranslucent, m._instanceBase};
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
    // Levels of detail (cull.comp picks one per instance): commands drawn
    // with their mesh's pipeline, counted at most all its instances.
    std::vector<GpuLod> lods(std::max<size_t>(slots, 1), GpuLod{});
    std::vector<double> weight(commandSlots);  // triangles a slot draws at most
    kind.resize(commandSlots);
    for (size_t slot = 0, next = slots; slot < slots; ++slot) {
        const Mesh &m = *_meshes[slot];
        weight[slot] = double(drawn[slot]) * double(m._indexCount / 3);
        GpuLod &g = lods[slot];
        g.slots[0] = uint32_t(next);
        // Translucent meshes draw per slot, without levels; displaced ones
        // keep their density (the levels' errors don't see displacement).
        g.slots[1] = (kind[slot] & (Gpu::kWide - 1)) == Gpu::kTranslucent || (kind[slot] & Gpu::kDisplaced) ||
                             m._lodInstanceCount == 0
                         ? 0
                         : uint32_t(m._lods.size());
        const uint32_t firsts[3] = {m._lodInstanceBase, m._lodInstanceBase + m._instanceCount,
                                    m._lodInstanceBase + 2 * m._instanceCount};
        std::copy(firsts, firsts + 3, g.regions);
        for (size_t k = 0; k < m._lods.size(); ++k, ++next) {
            const Mesh::Lod &lod = m._lods[k];
            templates[next] = {lod.indexCount, 0, lod.firstIndex, int32_t(m._vertexOffset), firsts[k]};
            kind[next] = kind[slot];
            weight[next] = double(drawn[slot]) * double(lod.indexCount / 3);
            g.errors[k] = lod.error;
        }
    }
    _device->Reserve(_lodTable, lods.size() * sizeof(GpuLod), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(lods.begin(), lods.end(), static_cast<GpuLod *>(_lodTable.mapped));
    _gpu.lods = _lodTable.buffer;
    _gpu.meshCount = uint32_t(slots);

    _gpu.drawChunks.assign(1, 0);
    _gpu.chunkKind.assign(1, Gpu::kOpaque);
    _gpu.chunkLevels.assign(1, 0);
    _gpu.instanceCount = _gpu.triangleCount = 0;
    double triangles = 0;
    for (size_t slot = 0; slot < commandSlots; ++slot) {
        const double t = weight[slot];
        if (slot < slots) {
            _gpu.instanceCount += drawn[slot];
            _gpu.triangleCount += uint64_t(drawn[slot]) * (_meshes[slot]->_indexCount / 3);
        }
        if (t == 0) {
            continue;  // draws nothing: fits any chunk
        }
        const uint8_t levels = slot >= slots || lods[slot].slots[1] > 0;
        if (triangles > 0 && (triangles + t > kChunkTriangles || kind[slot] != _gpu.chunkKind.back() ||
                              levels != _gpu.chunkLevels.back())) {
            _gpu.drawChunks.push_back(uint32_t(slot));
            _gpu.chunkKind.push_back(kind[slot]);
            _gpu.chunkLevels.push_back(levels);
            triangles = 0;
        }
        _gpu.chunkKind.back() = kind[slot];
        _gpu.chunkLevels.back() = levels;
        triangles += t;
    }
    _gpu.drawChunks.push_back(uint32_t(commandSlots));

    // Visibility edits change what shadows draw without any upload.
    if (changed || drawn != _lastDrawn) {
        static std::atomic<uint64_t> versions{0};
        _gpu.version = ++versions;
        _lastDrawn = std::move(drawn);
    }
    _gpu.hasBounds = anyDrawable;
    _gpu.facing = facing;
    std::copy(lo, lo + 3, _gpu.boundsMin);
    std::copy(hi, hi + 3, _gpu.boundsMax);

    // Displacement, per material slot.
    std::vector<GpuDisplacement> displacements(std::max<size_t>(materials.size(), 1), GpuDisplacement{});
    _gpu.displaced = false;
    for (const auto &[material, slot] : materialSlots) {
        GpuDisplacement &g = displacements[slot];
        g.params[0] = material->displacement;
        g.params[1] = material->displacementBound;
        g.tex = texRef(material->displacementTex);
        appendProgram(material->displacementProgram, g.program);
        _gpu.displaced = _gpu.displaced || material->displacementBound > 0;
    }
    _device->Reserve(_displacements, displacements.size() * sizeof(GpuDisplacement),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(displacements.begin(), displacements.end(), static_cast<GpuDisplacement *>(_displacements.mapped));
    _gpu.displacements = _displacements.buffer;
    _gpu.probeBoxes.clear();
    for (const auto &m : _meshes) {
        if (m->reflectionProbe && m->_instanceCount > 0 && m->_worldMin[0] <= m->_worldMax[0]) {
            _gpu.probeBoxes.push_back({m->_worldMin[0], m->_worldMin[1], m->_worldMin[2], m->_worldMax[0],
                                       m->_worldMax[1], m->_worldMax[2]});
        }
    }
    // Planar reflections' mirrors: the flagged flat meshes, else the
    // largest glossy one (its world bounds' diagonal) spanning a third of
    // the scene. ponytail: each mesh from its first instance, one render
    // each (coplanar mirrors don't share one).
    std::vector<std::tuple<bool, float, Gpu::Mirror>> mirrors;  // flagged, size
    const float sceneSize = anyDrawable ? Length(Vec3{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) : 0.0f;
    for (size_t slot = 0; slot < slots; ++slot) {
        const Mesh &m = *_meshes[slot];
        if (!draws[slot].visible || !m._flat || (kind[slot] & (Gpu::kWide - 1)) != Gpu::kOpaque ||
            m.transforms.size() < 16) {
            continue;
        }
        const Material *mat = m.material.get();
        const bool glossy = mat && mat->roughness <= 0.3f && !mat->roughnessTex.texture &&
                            !(mat->program.Valid() && mat->program.Outputs(MaterialProgram::kRoughness));
        const float size = Length(Vec3{m._worldMax[0] - m._worldMin[0], m._worldMax[1] - m._worldMin[1],
                                       m._worldMax[2] - m._worldMin[2]});
        if (!m.planarReflection && !(glossy && size >= sceneSize / 3.0f && size > 0.0f)) {
            continue;
        }
        const float *x = m.transforms.data();  // row-vector order
        Vec3 p[3];
        for (int k = 0; k < 3; ++k) {
            const float *t = &m._flatTriangle[k * 3];
            p[k] = {t[0] * x[0] + t[1] * x[4] + t[2] * x[8] + x[12], t[0] * x[1] + t[1] * x[5] + t[2] * x[9] + x[13],
                    t[0] * x[2] + t[1] * x[6] + t[2] * x[10] + x[14]};
        }
        const Vec3 n = Normalize(Cross(p[1] - p[0], p[2] - p[0]));
        if (Dot(n, n) < 0.5f) {
            continue;
        }
        Gpu::Mirror mirror;
        mirror.slot = uint32_t(slot);
        const float plane[4] = {n.x, n.y, n.z, -Dot(n, p[0])};
        std::copy(plane, plane + 4, mirror.plane);
        std::copy(m._worldMin, m._worldMin + 3, mirror.min);
        std::copy(m._worldMax, m._worldMax + 3, mirror.max);
        mirrors.push_back({m.planarReflection, size, mirror});
    }
    std::sort(mirrors.begin(), mirrors.end(), [](const auto &a, const auto &b) {
        return std::tie(std::get<0>(a), std::get<1>(a)) > std::tie(std::get<0>(b), std::get<1>(b));
    });
    const bool flagged = !mirrors.empty() && std::get<0>(mirrors[0]);
    _gpu.mirrors.clear();
    for (const auto &[isFlagged, size, mirror] : mirrors) {
        if (_gpu.mirrors.size() == (flagged ? Gpu::kMaxMirrors : 1u) || isFlagged != flagged) {
            break;
        }
        _gpu.mirrors.push_back(mirror);
    }

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
    if (!_velocities.buffer) {  // the same for velocities
        _EnsureCapacity(_velocities, _velocityAlloc, 1, 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    }

    const std::vector<Emitter> emitters = _FindEmitters();
    // The dome's environment first: its extracted sun (SetDomeSun) lights
    // the scene as the last distant light (a scene's own sun keeps the
    // cascades).
    const auto dome = std::find_if(_domes.begin(), _domes.end(), [](const auto &d) { return d->visible; });
    if (dome != _domes.end()) {
        _UpdateEnvironment(dome->get());
    }
    Light domeSun;
    const bool hasDomeSun = dome != _domes.end() && _envThreshold > 0.0f &&
                            _domeSun.irradiance[0] + _domeSun.irradiance[1] + _domeSun.irradiance[2] > 0.0f;
    if (hasDomeSun) {
        // Dome-local to world: the rotation's transpose (USD row-vector order).
        const float *m = (*dome)->worldToLocal;
        const float *d = _domeSun.direction;
        float w[3];
        for (int k = 0; k < 3; ++k) {
            w[k] = m[k * 4 + 0] * d[0] + m[k * 4 + 1] * d[1] + m[k * 4 + 2] * d[2];
        }
        const float len = std::max(std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), 1e-20f);
        for (int k = 0; k < 3; ++k) {
            domeSun.direction[k] = w[k] / len;
            domeSun.irradiance[k] = _domeSun.irradiance[k] * (*dome)->radiance[k];
        }
        domeSun.angle = std::max(_domeSun.angle, 0.1f);
        domeSun.castShadows = true;
    }
    std::vector<const Light *> sceneLights;
    for (const auto &l : _lights) {
        sceneLights.push_back(l.get());
    }
    if (hasDomeSun) {
        sceneLights.push_back(&domeSun);
    }
    _device->Reserve(_lightBuffer, (sceneLights.size() + 2 * emitters.size()) * sizeof(GpuLight),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto *lights = static_cast<GpuLight *>(_lightBuffer.mapped);
    uint32_t lightCount = 0, localLightCount = 0;
    _gpu.shadowLight = kNone;
    _gpu.shadowAngle = 0;
    _gpu.shadowCasters.clear();
    _gpu.sunRegion = false;
    for (const bool distant : {true, false}) {  // distant lights first
        for (const Light *l : sceneLights) {
            if (!l->visible || (l->type == Light::Type::Distant) != distant) {
                continue;
            }
            GpuLight &g = lights[lightCount + localLightCount];
            g = GpuLight{};
            g.halfHeight[3] = l->diffuse;
            g.color[3] = l->cameraVisible ? 1.0f : 0.0f;  // light_shapes.frag (the colour is copied over rgb)
            g.shaping[3] = l->specular;
            if (distant) {
                if (l->castShadows && _gpu.shadowLight == kNone) {
                    _gpu.shadowLight = lightCount;
                    _gpu.shadowAngle = l->angle;
                    std::copy(l->direction, l->direction + 3, _gpu.shadowDirection);
                }
                std::copy(l->direction, l->direction + 3, g.position);
                std::copy(l->irradiance, l->irradiance + 3, g.color);
                g.axis[3] = 0.5f * l->angle * 0.0174532925f;
                const float sunTan = std::tan(g.axis[3]), a = minPerceptual * minPerceptual;
                _gpu.sunRegion = _gpu.sunRegion || sunTan * sunTan > 4e-4f * a * a;
                ++lightCount;
                continue;
            }
            const bool sphere = l->type == Light::Type::Sphere;
            const bool cylinder = l->type == Light::Type::Cylinder;
            std::copy(l->position, l->position + 3, g.position);
            g.position[3] = sphere ? 1.0f : l->type == Light::Type::Rect ? 2.0f : cylinder ? 4.0f : 3.0f;
            std::copy(l->radiance, l->radiance + 3, g.color);
            std::copy(l->direction, l->direction + 3, g.axis);
            g.axis[3] = sphere || cylinder ? l->radius : 0.0f;
            std::copy(l->halfWidth, l->halfWidth + 3, g.halfWidth);
            g.halfWidth[3] = (sphere || cylinder) && !l->point ? l->radius : 0.0f;
            std::copy(l->halfHeight, l->halfHeight + 3, g.halfHeight);
            const float cone = l->coneAngle * 0.0174532925f;
            g.shaping[0] = cone * (1.0f - std::clamp(l->coneSoftness, 0.0f, 1.0f));
            g.shaping[1] = cone;
            g.shaping[2] = l->focus;
            std::copy(l->focusTint, l->focusTint + 3, g.focusTint);
            const bool flat = l->type == Light::Type::Rect || l->type == Light::Type::Disk;
            const uint32_t clamp = uint32_t(Wrap::Clamp) * (1 + kWrapModes);
            g.focusTint[3] = flat ? float(_TextureIndex(l->texture.get(), clamp)) : -1.0f;  // its bindless index
            if (l->castShadows) {
                // Mirrors LightRange() in shaders/lights.glsl.
                Scene::Gpu::ShadowCaster c{};
                c.light = lightCount + localLightCount;
                std::copy(l->position, l->position + 3, c.position);
                std::copy(l->direction, l->direction + 3, c.axis);
                c.coneAngle = cone;
                c.shape = sphere ? Scene::Gpu::ShadowCaster::Shape::Sphere
                        : cylinder ? Scene::Gpu::ShadowCaster::Shape::Cylinder
                        : l->type == Light::Type::Rect ? Scene::Gpu::ShadowCaster::Shape::Rect
                                                       : Scene::Gpu::ShadowCaster::Shape::Disk;
                std::copy(l->halfWidth, l->halfWidth + 3, c.halfWidth);
                std::copy(l->halfHeight, l->halfHeight + 3, c.halfHeight);
                const float radiance = std::max({l->radiance[0], l->radiance[1], l->radiance[2]}) *
                                       std::max(l->diffuse, l->specular);
                if (sphere) {
                    c.rangeIntensity = radiance * l->radius * l->radius;
                    c.radius = l->radius;
                    c.size = l->point ? 0.0f : l->radius;
                } else if (cylinder) {
                    // Seen from the side it is a 2r x length rect; casters
                    // can come within r / sqrt 2 of the center (radius 0).
                    const float h = std::sqrt(l->halfWidth[0] * l->halfWidth[0] + l->halfWidth[1] * l->halfWidth[1] +
                                              l->halfWidth[2] * l->halfWidth[2]);
                    c.rangeIntensity = radiance * 4.0f * l->radius * h / 3.14159265f;
                    c.size = std::sqrt(4.0f * l->radius * h / 3.14159265f);
                    c.extent = h;
                    c.cylinderRadius = l->point ? 0.0f : l->radius;
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
    // Emissive meshes (SetEmissiveLights): diffuse lights after the scene's
    // own, shadowed like them.
    std::unordered_map<const Mesh *, uint32_t> promoted;
    for (const Emitter &e : emitters) {
        for (const float side : {1.0f, -1.0f}) {
            if (!e.flat && side < 0.0f) {
                break;
            }
            GpuLight &g = lights[lightCount + localLightCount];
            g = GpuLight{};
            std::copy(e.radiance, e.radiance + 3, g.color);
            g.halfHeight[3] = 1.0f;
            g.shaping[0] = g.shaping[1] = 3.14159265f;  // no cone; specular multiplier 0
            Scene::Gpu::ShadowCaster c{};
            c.light = lightCount + localLightCount;
            c.coneAngle = 3.14159265f;
            const float peak = std::max({e.radiance[0], e.radiance[1], e.radiance[2]});
            if (e.flat) {
                // A rect just off this face (its normal from the wide axes:
                // a quad has no thickness), emitting away from it.
                const float n[3] = {e.wide[1] * e.high[2] - e.wide[2] * e.high[1],
                                    e.wide[2] * e.high[0] - e.wide[0] * e.high[2],
                                    e.wide[0] * e.high[1] - e.wide[1] * e.high[0]};
                const float nl = std::max(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1e-30f);
                const float offset = std::sqrt(e.thin[0] * e.thin[0] + e.thin[1] * e.thin[1] + e.thin[2] * e.thin[2]) +
                                     1e-3f * e.radius;
                for (int k = 0; k < 3; ++k) {
                    g.axis[k] = side * n[k] / nl;
                    g.position[k] = e.center[k] + g.axis[k] * offset;
                    g.halfWidth[k] = e.wide[k];
                    g.halfHeight[k] = e.high[k];
                    c.extent += (e.wide[k] + e.high[k]) * (e.wide[k] + e.high[k]);
                }
                g.position[3] = 2.0f;
                c.shape = Scene::Gpu::ShadowCaster::Shape::Rect;
                std::copy(e.wide, e.wide + 3, c.halfWidth);
                std::copy(e.high, e.high + 3, c.halfHeight);
                const float area = 4.0f * std::sqrt(e.wide[0] * e.wide[0] + e.wide[1] * e.wide[1] + e.wide[2] * e.wide[2]) *
                                   std::sqrt(e.high[0] * e.high[0] + e.high[1] * e.high[1] + e.high[2] * e.high[2]);
                c.rangeIntensity = peak * area / 3.14159265f;  // as for rect lights above
                c.size = std::sqrt(area / 3.14159265f);
                c.extent = std::sqrt(c.extent);
            } else {
                std::copy(e.center, e.center + 3, g.position);
                g.position[3] = 1.0f;
                g.axis[2] = -1.0f;
                g.axis[3] = g.halfWidth[3] = e.radius;
                c.rangeIntensity = peak * e.radius * e.radius;
                c.radius = c.size = e.radius;
            }
            std::copy(g.position, g.position + 3, c.position);
            std::copy(g.axis, g.axis + 3, c.axis);
            _gpu.shadowCasters.push_back(c);
            ++localLightCount;
        }
        ++promoted[e.mesh];
    }
    for (const auto &[mesh, instances] : promoted) {
        _gpu.slotGeometry[mesh->_slot].emitter = instances == mesh->_instanceCount;
    }

    _gpu.environment = VK_NULL_HANDLE;
    _gpu.environmentLatlong = kNone;
    if (dome != _domes.end()) {
        _UpdateEnvironment(dome->get());
        _gpu.environment = _envView;
        std::copy((*dome)->radiance, (*dome)->radiance + 3, _gpu.environmentRadiance);
        std::copy((*dome)->worldToLocal, (*dome)->worldToLocal + 16, _gpu.environmentWorldToLocal);
        const int32_t index = _TextureIndex((*dome)->texture.get());
        _gpu.environmentLatlong = index < 0 ? kNone : uint32_t(index);
        _gpu.environmentCameraVisible = (*dome)->cameraVisible;
    }

    // What lighting depends on besides geometry (FNV-1a, a word at a time).
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](const void *data, size_t bytes) {
        const auto *b = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < bytes; i += 8) {
            uint64_t word = 0;
            std::memcpy(&word, b + i, std::min<size_t>(8, bytes - i));
            hash = (hash ^ word) * 1099511628211ull;
        }
    };
    mix(lights, (lightCount + localLightCount) * sizeof(GpuLight));
    mix(materials.data(), materials.size() * sizeof(GpuMaterial));
    mix(&_gpu.environment, sizeof(_gpu.environment));
    mix(&_gpu.environmentCameraVisible, sizeof(_gpu.environmentCameraVisible));
    mix(_gpu.environmentRadiance, sizeof(_gpu.environmentRadiance));
    mix(_gpu.environmentWorldToLocal, sizeof(_gpu.environmentWorldToLocal));
    _gpu.lightingHash = hash;

    _CommitVolumes();
    _gpu.vertices = _vertices.buffer;
    _gpu.indices = _indices.buffer;
    _gpu.instances = _instances.buffer;
    static std::atomic<uint64_t> sceneIds{0};
    if (_gpu.sceneId == 0) {
        _gpu.sceneId = ++sceneIds;
    }
    _gpu.visible = _visible.buffer;
    _gpu.draws = _draws.buffer;
    _gpu.commandTemplate = _commandTemplate.buffer;
    _gpu.materials = _materials.buffer;
    _gpu.programCode = _programCode.buffer;
    _gpu.programConstants = _programConstants.buffer;
    _gpu.primvarTable = _primvarTable.buffer;
    _gpu.primvarData = _primvarData.buffer;
    _gpu.primvarVertices = _primvarVertices.buffer;
    _gpu.velocities = _velocities.buffer;
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
    _gpu.blendWeightsBytes = _blendWeights.size;
    _gpu.drawCount = uint32_t(commandSlots);
    _gpu.instanceCapacity = capacity;
    _gpu.lightCount = lightCount;
    _gpu.localLightCount = localLightCount;
}

} // namespace vkr
