#pragma once

#include "device.h"
#include "ibl.h"
#include "volume.h"
#include "lod.h"

#include <future>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkr {

class Scene;

// How a texture reads outside st [0, 1] (UsdUVTexture wrapS/wrapT, MaterialX
// uaddressmode/vaddressmode): Black is a transparent black border. Each s, t
// pair is its own sampler, so the shaders don't wrap.
enum class Wrap : uint8_t { Repeat, Mirror, Clamp, Black };
constexpr uint32_t kWrapModes = 4;

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
    // The file's own wrap modes (s, t) from its metadata, what UsdUVTexture's
    // "useMetadata" reads; Black where it has none, as in Storm.
    Wrap fileWrap[2] = {Wrap::Black, Wrap::Black};

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
    // Wrap modes: s, t. UDIM tiles always repeat inside their tile.
    Wrap wrap[2] = {Wrap::Repeat, Wrap::Repeat};
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
        // MaterialX procedurals (shaders/noise.glsl) at a, in imm0 = 2 or 3
        // dimensions, imm1 = 1..4 components out (1: splat).
        kNoise,        // noise2d/3d: value * b + c.x
        kFractal,      // fractal2d/3d: value * b, c = (octaves, lacunarity, diminish)
        kCellNoise,    // cellnoise2d/3d (float)
        kWorleyNoise,  // worleynoise2d/3d: b.x = jitter, c.x = style
        kPosition,     // the surface point, imm0: 0 object space, 1 world
        kGeomNormal,   // the interpolated normal, imm0: 0 object space, 1 world
    };  // as shaders/program.glsl numbers them
    // Material inputs kOutput writes, as GpuMaterial::tex orders them, then
    // the lobes' (Material's constants of the same names; brdf.glsl reads
    // them, in the LAYERS shaders only).
    enum Input : uint32_t {
        kBaseColor, kEmissive, kMetallic, kRoughness, kOcclusion, kNormal, kOpacity,
        kSpecularColor, kCoat, kCoatRoughness, kCoatColor, kSheen, kSheenColor, kSheenRoughness, kAnisotropy,
        kThinFilmThickness
    };

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
    // Whether it runs kNoise .. kGeomNormal: such programs get pipelines of
    // their own, so that code costs the others nothing.
    bool Procedural() const
    {
        for (size_t i = 0; i < code.size(); i += 4) {
            const uint32_t op = code[i] & 0xFFu;
            if (op >= kNoise && op <= kGeomNormal) {
                return true;
            }
        }
        return false;
    }
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
    // Lobes beyond the metallic base (shaders/brdf.glsl): constants, which
    // `program` overrides for the inputs it outputs (MaterialProgram::Input
    // from kSpecularColor on: textured or graph-driven lobes).
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
    // Anisotropic specular, stretched along the uv set's u direction turned
    // by the rotation (in turns). 0: isotropic.
    float anisotropy = 0;
    float anisotropyRotation = 0;
    bool openPbrAnisotropy = false;  // OpenPBR's roughness mapping, else standard_surface's
    // NPR toon: the lit result's intensity quantized into this many flat
    // bands per unit (0: none, or Renderer::Frame::toonBands), their edges
    // `toonSoftness` of a band wide.
    float toonBands = 0;
    float toonSoftness = 0.05f;

    // Whether any lobe beyond the metallic base is on: such materials shade
    // with the renderer's LAYERS pipelines, the others never pay for them.
    bool Layered() const
    {
        if (specularWorkflow || coat > 0 || sheen > 0 || thinFilm > 0 || anisotropy > 0 || toonBands > 0) {
            return true;
        }
        for (uint32_t i = MaterialProgram::kSpecularColor; i <= MaterialProgram::kThinFilmThickness; ++i) {
            if (program.Outputs(MaterialProgram::Input(i))) {
                return true;  // a textured or graph-driven lobe
            }
        }
        return false;
    }
    // Transmission (refraction, transparency.glsl): of the surface opacity
    // leaves, `transmission` lets light through, tinted by
    // transmissionColor (Beer-Lambert at transmissionDepth along the path
    // inside; 0: at the surface) and bent by ior unless thinWalled. With
    // opacityRefracts what opacity lets through is refracted too
    // (UsdPreviewSurface with an ior). Constants only.
    float transmission = 0;
    float transmissionColor[3] = {1, 1, 1};
    float transmissionDepth = 0;
    float ior = 1.5f;
    bool thinWalled = false;
    bool opacityRefracts = false;
    // Subsurface scattering (subsurface.h): the weight of the diffuse
    // light that scatters under the surface, its colour, and how far each
    // channel travels (mean free path, world units). Constants only.
    float subsurface = 0;
    float subsurfaceColor[3] = {1, 1, 1};
    float subsurfaceRadius[3] = {1, 1, 1};

    // Displacement (mesh.vert, every pass): an offset along the vertex
    // normal in object units, displacementTex's texel (its channel, scale
    // and bias) where it has a texture, else `displacement`; the shading
    // pass bumps the normal by the texture's slope. displacementBound: the
    // largest offset either way (culling bounds).
    float displacement = 0;
    float displacementBound = 0;
    TextureRef displacementTex;
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
    // Shade as hair (BasisCurves): strand highlights along the uv set's u,
    // which runs along each curve (shaders/brdf.glsl).
    bool hair = false;

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
    // Holdout (matte): drawn black with zero alpha, cut out of the image
    // but still hiding what's behind it and casting shadows.
    bool holdout = false;
    // Planar reflections: make this (flat) mesh the mirror
    // (primvars:hdvk:planarReflection), whatever its size and roughness.
    bool planarReflection = false;

