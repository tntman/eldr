#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>

namespace vkr {

namespace {

// std430 mirrors of the structs in mesh.frag.
struct GpuTexRef {
    int32_t index;
    int32_t channel;
    int32_t pad[2];
    float scale[4];
    float bias[4];
};
struct GpuMaterial {
    float baseColor[4]; // rgb, w = 1: use the mesh's displayColor instead
    float emissive[4];
    float params[4];  // metallic, roughness, occlusion, unused
    GpuTexRef tex[6]; // baseColor, emissive, metallic, roughness, occlusion, normal
};
struct GpuLight {
    float direction[4];
    float irradiance[4];
};

constexpr VkBufferUsageFlags kPoolUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

VkFormat ToVk(Texture::Format f)
{
    switch (f) {
    case Texture::Format::RGBA8:      return VK_FORMAT_R8G8B8A8_UNORM;
    case Texture::Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Texture::Format::RGBA16F:    return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Texture::Format::RGBA32F:    return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

size_t BytesPerPixel(Texture::Format f)
{
    switch (f) {
    case Texture::Format::RGBA16F: return 8;
    case Texture::Format::RGBA32F: return 16;
    default:                       return 4;
    }
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
    bool operator==(const VertexKey &o) const { return std::memcmp(&v, &o.v, sizeof(Vertex)) == 0; }
};
struct VertexKeyHash {
    size_t operator()(const VertexKey &k) const
    {
        uint64_t h = 1469598103934665603ull;  // FNV-1a
        const auto *p = reinterpret_cast<const uint8_t *>(&k.v);
        for (size_t i = 0; i < sizeof(Vertex); ++i) {
            h = (h ^ p[i]) * 1099511628211ull;
        }
        return size_t(h);
    }
};

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
        vkFreeMemory(_device->device, _memory, nullptr);
    }
}

Scene::Scene(Device *device)
    : _device(device)
{
}

Scene::~Scene()
{
    if (_device) {
        for (Buffer *b : {&_vertices, &_indices, &_instances, &_visible, &_draws, &_commandTemplate,
                          &_shadowCommands, &_materials, &_lightBuffer, &_staging}) {
            _device->Destroy(*b);
        }
        _DestroyEnvironment();
    }
}

Mesh *Scene::AddMesh()
{
    _meshes.push_back(std::make_unique<Mesh>());
    _meshes.back()->_slot = _meshes.size() - 1;
    return _meshes.back().get();
}

void Scene::RemoveMesh(Mesh *mesh)
{
    // Only bookkeeping here; the freed instance slots are invalidated on the
    // GPU at the next Commit.
    if (_device) {
        _vertexAlloc.Free(mesh->_vertexOffset, mesh->_vertexCount);
        _indexAlloc.Free(mesh->_firstIndex, mesh->_indexCount);
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

// Makes room for a further `needed` elements: doubles the pool, copies the
// old contents over on the GPU and, for the instance pool, marks the new
// slots free and rebuilds the visible buffer's identity half.
void Scene::_EnsureCapacity(Buffer &buffer, RangeAllocator &alloc, uint32_t needed,
                            VkDeviceSize elementSize, VkBufferUsageFlags usage, bool instances)
{
    _Flush();  // pending copies/fills target the old buffer
    const uint32_t old = alloc.Capacity();
    const uint32_t capacity = std::max({old * 2, old + needed, 1024u});
    Buffer grown = _device->CreateDeviceBuffer(capacity * elementSize, usage | kPoolUsage);
    _device->Submit([&](VkCommandBuffer cmd) {
        if (old > 0) {
            const VkBufferCopy region{0, 0, old * elementSize};
            vkCmdCopyBuffer(cmd, buffer.buffer, grown.buffer, 1, &region);
        }
        if (instances) {
            vkCmdFillBuffer(cmd, grown.buffer, old * elementSize, (capacity - old) * elementSize, 0xFFFFFFFFu);
        }
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    });
    _device->Destroy(buffer);
    buffer = grown;
    alloc.Grow(capacity);

    if (instances) {
        _device->Destroy(_visible);
        _visible = _device->CreateDeviceBuffer(VkDeviceSize(capacity) * 2 * sizeof(uint32_t),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | kPoolUsage);
        std::vector<uint32_t> identity(capacity);
        for (uint32_t i = 0; i < capacity; ++i) {
            identity[i] = i;
        }
        _copies.push_back({_visible.buffer, VkDeviceSize(capacity) * sizeof(uint32_t),
                           _Stage(identity.data(), identity.size() * sizeof(uint32_t)),
                           identity.size() * sizeof(uint32_t)});
    }
}

void Scene::_UploadGeometry(Mesh &m)
{
    _vertexAlloc.Free(m._vertexOffset, m._vertexCount);
    _indexAlloc.Free(m._firstIndex, m._indexCount);
    m._vertexCount = m._indexCount = 0;
    m.geometryDirty = false;

    const size_t corners = m.points.size() / 3;
    // Sizes come from scene data: skip a mesh whose arrays disagree rather
    // than read past them.
    const bool valid = m.points.size() % 9 == 0 &&
                       (m.normals.empty() || m.normals.size() == m.points.size()) &&
                       (m.uvs.empty() || m.uvs.size() == corners * 2);
    if (!valid || corners == 0) {
        return;
    }

    // Corners sharing position, normal and uv become one indexed vertex.
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices(corners);
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> unique;
    unique.reserve(corners);
    std::fill(m._boundsMin, m._boundsMin + 3, std::numeric_limits<float>::max());
    std::fill(m._boundsMax, m._boundsMax + 3, std::numeric_limits<float>::lowest());
    for (size_t i = 0; i < corners; ++i) {
        VertexKey key{};
        std::memcpy(key.v.position, &m.points[i * 3], sizeof(key.v.position));
        if (!m.normals.empty()) {
            std::memcpy(key.v.normal, &m.normals[i * 3], sizeof(key.v.normal));
        }
        if (!m.uvs.empty()) {
            std::memcpy(key.v.uv, &m.uvs[i * 2], sizeof(key.v.uv));
        }
        const auto [it, inserted] = unique.emplace(key, uint32_t(vertices.size()));
        if (inserted) {
            vertices.push_back(key.v);
            for (int k = 0; k < 3; ++k) {
                m._boundsMin[k] = std::min(m._boundsMin[k], key.v.position[k]);
                m._boundsMax[k] = std::max(m._boundsMax[k], key.v.position[k]);
            }
        }
        indices[i] = it->second;
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
    std::vector<Instance> instances(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::memcpy(instances[i].transform, &m.transforms[size_t(i) * 16], sizeof(instances[i].transform));
        instances[i].draw = uint32_t(m._slot);
        instances[i].index = int32_t(i);
    }
    _copies.push_back({_instances.buffer, VkDeviceSize(m._instanceBase) * sizeof(Instance),
                       _Stage(instances.data(), instances.size() * sizeof(Instance)),
                       instances.size() * sizeof(Instance)});
}

void Scene::_UpdateWorldBounds(Mesh &m)
{
    std::fill(m._worldMin, m._worldMin + 3, std::numeric_limits<float>::max());
    std::fill(m._worldMax, m._worldMax + 3, std::numeric_limits<float>::lowest());
    if (m._indexCount == 0) {
        return;
    }
    float center[3], extent[3];
    for (int k = 0; k < 3; ++k) {
        center[k] = 0.5f * (m._boundsMin[k] + m._boundsMax[k]);
        extent[k] = 0.5f * (m._boundsMax[k] - m._boundsMin[k]);
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
    t._memory = _device->Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindImageMemory(device, t._image, t._memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t._image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
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

int32_t Scene::_TextureIndex(const TextureRef &ref)
{
    Texture *t = ref.texture.get();
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
    vkFreeMemory(_device->device, _envMemory, nullptr);
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
    }

    // Upload only what changed.
    for (const auto &m : _meshes) {
        const bool geometry = m->geometryDirty, transforms = m->transformsDirty;
        if (geometry) {
            _UploadGeometry(*m);
        }
        if (transforms) {
            _UploadInstances(*m);
        }
        if (geometry || transforms) {
            _UpdateWorldBounds(*m);
        }
    }
    _Flush();

    // Per-mesh tables, materials and textures are small: rebuilt every
    // commit from what visible meshes use (O(meshes), not O(instances)).
    _gpu.textures.clear();
    _textureSlots.clear();
    std::unordered_map<const Material *, uint32_t> materialSlots;
    std::vector<GpuMaterial> materials;
    auto materialIndex = [&](const Material &m) {
        const auto [it, inserted] = materialSlots.emplace(&m, uint32_t(materials.size()));
        if (inserted) {
            GpuMaterial g{};
            std::copy(m.baseColor, m.baseColor + 3, g.baseColor);
            g.baseColor[3] = m.baseColorFromDisplayColor ? 1.0f : 0.0f;
            std::copy(m.emissive, m.emissive + 3, g.emissive);
            g.params[0] = m.metallic;
            g.params[1] = m.roughness;
            g.params[2] = m.occlusion;
            const TextureRef *refs[6] = {&m.baseColorTex, &m.emissiveTex, &m.metallicTex,
                                         &m.roughnessTex, &m.occlusionTex, &m.normalTex};
            for (int i = 0; i < 6; ++i) {
                g.tex[i].index = _TextureIndex(*refs[i]);
                g.tex[i].channel = refs[i]->channel;
                std::copy(refs[i]->scale, refs[i]->scale + 4, g.tex[i].scale);
                std::copy(refs[i]->bias, refs[i]->bias + 4, g.tex[i].bias);
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
    _device->Reserve(_shadowCommands, slots * sizeof(VkDrawIndexedIndirectCommand),
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    auto *draws = static_cast<DrawInfo *>(_draws.mapped);
    auto *templates = static_cast<VkDrawIndexedIndirectCommand *>(_commandTemplate.mapped);
    auto *shadows = static_cast<VkDrawIndexedIndirectCommand *>(_shadowCommands.mapped);
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};
    bool anyDrawable = false;
    for (size_t slot = 0; slot < slots; ++slot) {
        const Mesh &m = *_meshes[slot];
        const bool drawable = m.visible && m._indexCount > 0 && m._instanceCount > 0;
        DrawInfo &d = draws[slot];
        d = DrawInfo{};
        std::copy(m._boundsMin, m._boundsMin + 3, d.boundsMin);
        std::copy(m._boundsMax, m._boundsMax + 3, d.boundsMax);
        std::copy(m.color, m.color + 3, d.color);
        d.material = drawable && m.material ? materialIndex(*m.material) : kNoMaterial;
        d.primId = m.primId;
        d.visible = drawable ? 1u : 0u;
        // Culling fills in instanceCount; shadows draw every instance through
        // the identity half of the visible buffer.
        templates[slot] = {m._indexCount, 0, m._firstIndex, int32_t(m._vertexOffset), m._instanceBase};
        shadows[slot] = {m._indexCount, drawable ? m._instanceCount : 0, m._firstIndex,
                         int32_t(m._vertexOffset), capacity + m._instanceBase};
        if (drawable) {
            anyDrawable = true;
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], m._worldMin[k]);
                hi[k] = std::max(hi[k], m._worldMax[k]);
            }
        }
    }
    _gpu.hasBounds = anyDrawable;
    std::copy(lo, lo + 3, _gpu.boundsMin);
    std::copy(hi, hi + 3, _gpu.boundsMax);

    _device->Reserve(_materials, materials.size() * sizeof(GpuMaterial),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(materials.begin(), materials.end(), static_cast<GpuMaterial *>(_materials.mapped));

    _device->Reserve(_lightBuffer, _lights.size() * sizeof(GpuLight),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto *lights = static_cast<GpuLight *>(_lightBuffer.mapped);
    uint32_t lightCount = 0;
    _gpu.shadowLight = kNone;
    for (const auto &l : _lights) {
        if (!l->visible) {
            continue;
        }
        if (l->castShadows && _gpu.shadowLight == kNone) {
            _gpu.shadowLight = lightCount;
            std::copy(l->direction, l->direction + 3, _gpu.shadowDirection);
        }
        GpuLight &g = lights[lightCount++];
        std::copy(l->direction, l->direction + 3, g.direction);
        std::copy(l->irradiance, l->irradiance + 3, g.irradiance);
        g.direction[3] = g.irradiance[3] = 0;
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
        TextureRef latlong;
        latlong.texture = (*dome)->texture;
        const int32_t index = _TextureIndex(latlong);
        _gpu.environmentLatlong = index < 0 ? kNone : uint32_t(index);
    }

    _gpu.vertices = _vertices.buffer;
    _gpu.indices = _indices.buffer;
    _gpu.instances = _instances.buffer;
    _gpu.visible = _visible.buffer;
    _gpu.draws = _draws.buffer;
    _gpu.commandTemplate = _commandTemplate.buffer;
    _gpu.shadowCommands = _shadowCommands.buffer;
    _gpu.materials = _materials.buffer;
    _gpu.lights = _lightBuffer.buffer;
    _gpu.drawCount = uint32_t(slots);
    _gpu.instanceCapacity = capacity;
    _gpu.lightCount = lightCount;
}

} // namespace vkr
