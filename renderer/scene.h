#pragma once

#include "device.h"
#include "ibl.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkr {

class Scene;

// Decoded image, rows bottom-up (so USD st (0,0) is the first texel),
// 4 channels, tightly packed. Its GPU copy is made by Scene::Commit and
// freed with the texture; drop the last reference only while the GPU is idle.
// One- and two-channel formats are gray and gray+alpha: sampling spreads
// the gray to rgb (alpha 1 without one), as UsdUVTexture reads them.
struct Texture {
    enum class Format { R8, RG8, RGBA8, RGBA8_SRGB, R16, RG16, RGBA16, R16F, RG16F, RGBA16F, RGBA32F };

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

uint32_t Channels(Texture::Format f);
uint32_t BytesPerChannel(Texture::Format f);

// texel * scale + bias, then one channel (0..3) or all of them (-1).
// UDIM tiles 1001..1100: tile 1001 + u + 10 v covers uv [u, u+1) x [v, v+1).
constexpr size_t kUdimTiles = 100;

struct TextureRef {
    std::shared_ptr<Texture> texture;  // null: use the material's constant
    // UDIM: kUdimTiles tiles, null where missing (texture is then one of
    // them); empty for a single texture.
    std::vector<std::shared_ptr<Texture>> udim;
    int channel = -1;
    float scale[4] = {1, 1, 1, 1};
    float bias[4] = {0, 0, 0, 0};
    // Texture coordinates: (u', v') = ((a b c) (d e f)) (u v 1), row-major.
    float uvTransform[6] = {1, 0, 0, 0, 1, 0};
};

// Material inputs a shading graph computes per pixel (MaterialX math,
// several textures, primvars), run by mesh.frag's interpreter
// (shaders/program.glsl). Each op is four words: opcode | result register
// << 8 | imm1 << 16; four operand bytes (a register below kRegisters, else
// constant byte - kRegisters); imm0. Values are vec4s: floats splat to all
// four components, vector2 is (x, y, 0, 0).
struct MaterialProgram {
    static constexpr uint32_t kRegisters = 8;
    enum Op : uint8_t {
        kOutput,       // material input imm0 (Program::Input) = a
        kTexcoord,     // the mesh's uv set
        kDisplayColor, // the mesh's displayColor (rgb, 1)
        kPrimvar,      // primvar slot imm0 (`primvars`), else a
        kImage,        // images[imm0] at uv a, else b (missing tile, no texture)
        kAdd, kSubtract, kMultiply, kDivide, kMin, kMax, kPower, kModulo,
        kAbs, kFloor, kCeil, kSin, kCos, kSqrt, kExp, kLog,
        kInvert,       // b - a
        kMix,          // mix(a = bg, b = fg, c)
        kOverlay,      // MaterialX overlay: a = fg over b = bg, mixed by c
        kClamp,        // clamp(a, b, c)
        kSmoothstep,   // smoothstep(b, c, a)
        kRemap,        // a from [b, c] to [d, constants[imm0]]
        kExtract,      // channel imm1 of a, splat
        kCombine,      // (a.x, b.x, c.x, d.x)
        kDot,          // dot(a.xyz, b.xyz), splat
        kNormalize,
        kLuminance,    // dot(a.rgb, b.rgb), splat
        kIfGreater,    // a.x > b.x ? c : d
        kIfGreaterEq,  // a.x >= b.x ? c : d
        kIfEqual,      // a.x == b.x ? c : d
        kHsvAdjust,    // a in HSV, hue + b.x, saturation * b.y, value * b.z
        kRgbToHsv, kHsvToRgb,
        kAffine,       // (dot(b.xyz, (a.xy, 1)), dot(c.xyz, (a.xy, 1)), 0, 0)
        kNormalMap,    // tangent-space normal of a texel a, xy scaled by b
        kInvLinear,    // Houdini hinvlinear: a among ten keys (constants from imm0, 4 per vec4), b.x = num
        kUniformRamp,  // Houdini huniformramp: ten values (constants from imm0) at a, b.x = num
    };  // as shaders/program.glsl numbers them
    // Material inputs kOutput writes, as GpuMaterial::tex orders them.
    enum Input : uint32_t { kBaseColor, kEmissive, kMetallic, kRoughness, kOcclusion, kNormal, kOpacity };

