#pragma once

#include "device.h"
#include "ibl.h"

#include <map>
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
// triangle), so any primvar interpolation maps 1:1; Commit deduplicates the
// corners into indexed vertices. Different meshes may be written
// concurrently (Hydra syncs rprims in parallel); nothing here touches Vulkan.
struct Mesh {
    std::vector<float> points;   // xyz per corner
    std::vector<float> normals;  // xyz per corner, or empty (flat shading)
    std::vector<float> uvs;      // st per corner, or empty
    std::vector<float> colors;   // displayColor rgb per corner, or empty (constant `color`)
    // One world transform per instance, 16 floats each, USD row-vector order.
    std::vector<float> transforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float color[3] = {0.5f, 0.5f, 0.5f};  // used when material is null
    std::shared_ptr<Material> material;
    int32_t primId = -1;         // written to the primId target (Hydra picking)
    bool visible = true;
    bool geometryDirty = true;   // set after changing points/normals/uvs/joint influences
    bool transformsDirty = true; // set after changing transforms

    // Linear blend skinning (UsdSkel), skinned on the GPU. Up to 4 joints
    // per corner with weights summing to 1; per instance, jointCount
    // matrices (USD row-vector order) that take `points` straight to world
    // space, so `transforms` is ignored and the instance count is
    // jointMatrices.size() / (16 * jointCount). Only the matrices change per
    // frame: set jointsDirty alone.
    std::vector<uint16_t> jointIndices;  // 4 per corner; empty: rigid mesh
    std::vector<float> jointWeights;     // 4 per corner
    uint32_t jointCount = 0;
    std::vector<float> jointMatrices;
    bool jointsDirty = false;

private:
    friend class Scene;
    size_t _slot = 0;
    // Placement in the GPU pools (deduplicated, indexed vertices).
    uint32_t _vertexOffset = 0, _vertexCount = 0;
    uint32_t _firstIndex = 0, _indexCount = 0;
    uint32_t _instanceBase = 0, _instanceCount = 0;
    uint32_t _skinOffset = 0, _skinCount = 0;    // SkinVertex pool, one per vertex
    uint32_t _jointBase = 0, _jointSlots = 0;    // joint matrix pool, instances * jointCount
    std::vector<float> _jointBounds;             // per joint: rest bounds of the vertices it moves
    float _boundsMin[3] = {}, _boundsMax[3] = {};  // object space
    float _worldMin[3] = {}, _worldMax[3] = {};    // all instances, world space
};

// Lights in world space, same threading rules as Mesh. The Hydra layer resolves the UsdLux inputs
// (intensity, exposure, color, temperature, normalize) into irradiance for
// distant lights and the emitting surface's radiance for sphere/rect lights.
struct Light {
    enum class Type { Distant, Sphere, Rect, Disk };
    Type type = Type::Distant;
    float direction[3] = {0, 0, 1};      // distant: towards the light; sphere/rect: emission axis (light -Z)
    float irradiance[3] = {1, 1, 1};     // distant, linear RGB
    float radiance[3] = {1, 1, 1};       // sphere/rect, linear RGB
    float position[3] = {0, 0, 0};       // sphere/rect center
    float halfWidth[3] = {0.5f, 0, 0};   // rect: center to the +X edge; disk: radius along X
    float halfHeight[3] = {0, 0.5f, 0};  // rect: center to the +Y edge; disk: radius along Y
    float radius = 0.5f;                 // sphere
    bool point = false;                  // sphere: shade as a point (treatAsPoint), same power
    float coneAngle = 180.0f;            // UsdLuxShapingAPI around the emission axis, degrees
    float coneSoftness = 0.0f;
    float focus = 0.0f;
    float focusTint[3] = {0, 0, 0};
    float diffuse = 1.0f;                // UsdLux diffuse/specular multipliers
    float specular = 1.0f;
    bool visible = true;
    bool castShadows = false;            // distant: the first caster gets the CSM; others: atlas tiles

private:
    friend class Scene;
    size_t _slot = 0;
};

