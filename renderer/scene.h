#pragma once

#include "device.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace vkr {

class Scene;

// Decoded image, rows bottom-up (so USD st (0,0) is the first texel),
// 4 channels, tightly packed. Its GPU copy is made by Scene::Commit and
// freed with the texture; drop the last reference only while the GPU is idle.
struct Texture {
    enum class Format { RGBA8, RGBA8_SRGB, RGBA16F, RGBA32F };

    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::RGBA8;
    std::vector<uint8_t> pixels;

    Texture() = default;
    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;
    ~Texture();

private:
    friend class Scene;
    Device *_device = nullptr;
    VkImage _image = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
    VkImageView _view = VK_NULL_HANDLE;
};

// texel * scale + bias, then one channel (0..3) or all of them (-1).
struct TextureRef {
    std::shared_ptr<Texture> texture;  // null: use the material's constant
    int channel = -1;
    float scale[4] = {1, 1, 1, 1};
    float bias[4] = {0, 0, 0, 0};
};

// UsdPreviewSurface metallic workflow subset. Constants are used where the
// matching TextureRef has no texture.
struct Material {
    float baseColor[3] = {0.18f, 0.18f, 0.18f};
    float emissive[3] = {0, 0, 0};
    float metallic = 0;
    float roughness = 0.5f;
    float occlusion = 1;
    bool baseColorFromDisplayColor = false;  // diffuseColor <- primvar reader "displayColor"
    TextureRef baseColorTex, emissiveTex, metallicTex, roughnessTex, occlusionTex,
        normalTex;  // tangent space, after scale/bias
};

// CPU mirror of one mesh: a triangle list stored per corner (3 vertices per
// triangle, no index buffer), so any primvar interpolation maps 1:1.
// Different meshes may be written concurrently (Hydra syncs rprims in
// parallel); nothing here touches Vulkan.
struct Mesh {
    std::vector<float> points;   // xyz per corner
    std::vector<float> normals;  // xyz per corner, or empty (flat shading)
    std::vector<float> uvs;      // st per corner, or empty
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // USD row-vector order
    float color[3] = {0.5f, 0.5f, 0.5f};  // used when material is null
    std::shared_ptr<Material> material;
    bool visible = true;
    bool geometryDirty = true;   // set after changing points/normals/uvs

private:
    friend class Scene;
    size_t _slot = 0;
    uint32_t _firstVertex = 0;
    uint32_t _vertexCount = 0;
};

// Distant light. Same threading rules as Mesh.
struct Light {
    float direction[3] = {0, 0, 1};      // world space, pointing at the light
    float irradiance[3] = {1, 1, 1};     // linear RGB, all scaling applied
    bool visible = true;

private:
    friend class Scene;
    size_t _slot = 0;
};

// GPU layouts shared with the shaders (std430).
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};
struct Instance {
    float transform[16];
    float color[3];
    uint32_t material;  // kNoMaterial: color + default material
};
constexpr uint32_t kNoMaterial = 0xFFFFFFFFu;
constexpr uint32_t kMaxTextures = 4096;

// Owns meshes, lights and their GPU copy: one vertex pool, one instance
// buffer, one indirect-draw buffer, material/light tables and textures.
// Add*/Remove*/Commit are single-threaded.
class Scene {
public:
    explicit Scene(Device *device);  // null device: CPU mirror only
    ~Scene();

    Mesh *AddMesh();
    void RemoveMesh(Mesh *mesh);
    Light *AddLight();
    void RemoveLight(Light *light);

    // Uploads dirty data. The GPU must be idle.
    void Commit();

    struct Gpu {
        VkBuffer vertices = VK_NULL_HANDLE;
        VkBuffer instances = VK_NULL_HANDLE;
        VkBuffer indirect = VK_NULL_HANDLE;
        VkBuffer materials = VK_NULL_HANDLE;
        VkBuffer lights = VK_NULL_HANDLE;
        uint32_t drawCount = 0;
        uint32_t lightCount = 0;
        std::vector<VkImageView> textures;  // index = TexRef index in shaders
    };
    const Gpu &GetGpu() const { return _gpu; }

private:
    void _RebuildVertexPool();
    void _Upload(Texture &texture);
    int32_t _TextureIndex(const TextureRef &ref);

    Device *_device;
    std::vector<std::unique_ptr<Mesh>> _meshes;
    std::vector<std::unique_ptr<Light>> _lights;
    bool _poolDirty = true;
    Buffer _vertices, _instances, _indirect, _materials, _lightBuffer, _staging;
    std::unordered_map<const Texture *, int32_t> _textureSlots;  // rebuilt per Commit
    Gpu _gpu;
};

} // namespace vkr