    std::vector<uint32_t> code;      // 4 words per op
    std::vector<float> constants;    // 4 per vec4
    std::vector<TextureRef> images;  // kImage's textures
    std::vector<std::string> primvars;  // kPrimvar's slots, by primvar name

    bool Empty() const { return code.empty(); }
    // An operand byte for a constant (only the first 256 - kRegisters are
    // reachable that way; see Valid).
    uint8_t Constant(float x, float y, float z, float w)
    {
        constants.insert(constants.end(), {x, y, z, w});
        return uint8_t(kRegisters + constants.size() / 4 - 1);
    }
    bool Valid() const { return kRegisters + constants.size() / 4 <= 256; }
    bool Outputs(Input input) const
    {
        for (size_t i = 0; i < code.size(); i += 4) {
            if ((code[i] & 0xFFu) == kOutput && code[i + 2] == input) {
                return true;
            }
        }
        return false;
    }
    void Emit(Op op, uint32_t result, std::initializer_list<uint8_t> args = {}, uint32_t imm0 = 0,
              uint32_t imm1 = 0)
    {
        uint32_t operands = 0, shift = 0;
        for (const uint8_t a : args) {
            operands |= uint32_t(a) << shift;
            shift += 8;
        }
        code.insert(code.end(), {uint32_t(op) | (result << 8) | (imm1 << 16), operands, imm0, 0u});
    }
};

// UsdPreviewSurface metallic workflow subset. Constants are used where the
// matching TextureRef has no texture, and `program` overrides both for the
// inputs it outputs.
struct Material {
    float baseColor[3] = {0.18f, 0.18f, 0.18f};
    float emissive[3] = {0, 0, 0};
    float metallic = 0;
    float roughness = 0.5f;
    float occlusion = 1;
    float opacity = 1;           // below 1 (or a texture) without a threshold: translucent
    float opacityThreshold = 0;  // > 0: a cutout, no surface where opacity is below it
    // Dielectric specular weight times colour (standard_surface's specular
    // and specular_color); metals keep theirs.
    float specular[3] = {1, 1, 1};
    bool baseColorFromDisplayColor = false;  // diffuseColor <- primvar reader "displayColor"
    TextureRef baseColorTex, emissiveTex, metallicTex, roughnessTex, occlusionTex,
        normalTex,  // tangent space, after scale/bias
        opacityTex;
    // ponytail: only the shading pass runs it; cutouts and the depth passes
    // use the constants and textures.
    MaterialProgram program;
    // Lobes beyond the metallic base (shaders/brdf.glsl), constants only.
    // ponytail: no textures or programs for them; add a TextureRef (or a
    // MaterialProgram::Input) per input when assets need it.
    // UsdPreviewSurface useSpecularWorkflow: F0 is specularColor (F90 white),
    // metallic is ignored.
    bool specularWorkflow = false;
    float specularColor[3] = {0, 0, 0};
    // Clearcoat: a second GGX lobe on top; the base beneath is tinted by
    // coatColor and loses what the coat reflects. Weight 0: none.
    float coat = 0;
    float coatRoughness = 0.01f;
    float coatF0 = 0.04f;  // from the coat's IOR
    float coatColor[3] = {1, 1, 1};
    // Sheen (fabric, fuzz): a microfiber lobe over the diffuse. Weight 0: none.
    float sheen = 0;
    float sheenColor[3] = {1, 1, 1};
    float sheenRoughness = 0.3f;
    // Thin-film iridescence on the base's specular. Weight 0: none.
    float thinFilm = 0;
    float thinFilmThickness = 0;  // nanometres
    float thinFilmIor = 1.5f;