private:
    friend class Scene;
    size_t _slot = 0;
    // Planar reflections: all points on one plane, through this triangle
    // (object space, three points).
    bool _flat = false;
    float _flatTriangle[9] = {};
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
    // Ray tracing: unique per geometry upload (a new BLAS when it changes).
    uint64_t _geometryVersion = 0;
    // Emissive lights: the object-space area of `points`, at this version.
    float _emitterArea = 0;
    uint64_t _emitterAreaVersion = 0;
    // Levels of detail (lod.h), coarser and coarser: their triangles in the
    // index pool after the mesh's own, and instance slots (never valid)
    // reserving levels 2 and 3 their range of the visible lists.
    struct Lod {
        uint32_t firstIndex, indexCount;
        float error;  // relative to the bounding radius
    };
    std::vector<Lod> _lods;
    uint32_t _lodFirstIndex = 0, _lodIndexCount = 0;
    uint32_t _lodInstanceBase = 0, _lodInstanceCount = 0;
    uint32_t _lodGeneration = 0;  // geometry uploads, to drop stale levels
};

// Lights in world space, same threading rules as Mesh. The Hydra layer resolves the UsdLux inputs
// (intensity, exposure, color, temperature, normalize) into irradiance for
// distant lights and the emitting surface's radiance for sphere/rect lights.
struct Light {
    enum class Type { Distant, Sphere, Rect, Disk, Cylinder };
    Type type = Type::Distant;
    float direction[3] = {0, 0, 1};      // distant: towards the light; sphere/rect: emission axis (light -Z)
    float irradiance[3] = {1, 1, 1};     // distant, linear RGB
    float radiance[3] = {1, 1, 1};       // sphere/rect, linear RGB
    float position[3] = {0, 0, 0};       // sphere/rect center
    // Rect: center to the +X edge; disk: radius along X; cylinder: center to
    // the +X end of its axis (half its length).
    float halfWidth[3] = {0.5f, 0, 0};
    float halfHeight[3] = {0, 0.5f, 0};  // rect: center to the +Y edge; disk: radius along Y
    float radius = 0.5f;                 // sphere, cylinder
    // Sphere: shade as a point (treatAsPoint); cylinder: as a line
    // (treatAsLine). Same power, only the highlight loses the radius.
    bool point = false;
    float coneAngle = 180.0f;            // UsdLuxShapingAPI around the emission axis, degrees
    float coneSoftness = 0.0f;
    float focus = 0.0f;
    float focusTint[3] = {0, 0, 0};
    float diffuse = 1.0f;                // UsdLux diffuse/specular multipliers
    float specular = 1.0f;
    bool visible = true;
    bool castShadows = false;            // distant: the first caster gets the CSM; others: atlas tiles
    float angle = 0.53f;                 // distant: the sun disk's angular diameter, degrees (traced, soft shadows)

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
    float boundsMax[4];  // w: Material::displacementBound
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
    uint32_t hair;             // 1: shaded as hair (Mesh::hair, shaders/brdf.glsl)
    uint32_t holdout;          // 1: Mesh::holdout
    uint32_t pad[2];
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
// Levels of detail: meshes from kLodMinTriangles get up to kMaxLods coarser
// levels, each about half the triangles, none moving the surface more than
// kLodMaxError of the mesh's bounding radius.
constexpr uint32_t kLodMinTriangles = 1000, kMaxLods = 3, kLodMaxInstances = 1u << 20;
constexpr float kLodMaxError = 0.25f;
// How much a change of a vertex normal or colour (per component) costs, as
// the share of the bounding radius the surface would have to move.
constexpr float kLodNormalWeight = 1.0f, kLodColorWeight = 1.0f;

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
    Volume *AddVolume();
    void RemoveVolume(Volume *volume);

