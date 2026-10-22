#include "scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
    const size_t slot = mesh->_slot;
    std::swap(_meshes[slot], _meshes.back());
    _meshes[slot]->_slot = slot;
    _meshes.pop_back();
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
    const size_t slot = light->_slot;
    std::swap(_lights[slot], _lights.back());
    _lights[slot]->_slot = slot;
    _lights.pop_back();
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
        for (size_t i = 0; i < n; ++i) {
            Vertex &v = dst[first + i];
            std::memcpy(v.position, &m->points[i * 3], sizeof(v.position));
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

void Scene::Commit()
{
    if (!_device) {
        return;
    }
    if (_poolDirty || std::any_of(_meshes.begin(), _meshes.end(),
                                  [](const auto &m) { return m->geometryDirty; })) {
        _RebuildVertexPool();
    }

    // Per-draw, material, light and texture tables are small: rebuild them
    // every commit from what visible meshes use, instead of tracking dirtiness.
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

    _device->Reserve(_instances, _meshes.size() * sizeof(Instance),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    _device->Reserve(_indirect, _meshes.size() * sizeof(VkDrawIndirectCommand),
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    auto *inst = static_cast<Instance *>(_instances.mapped);
    auto *cmd = static_cast<VkDrawIndirectCommand *>(_indirect.mapped);
    uint32_t n = 0;
    for (const auto &m : _meshes) {
        if (!m->visible || m->_vertexCount == 0) {
            continue;
        }
        std::memcpy(inst[n].transform, m->transform, sizeof(m->transform));
        std::memcpy(inst[n].color, m->color, sizeof(m->color));
        inst[n].material = m->material ? materialIndex(*m->material) : kNoMaterial;
        cmd[n] = {m->_vertexCount, 1, m->_firstVertex, n};
        ++n;
    }

    _device->Reserve(_materials, materials.size() * sizeof(GpuMaterial),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(materials.begin(), materials.end(), static_cast<GpuMaterial *>(_materials.mapped));

    _device->Reserve(_lightBuffer, _lights.size() * sizeof(GpuLight),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto *lights = static_cast<GpuLight *>(_lightBuffer.mapped);
    uint32_t lightCount = 0;
    for (const auto &l : _lights) {
        if (l->visible) {
            GpuLight &g = lights[lightCount++];
            std::copy(l->direction, l->direction + 3, g.direction);
            std::copy(l->irradiance, l->irradiance + 3, g.irradiance);
            g.direction[3] = g.irradiance[3] = 0;
        }
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
