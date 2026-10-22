#pragma once

#include "device.h"

#include <memory>
#include <vector>

namespace vkr {

// CPU mirror of one mesh. Different meshes may be written concurrently
// (Hydra syncs rprims in parallel); nothing here touches Vulkan.
struct Mesh {
    std::vector<float> points;      // xyz
    std::vector<uint32_t> indices;  // triangle list
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // USD row-vector order
    float color[3] = {0.5f, 0.5f, 0.5f};
    bool visible = true;
    bool geometryDirty = true;      // set after changing points or indices

private:
    friend class Scene;
    size_t _slot = 0;
    uint32_t _firstIndex = 0;
    uint32_t _indexCount = 0;
    int32_t _vertexOffset = 0;
};

// Per-draw data, read as instance-rate vertex attributes via firstInstance.
struct Instance {
    float transform[16];
    float color[4];
};

// Owns all meshes and their GPU copy: one vertex pool, one index pool,
// one instance buffer and one indirect-draw buffer.
// AddMesh/RemoveMesh/Commit are single-threaded.
class Scene {
public:
    explicit Scene(Device *device);  // null device: CPU mirror only
    ~Scene();

    Mesh *AddMesh();
    void RemoveMesh(Mesh *mesh);

    // Uploads dirty data. The GPU must be idle.
    void Commit();

    struct Gpu {
        VkBuffer vertices = VK_NULL_HANDLE;
        VkBuffer indices = VK_NULL_HANDLE;
        VkBuffer instances = VK_NULL_HANDLE;
        VkBuffer indirect = VK_NULL_HANDLE;
        uint32_t drawCount = 0;
    };
    const Gpu &GetGpu() const { return _gpu; }

private:
    void _RebuildPools();

    Device *_device;
    std::vector<std::unique_ptr<Mesh>> _meshes;
    bool _poolsDirty = true;
    Buffer _vertices, _indices, _instances, _indirect;
    Gpu _gpu;
};

} // namespace vkr