    // Uploads dirty data. The GPU must be idle.
    void Commit();
    // Levels of detail are still building (a later Commit adds them).
    bool LodsPending() const { return !_pendingLods.empty(); }
    // Meshes whose tag has no bit in `mask` are not drawn (from the next
    // Commit). Returns whether the mask changed.
    bool SetTagMask(uint32_t mask);
    // Emissive meshes as lights: from the next Commit, each instance of a
    // small rigid mesh whose material emits at least `threshold` (a constant
    // emissiveColor's brightest channel, scene-referred radiance) also
    // lights the scene as a sphere light around it, of the same power
    // (pi x radiance x area), casting shadows. Returns whether it changed.
    bool SetEmissiveLights(bool on, float threshold);

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
        uint32_t drawCount = 0;         // command slots: meshCount meshes' (some may draw nothing), then levels'
        // Slots are drawn in indirect calls of [drawChunks[i], drawChunks[i + 1]),
        // each small enough to stay well under Windows' ~2 s GPU timeout (TDR)
        // even with every instance visible.
        std::vector<uint32_t> drawChunks;
        // Per chunk: kOpaque, kCutout (alpha-tested depth passes) or
        // kTranslucent (skipped: drawn per slot, back to front, after the
        // opaque), plus kWide (eight skinning influences), kDualQuat and
        // kProgram (a material program, shading pass only) with kProcedural
        // (it runs noise, MaterialProgram::Procedural), kLayers (lobes
        // beyond the metallic base, Material::Layered, or Mesh::hair; shading
        // pass only), kDisplaced (a displacing material; every pass): kind >>
        // 2 picks the renderer's pipeline variant. Chunks also end where it
        // changes.
        enum : uint8_t {
            kOpaque, kCutout, kTranslucent, kWide = 4, kDualQuat = 8, kProgram = 16, kProcedural = 32, kLayers = 64,
            kDisplaced = 128
        };
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
        // Per texture its sampler's wrap modes: s + kWrapModes * t (Wrap).
        std::vector<uint8_t> textureWraps;

        uint32_t shadowLight = kNone;       // index into lights
        float shadowDirection[3] = {0, 0, 1};
        float shadowAngle = 0;              // its Light::angle (soft sun shadows)
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
        // Blend shape motion: bytes of blendWeights (the renderer keeps a copy).
        VkDeviceSize blendWeightsBytes = 0;

        // Ray tracing (RayTracing builds a BLAS per slot from these, per
        // instance for skinned ones): per mesh slot its vertices and
        // triangles in the pools, `version` unique per upload, 0 for slots
        // ray tracing skips (not drawn or translucent).
        struct SlotGeometry {
            uint32_t vertexOffset, vertexCount, firstIndex, indexCount;
            uint64_t version;
            bool cutout;   // alpha-tested: any-hit shaders run
            bool emitter;  // every instance is also a light (SetEmissiveLights): GI must not count it twice
            bool skinned;  // posed per instance (LargestSkinned)
        };
        std::vector<SlotGeometry> slotGeometry;
        // A drawn translucent material refracts or tints what's behind it
        // (Material::transmission, opacityRefracts): the renderer copies
        // the opaque colour for it.
        bool refraction = false;
        bool holdouts = false;  // a drawn mesh is a holdout (Mesh::holdout)
        // Slots of opaque and cutout meshes with subsurface materials (and
        // their pipeline variants), shaded again by the subsurface pass.
        std::vector<uint32_t> subsurface;
        std::vector<uint8_t> subsurfaceVariant;