// Dome light (environment). The first visible one lights the scene.
struct Dome {
    std::shared_ptr<Texture> texture;    // latlong, see BuildEnvironment; null = uniform
    float radiance[3] = {1, 1, 1};       // texture multiplier: intensity * 2^exposure * color
    float worldToLocal[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // USD order
    bool visible = true;

private:
    friend class Scene;
    size_t _slot = 0;
};

// GPU layouts shared with the shaders (std430, see shaders/scene.glsl).
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint16_t color[4];   // half floats; only read when DrawInfo::vertexColors
};
struct Instance {
    float transform[16];
    uint32_t draw;       // mesh slot = index into the DrawInfo table; kNone: free slot
    int32_t index;       // index into Mesh::transforms (instanceId AOV)
    uint32_t joints;     // skinned: first joint matrix; transform then only bounds culling
    uint32_t pad;
};
struct DrawInfo {
    float boundsMin[4];  // object space
    float boundsMax[4];
    float color[4];      // displayColor
    uint32_t material;   // kNoMaterial: color + default material
    int32_t primId;
    uint32_t visible;
    uint32_t skin;        // first SkinVertex of the mesh, kNone if rigid
    uint32_t vertexBase;  // the mesh's first vertex in the vertex pool
    uint32_t vertexColors;  // 1: per-vertex displayColor, else `color`
    uint32_t pad[2];
};
// Joint influences of one skinned vertex: four 16-bit joint indices and
// four unorm16 weights.
struct SkinVertex {
    uint32_t joints[2];
    uint32_t weights[2];
};
constexpr uint32_t kNoMaterial = 0xFFFFFFFFu;
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kMaxTextures = 4096;

// Sub-allocates [0, capacity) ranges, first fit, coalescing on free.
class RangeAllocator {
public:
    bool Allocate(uint32_t count, uint32_t *offset);
    void Free(uint32_t offset, uint32_t count);
    void Grow(uint32_t capacity);  // adds [old capacity, capacity) as free
    uint32_t Capacity() const { return _capacity; }

private:
    std::map<uint32_t, uint32_t> _free;  // offset -> count
    uint32_t _capacity = 0;
};

// Owns meshes, lights and their GPU copy, and uploads only what changed:
// device-local vertex/index/instance pools (sub-allocated per mesh), a
// per-mesh DrawInfo table and indirect commands, material/light tables,
// textures and the active environment. Add*/Remove*/Commit are
// single-threaded.
class Scene {
public:
    explicit Scene(Device *device);  // null device: CPU mirror only
    ~Scene();

    Mesh *AddMesh();
    void RemoveMesh(Mesh *mesh);
    Light *AddLight();
    void RemoveLight(Light *light);
    Dome *AddDome();
    void RemoveDome(Dome *dome);

    // Uploads dirty data. The GPU must be idle.
    void Commit();

    struct Gpu {
        VkBuffer vertices = VK_NULL_HANDLE;
        VkBuffer indices = VK_NULL_HANDLE;
        VkBuffer instances = VK_NULL_HANDLE;   // Instance[instanceCapacity]
        // [0, cap): per-mesh lists of visible instances, written by culling;
        // [cap, 2*cap): identity, used by passes that don't cull (shadows).
        VkBuffer visible = VK_NULL_HANDLE;
        VkBuffer draws = VK_NULL_HANDLE;           // DrawInfo per mesh slot
        VkBuffer commandTemplate = VK_NULL_HANDLE; // per slot, instanceCount 0
        VkBuffer shadowCommands = VK_NULL_HANDLE;  // per slot, all instances
        VkBuffer materials = VK_NULL_HANDLE;
        VkBuffer lights = VK_NULL_HANDLE;      // distant lights, then sphere/rect lights
        VkBuffer skin = VK_NULL_HANDLE;        // SkinVertex pool
        VkBuffer joints = VK_NULL_HANDLE;      // joint matrix pool (mat4)
        uint32_t drawCount = 0;         // mesh slots (some may draw nothing)
        uint32_t instanceCapacity = 0;
        uint32_t lightCount = 0;        // distant
        uint32_t localLightCount = 0;   // sphere/rect, after the distant ones
        std::vector<VkImageView> textures;  // index = TexRef index in shaders

