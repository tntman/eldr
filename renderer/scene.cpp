#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

} // namespace

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
        for (Buffer *b : {&_vertices, &_instances, &_indirect, &_materials, &_lightBuffer, &_staging}) {
            _device->Destroy(*b);
        }
        _DestroyEnvironment();
    }
}

Mesh *Scene::AddMesh()
{
    _meshes.push_back(std::make_unique<Mesh>());
    _meshes.back()->_slot = _meshes.size() - 1;
    _poolDirty = true;
    return _meshes.back().get();
}

void Scene::RemoveMesh(Mesh *mesh)
{
    _RemoveSlot(_meshes, mesh);
    _poolDirty = true;
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

// ponytail: any geometry edit re-packs every mesh, and corners are not
// shared (3 vertices per triangle). Sub-allocate and re-index when big
// scenes make this slow or memory-heavy.
void Scene::_RebuildVertexPool()
{
    size_t total = 0;
    for (const auto &m : _meshes) {
        total += m->points.size() / 3;
    }
    _device->Reserve(_vertices, total * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    auto *dst = static_cast<Vertex *>(_vertices.mapped);
    uint32_t first = 0;
    for (const auto &m : _meshes) {
        m->geometryDirty = false;
        const size_t n = m->points.size() / 3;
        // Sizes come from scene data: skip a mesh whose arrays disagree
        // rather than read past them.
        const bool valid = m->points.size() % 9 == 0 &&
                           (m->normals.empty() || m->normals.size() == m->points.size()) &&
                           (m->uvs.empty() || m->uvs.size() == n * 2);
        if (!valid) {
            m->_vertexCount = 0;
            continue;
        }
        std::fill(m->_boundsMin, m->_boundsMin + 3, std::numeric_limits<float>::max());
        std::fill(m->_boundsMax, m->_boundsMax + 3, std::numeric_limits<float>::lowest());
        for (size_t i = 0; i < n; ++i) {
            Vertex &v = dst[first + i];
            std::memcpy(v.position, &m->points[i * 3], sizeof(v.position));
            for (int k = 0; k < 3; ++k) {
                m->_boundsMin[k] = std::min(m->_boundsMin[k], v.position[k]);
                m->_boundsMax[k] = std::max(m->_boundsMax[k], v.position[k]);
            }
            if (m->normals.empty()) {
                std::fill(v.normal, v.normal + 3, 0.0f);
            } else {
                std::memcpy(v.normal, &m->normals[i * 3], sizeof(v.normal));
            }
            if (m->uvs.empty()) {
                std::fill(v.uv, v.uv + 2, 0.0f);
            } else {
                std::memcpy(v.uv, &m->uvs[i * 2], sizeof(v.uv));
            }
        }
        m->_firstVertex = first;
        m->_vertexCount = uint32_t(n);
        first += uint32_t(n);
    }
    _poolDirty = false;
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
    if (_poolDirty || std::any_of(_meshes.begin(), _meshes.end(),
                                  [](const auto &m) { return m->geometryDirty; })) {
        _RebuildVertexPool();
    }

    // Per-draw, material, light and texture tables are small enough to
    // rebuild every commit from what visible meshes use, instead of
    // tracking dirtiness. ponytail: not for 1M+ instances; dirty ranges in step 6.
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

    auto drawable = [](const Mesh &m) {
        return m.visible && m._vertexCount > 0 && !m.transforms.empty() &&
               m.transforms.size() % 16 == 0;
    };
    size_t instanceCount = 0, drawCount = 0;
    for (const auto &m : _meshes) {
        if (drawable(*m)) {
            instanceCount += m->transforms.size() / 16;
            ++drawCount;
        }
    }
    _device->Reserve(_instances, instanceCount * sizeof(Instance), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    _device->Reserve(_indirect, drawCount * sizeof(VkDrawIndirectCommand),
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    auto *inst = static_cast<Instance *>(_instances.mapped);
    auto *cmd = static_cast<VkDrawIndirectCommand *>(_indirect.mapped);
    uint32_t n = 0, firstInstance = 0;
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::lowest()};
    for (const auto &m : _meshes) {
        if (!drawable(*m)) {
            continue;
        }
        const uint32_t material = m->material ? materialIndex(*m->material) : kNoMaterial;
        const uint32_t count = uint32_t(m->transforms.size() / 16);
        float center[3], extent[3];
        for (int k = 0; k < 3; ++k) {
            center[k] = 0.5f * (m->_boundsMin[k] + m->_boundsMax[k]);
            extent[k] = 0.5f * (m->_boundsMax[k] - m->_boundsMin[k]);
        }
        for (uint32_t i = 0; i < count; ++i) {
            Instance &out = inst[firstInstance + i];
            const float *x = &m->transforms[size_t(i) * 16];
            std::memcpy(out.transform, x, sizeof(out.transform));
            std::memcpy(out.color, m->color, sizeof(m->color));
            out.material = material;
            out.primId = m->primId;
            out.instanceId = int32_t(i);
            // World AABB of this instance (row-vector: p' = p * M).
            for (int k = 0; k < 3; ++k) {
                const float c = center[0] * x[k] + center[1] * x[4 + k] + center[2] * x[8 + k] + x[12 + k];
                const float e = extent[0] * std::abs(x[k]) + extent[1] * std::abs(x[4 + k]) +
                                extent[2] * std::abs(x[8 + k]);
                lo[k] = std::min(lo[k], c - e);
                hi[k] = std::max(hi[k], c + e);
            }
        }
        cmd[n++] = {m->_vertexCount, count, m->_firstVertex, firstInstance};
        firstInstance += count;
    }
    _gpu.hasBounds = n > 0;
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
    _gpu.instances = _instances.buffer;
    _gpu.indirect = _indirect.buffer;
    _gpu.materials = _materials.buffer;
    _gpu.lights = _lightBuffer.buffer;
    _gpu.drawCount = n;
    _gpu.lightCount = lightCount;
}

} // namespace vkr