        // Volumetric fog, reflection probes: changes with what lighting
        // depends on besides geometry (the light and material tables, the
        // dome), so their histories and captures know when to start over.
        uint64_t lightingHash = 0;
        // Planar reflections: the mirror, the flat mesh flagged by
        // Mesh::planarReflection or else the largest flat, glossy (roughness
        // up to 0.3), opaque one at least a third of the scene's size; kNone:
        // none. Its first instance's plane (unit normal n, n . x + d = 0) and
        // world bounds.
        uint32_t mirrorSlot = kNone;
        float mirrorPlane[4] = {};
        float mirrorMin[3] = {}, mirrorMax[3] = {};

        // Volumes: a GpuVolume per drawn instance, and the NanoVDB grids
        // they read (both bound even when empty).
        VkBuffer volumes = VK_NULL_HANDLE;
        VkBuffer volumeGrids = VK_NULL_HANDLE;
        uint32_t volumeCount = 0;
        // Displacement: per material (as `materials`) its constant, bound
        // and texture (shaders/displace.glsl); whether a drawn one displaces.
        VkBuffer displacements = VK_NULL_HANDLE;
        bool displaced = false;
        // Levels of detail: per mesh slot its LOD commands and their ranges
        // of the visible lists (shaders/cull.comp's Lod). The LOD commands
        // are slots after the meshes' (drawCount counts both): level 1
        // fills the mesh's range from its end, levels 2 and 3 a second
        // range from either end.
        VkBuffer lods = VK_NULL_HANDLE;
        uint32_t meshCount = 0;  // mesh slots, the first of the drawCount slots
    };
    const Gpu &GetGpu() const { return _gpu; }

    // Ray tracing: up to `count` instances of skinned meshes it traces
    // (Gpu::SlotGeometry), the largest as seen from `eye` first (their
    // posed bounds' radius over the distance to their centre): a crowd's
    // nearest agents, a character's body before its teeth.
    struct SkinnedInstance {
        uint32_t instance, slot;  // instance slot, mesh slot
        uint32_t flags;           // 1: eight influences, 2: dual quaternions
    };
    std::vector<SkinnedInstance> LargestSkinned(const float eye[3], uint32_t count) const;

    // Bytes of the GPU pools: per-vertex and per-instance data (skinning,
    // blend shapes, joints and vertex-rate primvars included), and triangle
    // indices.
    VkDeviceSize VertexBytes() const;
    VkDeviceSize IndexBytes() const { return _indices.size; }
    VkDeviceSize VolumeBytes() const { return _poolGrids.empty() ? 0 : _volumeGrids.size; }  // NanoVDB grids

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
    int32_t _TextureIndex(Texture *t, uint32_t wrap = 0);
    void _UpdateEnvironment(const Dome *dome);
    struct Emitter;
    std::vector<Emitter> _FindEmitters();
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
    // (texture, wrap modes) -> bindless index, rebuilt per Commit.
    std::map<std::pair<const Texture *, uint32_t>, int32_t> _textureSlots;

    // Environment built from this texture (kept alive so the pointer stays unique).
    std::shared_ptr<Texture> _envSource;
    bool _envBuilt = false;
    uint32_t _tagMask = ~0u;
    bool _emissiveLights = true;
    float _emissiveThreshold = 4.0f;
    bool _changed = true;           // since the last Commit: a mesh was added or removed
    std::vector<uint32_t> _lastDrawn;  // per slot: instances shadows drew last Commit
    VkImage _envImage = VK_NULL_HANDLE;
    VkDeviceMemory _envMemory = VK_NULL_HANDLE;
    VkImageView _envView = VK_NULL_HANDLE;

    // Volumes (volume.cpp): the grids in the pool, at the version uploaded.
    void _CommitVolumes();
    std::vector<std::unique_ptr<Volume>> _volumes;
    Buffer _volumeTable, _volumeGrids;
    std::vector<std::pair<std::shared_ptr<VolumeGrid>, uint64_t>> _poolGrids;

    Gpu _gpu;
    Buffer _displacements;  // Gpu::displacements
    Buffer _lodTable;       // Gpu::lods
    void _StoreLods(Mesh &mesh, std::vector<LodLevel> levels);
    void _ReserveLodInstances(Mesh &mesh);
    // Levels of detail building in the background, for a mesh's upload
    // (generation); mesh null once it was removed.
    struct PendingLods {
        Mesh *mesh;
        uint32_t generation;
        std::future<std::vector<LodLevel>> levels;
    };
    std::vector<PendingLods> _pendingLods;
};

} // namespace vkr