    // Whether any lobe beyond the metallic base is on: such materials shade
    // with the renderer's LAYERS pipelines, the others never pay for them.
    bool Layered() const { return specularWorkflow || coat > 0 || sheen > 0 || thinFilm > 0; }
};

// A primvar a material program reads, from the mesh or its instancer: one
// value per mesh (constant), triangle (uniform), instance or corner (vertex;
// per vertex with Mesh::indices). One to four components.
// ponytail: vertex-rate primvars share four components per vertex (the
// first ones that fit); the rest read their fallback.
struct Primvar {
    enum class Rate : uint32_t { Constant = 1, Uniform, Instance, Vertex };
    std::string name;
    Rate rate = Rate::Constant;
    uint32_t components = 1;
    std::vector<float> values;
};

// CPU mirror of one mesh: a triangle list stored per corner (3 vertices per
// triangle), so any primvar interpolation maps 1:1; Commit deduplicates the
// corners into indexed vertices. Geometry built already indexed (curves,
// points) sets `indices` instead: then the per-corner arrays below hold one
// value per vertex and nothing is merged. Different meshes may be written
// concurrently (Hydra syncs rprims in parallel); nothing here touches Vulkan.
struct Mesh {
    std::vector<float> points;   // xyz per corner
    std::vector<float> normals;  // xyz per corner, or empty (flat shading)
    std::vector<float> uvs;      // st per corner, or empty
    std::vector<float> colors;   // displayColor rgb per corner, or empty (constant `color`)
    std::vector<uint32_t> indices;  // 3 per triangle, or empty (the arrays are per corner)
    // One world transform per instance, 16 floats each, USD row-vector order.
    std::vector<float> transforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // The transforms they replace, when they change (motion vectors of
    // rigid meshes); Commit uploads and clears them.
    std::vector<float> prevTransforms;
    float color[3] = {0.5f, 0.5f, 0.5f};  // used when material is null
    std::shared_ptr<Material> material;
    int32_t primId = -1;         // written to the primId target (Hydra picking)
    bool visible = true;
    uint32_t tag = 1;            // caller's render tag bit: drawn when in the scene's tag mask
    bool geometryDirty = true;   // set after changing points/normals/uvs/joint influences
    bool transformsDirty = true; // set after changing transforms
    // Primvars the material's program reads, by name; vertex-rate ones are
    // geometry (geometryDirty), the others are read again every Commit.
    std::vector<Primvar> primvars;