        uint32_t shadowLight = kNone;       // index into lights
        float shadowDirection[3] = {0, 0, 1};
        // Sphere/rect/disk lights that cast shadows; the renderer gives the
        // ones that matter most this frame tiles in its shadow atlas.
        struct ShadowCaster {
            uint32_t light;          // index into lights
            float position[3];
            float axis[3];           // emission axis
            float coneAngle;         // radians (UsdLux cone:angle; pi = no cone)
            float rangeIntensity;    // range = sqrt(rangeIntensity / cutoff) + extent, as LightRange()
            float extent;
            float radius;            // sphere radius: the shadow near plane stays outside it
            float size;              // emitter radius for soft shadows (0: point, hard)
        };
        std::vector<ShadowCaster> shadowCasters;
        bool hasBounds = false;
        float boundsMin[3] = {}, boundsMax[3] = {};  // world space, all instances

        VkImageView environment = VK_NULL_HANDLE;  // prefiltered cube; null if no dome
        uint32_t environmentLevels = 0;
        uint32_t environmentLatlong = kNone;  // bindless index of the dome texture
        float environmentSh[9][3] = {};
        float environmentRadiance[3] = {};
        float environmentWorldToLocal[16] = {};
    };
    const Gpu &GetGpu() const { return _gpu; }

private:
    // Batched uploads: bytes staged on the CPU, copied/filled in one submit.
    struct Copy {
        VkBuffer dst;
        VkDeviceSize dstOffset, srcOffset, size;
    };
    struct Fill {
        VkBuffer dst;
        VkDeviceSize offset, size;
    };
    VkDeviceSize _Stage(const void *data, VkDeviceSize size);
    void _Flush();
    void _EnsureCapacity(Buffer &buffer, RangeAllocator &alloc, uint32_t needed,
                         VkDeviceSize elementSize, VkBufferUsageFlags usage, bool invalidateNew);
    void _UploadGeometry(Mesh &mesh);
    void _UploadInstances(Mesh &mesh);
    void _UploadJoints(Mesh &mesh);
    void _FreeInstances(Mesh &mesh);
    void _UpdateWorldBounds(Mesh &mesh);
    void _Upload(Texture &texture);
    int32_t _TextureIndex(const TextureRef &ref);
    void _UpdateEnvironment(const Dome *dome);
    void _DestroyEnvironment();
    template <typename T>
    static void _RemoveSlot(std::vector<std::unique_ptr<T>> &items, T *item);

    Device *_device;
    std::vector<std::unique_ptr<Mesh>> _meshes;
    std::vector<std::unique_ptr<Light>> _lights;
    std::vector<std::unique_ptr<Dome>> _domes;
    // Device-local pools and their allocators.
    Buffer _vertices, _indices, _instances, _visible, _skin, _joints;
    RangeAllocator _vertexAlloc, _indexAlloc, _instanceAlloc, _skinAlloc, _jointAlloc;
    // Host-visible per-commit tables.
    Buffer _draws, _commandTemplate, _shadowCommands, _materials, _lightBuffer, _staging;
    std::vector<uint8_t> _stagingBytes;
    std::vector<Copy> _copies;
    std::vector<Fill> _fills;
    std::unordered_map<const Texture *, int32_t> _textureSlots;  // rebuilt per Commit

    // Environment built from this texture (kept alive so the pointer stays unique).
    std::shared_ptr<Texture> _envSource;
    bool _envBuilt = false;
    VkImage _envImage = VK_NULL_HANDLE;
    VkDeviceMemory _envMemory = VK_NULL_HANDLE;
    VkImageView _envView = VK_NULL_HANDLE;

    Gpu _gpu;
};

} // namespace vkr
