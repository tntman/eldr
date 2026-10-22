#include "scene.h"

#include <algorithm>
#include <cstring>

namespace vkr {

Scene::Scene(Device *device)
    : _device(device)
{
}

Scene::~Scene()
{
    if (_device) {
        for (Buffer *b : {&_vertices, &_indices, &_instances, &_indirect}) {
            _device->Destroy(*b);
        }
    }
}

Mesh *Scene::AddMesh()
{
    _meshes.push_back(std::make_unique<Mesh>());
    _meshes.back()->_slot = _meshes.size() - 1;
    _poolsDirty = true;
    return _meshes.back().get();
}

void Scene::RemoveMesh(Mesh *mesh)
{
    const size_t slot = mesh->_slot;
    std::swap(_meshes[slot], _meshes.back());
    _meshes[slot]->_slot = slot;
    _meshes.pop_back();
    _poolsDirty = true;
}

// ponytail: any geometry edit re-packs every mesh into the pools.
// Sub-allocate with a free list when editing big scenes gets slow.
void Scene::_RebuildPools()
{
    size_t vertexCount = 0, indexCount = 0;
    for (const auto &m : _meshes) {
        vertexCount += m->points.size() / 3;
        indexCount += m->indices.size();
    }
    _device->Reserve(_vertices, vertexCount * 3 * sizeof(float),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    _device->Reserve(_indices, indexCount * sizeof(uint32_t),
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    auto *vdst = static_cast<float *>(_vertices.mapped);
    auto *idst = static_cast<uint32_t *>(_indices.mapped);
    uint32_t vertexOffset = 0, firstIndex = 0;
    for (const auto &m : _meshes) {
        m->geometryDirty = false;
        const uint32_t nverts = uint32_t(m->points.size() / 3);
        // Scene data is untrusted: an out-of-range index would make the GPU
        // read another mesh's vertices or past the pool.
        const bool valid = std::all_of(m->indices.begin(), m->indices.end(),
                                       [&](uint32_t i) { return i < nverts; });
        if (!valid) {
            m->_indexCount = 0;
            continue;
        }
        std::memcpy(vdst + size_t(vertexOffset) * 3, m->points.data(),
                    nverts * 3 * sizeof(float));
        std::memcpy(idst + firstIndex, m->indices.data(),
                    m->indices.size() * sizeof(uint32_t));
        m->_vertexOffset = int32_t(vertexOffset);
        m->_firstIndex = firstIndex;
        m->_indexCount = uint32_t(m->indices.size());
        vertexOffset += nverts;
        firstIndex += m->_indexCount;
    }
    _poolsDirty = false;
}

void Scene::Commit()
{
    if (!_device) {
        return;
    }
    if (_poolsDirty || std::any_of(_meshes.begin(), _meshes.end(),
                                   [](const auto &m) { return m->geometryDirty; })) {
        _RebuildPools();
    }

    // Per-draw data is small: rewrite all of it instead of tracking
    // transform/visibility/color dirtiness.
    _device->Reserve(_instances, _meshes.size() * sizeof(Instance),
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    _device->Reserve(_indirect, _meshes.size() * sizeof(VkDrawIndexedIndirectCommand),
                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    auto *inst = static_cast<Instance *>(_instances.mapped);
    auto *cmd = static_cast<VkDrawIndexedIndirectCommand *>(_indirect.mapped);
    uint32_t n = 0;
    for (const auto &m : _meshes) {
        if (!m->visible || m->_indexCount == 0) {
            continue;
        }
        std::memcpy(inst[n].transform, m->transform, sizeof(m->transform));
        std::memcpy(inst[n].color, m->color, sizeof(m->color));
        inst[n].color[3] = 1.0f;
        cmd[n] = {m->_indexCount, 1, m->_firstIndex, m->_vertexOffset, n};
        ++n;
    }
    _gpu = {_vertices.buffer, _indices.buffer, _instances.buffer, _indirect.buffer, n};
}

} // namespace vkr