    // UsdSkel skinning, on the GPU: linear blend, or dual quaternions with
    // `dualQuaternion`. 4 or 8 joints per corner (`influences`), strongest
    // first, with weights summing to 1, and jointCount skinning transforms (USD
    // row-vector order) that take `points` to the posed mesh in its own
    // space; `transforms` then place each instance as usual. One set is
    // shared by all instances, or there is one per instance
    // (instances * jointCount). Per frame only the skinning transforms
    // change: set jointsDirty alone. The GPU combines them with the instance
    // transforms into the joint matrices.
    std::vector<uint16_t> jointIndices;  // `influences` per corner; empty: rigid mesh
    std::vector<float> jointWeights;     // `influences` per corner
    uint32_t influences = 4;             // 4 or 8
    bool dualQuaternion = false;
    uint32_t jointCount = 0;
    std::vector<float> skinningTransforms;
    bool jointsDirty = false;
    // UsdSkel blend shapes, added to the rest points before skinning: per
    // corner a range of offsets (xyz, shape index); blendShapeCount weights
    // set together with the skinning transforms (shared, or per instance).
    std::vector<float> blendOffsets;     // 4 per offset
    std::vector<uint32_t> blendRanges;   // 2 per corner: first offset, count; or empty
    uint32_t blendShapeCount = 0;
    std::vector<float> blendWeights;

private:
    friend class Scene;
    size_t _slot = 0;
    // Placement in the GPU pools (deduplicated, indexed vertices).
    uint32_t _vertexOffset = 0, _vertexCount = 0;
    uint32_t _firstIndex = 0, _indexCount = 0;
    uint32_t _instanceBase = 0, _instanceCount = 0;
    uint32_t _skinOffset = 0, _skinCount = 0;    // SkinVertex pool, _skinStride per vertex
    uint32_t _skinStride = 1;
    uint32_t _jointBase = 0, _jointSlots = 0;    // joint matrices (GPU-built), instances * jointCount
    uint32_t _skinXformBase = 0, _skinXformSlots = 0;  // skinning transforms
    uint32_t _boundsBase = 0, _boundsSlots = 0;  // JointBounds pool, one per joint
    uint32_t _blendBase = 0, _blendSlots = 0;    // blend shape offsets
    uint32_t _weightBase = 0, _weightSlots = 0;  // blend shape weights
    // Vertex-rate primvars packed in one vec4 per vertex (_vertexCount),
    // and per entry of `primvars` its first component there (kNone: not packed).
    uint32_t _primvarVertexOffset = 0, _primvarVertexCount = 0;
    std::vector<uint32_t> _primvarComponents;
    bool _blendPerInstance = false;
    bool _skinPerInstance = false;
    std::vector<float> _jointBounds;             // per joint: rest bounds of the vertices it moves
    float _posedMin[3] = {}, _posedMax[3] = {};  // object space, all poses
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
    float rows[3][4];    // affine transform, column-vector rows (x' = rows[0] . (p, 1))
    uint32_t draw;       // mesh slot = index into the DrawInfo table; kNone: free slot
    int32_t index;       // index into Mesh::transforms (instanceId AOV)
    uint32_t joints;     // skinned: first of the instance's joint matrices
    uint32_t pad;
};
static_assert(sizeof(Instance) == 64, "std430 mirror in shaders/scene.glsl");
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
    uint32_t jointCount;
    uint32_t skinXforms;    // first skinning transform
    uint32_t jointBounds;   // first JointBounds
    uint32_t skinPerInstance;  // 1: jointCount skinning transforms per instance
    uint32_t blendWeights;     // first blend shape weight
    uint32_t blendShapes;      // weights per pose; 0: no blend shapes
    uint32_t blendPerInstance; // 1: blendShapes weights per instance
    uint32_t motion;           // first instance's rows in Gpu::motion, kNone: didn't move
    uint32_t primvars;         // the material program's primvar slots in Gpu::primvarTable, kNone: none
    uint32_t primvarVertices;  // the mesh's first vec4 in Gpu::primvarVertices, kNone: none
};
static_assert(sizeof(DrawInfo) % 16 == 0, "std430 array stride of DrawInfo");
// Rest bounds of the vertices one joint moves (min.x > max.x: none).
struct JointBounds {
    float min[4];
    float max[4];
};
// Joint influences of one skinned vertex: four 16-bit joint indices and
// four unorm16 weights.
// Per vertex; meshes with eight influences have two, the second holding
// influences 4..7 (drawn by the renderer's kWideSkin pipelines).
struct SkinVertex {
    uint32_t joints[2];   // four 16-bit joint indices
    uint32_t weights[2];  // four unorm16 weights
    uint32_t blend[2];    // first blend shape offset, count
    uint32_t pad[2];
};
constexpr uint32_t kNoMaterial = 0xFFFFFFFFu;
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kMaxTextures = 4096;
// Lists of instance indices in Gpu::visible, instanceCapacity each: [0]
// culled for the camera, [1, 5) culled for the shadow cascades, [5] for the
// shadow atlas tile being drawn (reused tile after tile). Passes that draw
// every instance read the instance pool directly (list kNone).
constexpr uint32_t kVisibleLists = 6;

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
    // Meshes whose tag has no bit in `mask` are not drawn (from the next
    // Commit). Returns whether the mask changed.
    bool SetTagMask(uint32_t mask);

    struct Gpu {
        VkBuffer vertices = VK_NULL_HANDLE;
        VkBuffer indices = VK_NULL_HANDLE;
        VkBuffer instances = VK_NULL_HANDLE;   // Instance[instanceCapacity]
        // kVisibleLists lists of instance indices, see kVisibleLists. Draws
        // index list L at L * instanceCapacity + firstInstance.
        VkBuffer visible = VK_NULL_HANDLE;
        VkBuffer draws = VK_NULL_HANDLE;           // DrawInfo per mesh slot
        VkBuffer commandTemplate = VK_NULL_HANDLE; // per slot, instanceCount 0
        VkBuffer materials = VK_NULL_HANDLE;
        VkBuffer lights = VK_NULL_HANDLE;      // distant lights, then sphere/rect lights
        VkBuffer skin = VK_NULL_HANDLE;        // SkinVertex pool
        VkBuffer joints = VK_NULL_HANDLE;      // joint matrices, built on the GPU (mat4)
        VkDeviceSize jointsBytes = 0;
        // Last commit's transforms (3 rows of vec4 per instance) of rigid
        // meshes that moved since: their motion vectors.
        VkBuffer motion = VK_NULL_HANDLE;
        bool moved = false;  // the last commit moved meshes or joints (motion blur)
        uint64_t instanceCount = 0, triangleCount = 0;  // drawable, before culling
        VkBuffer skinXforms = VK_NULL_HANDLE;  // skinning transforms (mat4)
        VkBuffer jointBounds = VK_NULL_HANDLE; // JointBounds pool
        VkBuffer blendOffsets = VK_NULL_HANDLE; // blend shape offsets (vec4)
        VkBuffer blendWeights = VK_NULL_HANDLE; // blend shape weights (float)
        VkBuffer udimTiles = VK_NULL_HANDLE;    // per UDIM texture kUdimTiles bindless indices, -1: missing
        // Material programs: their ops (uvec4) and constants (vec4, images'
        // GpuTexRefs after each program's own constants).
        VkBuffer programCode = VK_NULL_HANDLE;
        VkBuffer programConstants = VK_NULL_HANDLE;
        // Per draw with a program, per primvar slot (uvec4): Primvar::Rate
        // (0: missing), components, first value in primvarData, first
        // component in its vertex's primvarVertices vec4.
        VkBuffer primvarTable = VK_NULL_HANDLE;
        VkBuffer primvarData = VK_NULL_HANDLE;      // float
        VkBuffer primvarVertices = VK_NULL_HANDLE;  // vec4 per vertex of meshes with vertex-rate primvars
        uint32_t drawCount = 0;         // mesh slots (some may draw nothing)
        // Slots are drawn in indirect calls of [drawChunks[i], drawChunks[i + 1]),
        // each small enough to stay well under Windows' ~2 s GPU timeout (TDR)
        // even with every instance visible.
        std::vector<uint32_t> drawChunks;
        // Per chunk: kOpaque, kCutout (alpha-tested depth passes) or
        // kTranslucent (skipped: drawn per slot, back to front, after the
        // opaque), plus kWide (eight skinning influences), kDualQuat and
        // kProgram (a material program, shading pass only), kLayers (lobes
        // beyond the metallic base, Material::Layered; shading pass only):
        // kind >> 2 picks the renderer's pipeline variant. Chunks also end
        // where it changes.
        enum : uint8_t { kOpaque, kCutout, kTranslucent, kWide = 4, kDualQuat = 8, kProgram = 16, kLayers = 32 };
        std::vector<uint8_t> chunkKind;
        // The translucent slots (with their pipeline variants) and their
        // world bounds' centres (xyz).
        std::vector<uint32_t> translucent;
        std::vector<uint8_t> translucentVariant;
        std::vector<float> translucentCenters;
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
        // Changes whenever anything a shadow map could see changed (geometry,
        // instances, poses, visibility); unique across scenes.
        uint64_t version = 0;
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

    // Bytes of the GPU pools: per-vertex and per-instance data (skinning,
    // blend shapes, joints and vertex-rate primvars included), and triangle
    // indices.
    VkDeviceSize VertexBytes() const;
    VkDeviceSize IndexBytes() const { return _indices.size; }

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
    int32_t _TextureIndex(Texture *t);
    void _UpdateEnvironment(const Dome *dome);
    void _DestroyEnvironment();
    template <typename T>
    static void _RemoveSlot(std::vector<std::unique_ptr<T>> &items, T *item);

    Device *_device;
    std::vector<std::unique_ptr<Mesh>> _meshes;
    std::vector<std::unique_ptr<Light>> _lights;
    std::vector<std::unique_ptr<Dome>> _domes;
    // Device-local pools and their allocators.
    Buffer _vertices, _indices, _instances, _visible, _skin, _joints, _skinXforms, _jointBoundsPool,
        _blendOffsets, _blendWeights, _primvarVertices;
    RangeAllocator _vertexAlloc, _indexAlloc, _instanceAlloc, _skinAlloc, _jointAlloc, _skinXformAlloc,
        _boundsAlloc, _blendAlloc, _weightAlloc, _primvarVertexAlloc;
    // Host-visible per-commit tables.
    Buffer _draws, _commandTemplate, _materials, _udimTiles, _motion, _lightBuffer, _staging, _programCode,
        _programConstants, _primvarTable, _primvarData;
    std::vector<uint8_t> _stagingBytes;
    std::vector<Copy> _copies;
    std::vector<Fill> _fills;
    std::unordered_map<const Texture *, int32_t> _textureSlots;  // rebuilt per Commit

    // Environment built from this texture (kept alive so the pointer stays unique).
    std::shared_ptr<Texture> _envSource;
    bool _envBuilt = false;
    uint32_t _tagMask = ~0u;
    bool _changed = true;           // since the last Commit: a mesh was added or removed
    std::vector<uint32_t> _lastDrawn;  // per slot: instances shadows drew last Commit
    VkImage _envImage = VK_NULL_HANDLE;
    VkDeviceMemory _envMemory = VK_NULL_HANDLE;
    VkImageView _envView = VK_NULL_HANDLE;

    Gpu _gpu;
};

} // namespace vkr
