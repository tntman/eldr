#pragma once

#include "aov.h"
#include "device.h"
#include "distortion.h"
#include "fog.h"
#include "gtao.h"
#include "lens.h"
#include "post.h"
#include "raytracing.h"
#include "scene.h"
#include "subsurface.h"
#include "taa.h"
#include "transparency.h"
#include "volume.h"

#include <chrono>
#include <memory>

namespace vkr {

struct FrameData;
class Reflections;

class Renderer {
public:
    struct Frame {
        uint32_t width = 0;
        uint32_t height = 0;
        float viewProj[16];       // USD row-vector order, GL clip space
        float invViewProj[16];
        float cameraPos[3];       // world space
        float clearColor[4] = {0, 0, 0, 0};
        float clearDepth = 1.0f;
        bool showEnvironment = true;  // draw the dome texture as background
        bool postProcess = true;      // false: color is the raw linear HDR scene
        bool taa = true;              // with postProcess: jittered + temporally resolved
        bool ambientOcclusion = true; // GTAO on the dome (image-based) lighting
        float aoRadius = 0.5f;        // world units
        bool ssr = true;              // screen-space reflections (needs postProcess)
        bool shadows = true;          // the distant light's shadow cascades
        bool localShadows = true;     // local lights' shadow atlas
        bool softShadows = true;      // local lights' penumbrae from their size (PCSS); off: 3x3 PCF
        uint32_t taaSamples = 16;     // jitter positions a still camera accumulates
        float cullPixels = 0.5f;      // instances under this screen radius are skipped (0: none)
        // Always worth having, but each costs time: off to compare or to
        // speed up a heavy scene.
        bool depthPrepass = true;        // off: the main pass writes depth (no GTAO, no SSR)
        bool frustumCulling = true;      // off: every instance is drawn (small-object culling too)
        bool localLights = true;         // sphere, rect, disk and cylinder lights
        bool clusteredLights = true;     // off: every pixel loops over all local lights
        bool imageBasedLighting = true;  // the dome's diffuse and specular light
        bool normalMaps = true;
        bool materialGraphs = true;      // material programs (MaterialX graphs); off: plain inputs
        bool objectMotion = true;        // moving objects' motion vectors (TAA, motion blur); off: camera only
        bool translucency = true;        // off: translucent objects aren't drawn
        // Quality against speed.
        uint32_t shadowResolution = 2048;      // sun cascades, texels square (256..8192)
        uint32_t shadowCascades = kCascades;   // 1..kCascades
        uint32_t localShadowResolution = 512;  // a local light's atlas tile (64..kShadowAtlasSize / 2)
        uint32_t softShadowSamples = 16;       // PCSS blocker search and filter taps (1..16)
        uint32_t aoSlices = 3;                 // GTAO directions per pixel (1..8)
        uint32_t ssrSteps = 32;                // reflection ray march steps (1..256)
        uint32_t anisotropy = 16;              // material texture filtering (1 = off .. 16)
        // The camera's lens (UsdGeomCamera, world units), with postProcess:
        // depth of field when fStop > 0, motion blur while the shutter is open.
        bool depthOfField = true;
        bool motionBlur = true;
        float focalLength = 0.0f;
        float fStop = 0.0f;
        float focusDistance = 0.0f;
        float sensorWidth = 0.0f;     // horizontal aperture the projection shows
        float shutter = 0.0f;         // open interval in frames (shutter:close - shutter:open)
        // Inverse projection's z and w columns over NDC z (row-vector order):
        // camera distance = -(x * z + y) / (z * z + w), z = window depth * 2 - 1.
        float depthToDistance[4] = {0, 0, 0, 0};
        PostProcess::Settings post;
        // Procedural textures: MaterialX noise in material programs (off: it reads 0).
        bool proceduralTextures = true;
        // Lens distortion: the camera's lensDistortion:* warp the finished
        // frame, ids and depth included (distortion.h); the caller renders
        // the view distortion.overscan times wider.
        bool lensDistortion = true;
        LensDistortion::Params distortion;
        // Blend shape motion: skinned meshes' motion vectors (TAA, motion
        // blur) include their blend shapes' change since the last frame.
        bool blendShapeMotion = true;
        // Extra AOVs (Targets::aovs): world to camera (Neye, Peye,
        // cameraDepth; row-vector order) and per primId its Cryptomatte
        // object and material ids (CryptomatteHash), 2 per id.
        bool extraAovs = true;
        float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const uint32_t *cryptomatte = nullptr;
        uint32_t cryptomatteIds = 0;
        // GPU sharing (the Hydra layer's HdVkGLInterop, interop.h): colour
        // and depth go to the app's OpenGL on the GPU instead of Targets.
        bool shareFrame = true;
        // Material lobes beyond the metallic base (shaders/brdf.glsl); off:
        // materials render without them.
        bool specularWorkflow = true;  // UsdPreviewSurface specularColor as F0
        bool clearcoat = true;         // a second GGX lobe on top (coat)
        bool sheen = true;             // microfiber lobe (fabric, fuzz)
        bool thinFilm = true;          // thin-film iridescence on the specular
        bool anisotropicSpecular = true;  // highlights stretched along a tangent (brushed metal)
        bool toon = true;              // NPR toon bands of materials with their own (Material::toonBands)
        uint32_t toonBands = 0;        // with toon: bands for every other material too (0: none)
        bool hairShading = true;       // curves (Mesh::hair) with strand highlights
        // Ray tracing (raytracing.cpp), ignored without Device::rayTracing.
        // With the depth prepass: traced shadows of the sun and the three
        // lights that matter most (no shadow maps for them; soft from the
        // sun's angle and the lights' size) and traced reflections (instead
        // of the dome's and SSR, up to roughness 0.7), both noisy per frame
        // for TAA to average. Translucent meshes don't block rays; of the
        // skinned ones, the rtSkinnedInstances largest on screen do (posed
        // and refit every frame).
        bool rtShadows = false;
        uint32_t rtShadowSamples = 4;      // rays per light and pixel (1..16)
        bool rtReflections = false;
        uint32_t rtReflectionSamples = 1;  // rays per pixel (1..16)
        uint32_t rtSkinnedInstances = 16;  // skinned instances that block rays (0..kMaxRtSkinned), for every ray-traced effect
        // Global illumination (ray-traced DDGI probes; ignored without
        // Device::rayTracing): the diffuse light bounced off the scene,
        // instead of the dome's unoccluded light.
        bool gi = false;
        uint32_t giProbes = 16;      // along the scene's longest side (2..64; at most 16384 in all)
        uint32_t giRays = 128;       // per probe and frame (16..512)
        float giHysteresis = 0.97f;  // weight of the probes' history (0..0.99): smoother, but slower to react
        // Screen-space GI (gtao.cpp; no ray tracing needed), with the depth
        // prepass: per-pixel indirect diffuse light from what's on screen,
        // instead of GTAO; the probes (gi) or the dome light the rest.
        bool ssgi = false;
        uint32_t ssgiSteps = 8;        // per direction and side (1..32)
        float ssgiRadius = 1.0f;       // world units
        uint32_t ssgiResolution = 2;   // traced at 1 / this (1, 2), denoised and upsampled
        // Emissive surfaces as lights: small meshes emitting at least the
        // threshold also light the scene as sphere lights (with shadows);
        // the caller applies these to the Scene (Scene::SetEmissiveLights).
        // GI picks up every emissive surface's light regardless.
        bool emissiveLights = true;
        float emissiveLightThreshold = 4.0f;  // scene-referred radiance of the brightest channel
        // Translucency (transparency.h): weighted blended OIT, its weights
        // falling with distance to this power (0: a plain average); off:
        // sorted per mesh, blended over.
        bool oit = true;
        float oitDepthWeight = 4.0f;
        // Refraction (needs depthPrepass): translucent materials with a
        // transmission or an ior bend and tint the opaque colour behind
        // them, rough ones blurred over this many taps (1..16).
        bool refraction = true;
        uint32_t refractionSamples = 8;
        // Translucent shadows: translucent casters tint and lighten the
        // sun's and local lights' shadows, through maps of 1 / this the
        // shadow maps' resolution (1..4).
        bool translucentShadows = true;
        uint32_t translucentShadowDivisor = 2;
        // Subsurface scattering (needs depthPrepass): the diffuse light of
        // subsurface materials blurred over this many taps per direction
        // (1..64), and the sun shining through their thin parts.
        bool subsurface = true;
        uint32_t subsurfaceSamples = 15;
        // Holdouts (Mesh::holdout): black with zero alpha; off: drawn as
        // usual.
        bool holdouts = true;
        // Soft sun shadows: penumbrae from the distant light's angle (PCSS
        // on the cascades), with softShadows on.
        bool softSunShadows = true;
        // Volumetric fog (fog.h): height fog lit by every light, over every
        // surface and the sky. Units are the scene's.
        bool fog = false;
        float fogDensity = 0.02f;       // extinction per unit at fogHeight
        float fogHeight = 0.0f;         // base height, along Y (or Z with fogZUp)
        float fogHeightFalloff = 0.0f;  // density * exp(-falloff * (height - fogHeight)); 0: uniform
        float fogAnisotropy = 0.6f;     // Henyey-Greenstein g: 0 even, towards 1 forward (shafts towards lights)
        float fogColor[3] = {1, 1, 1};  // scattering albedo
        float fogMaxDistance = 0.0f;    // view depth the froxels reach (then constant); 0: the scene's far end
        uint32_t fogGridSize = 160;     // froxels across (16..512); rows by aspect, 64 slices
        bool fogZUp = false;            // heights along Z (Z-up stages)
        // Reflection probe and planar reflections (probes.h).
        bool reflectionProbe = false;    // one cube captured in the scene, before the dome (with imageBasedLighting)
        uint32_t probeResolution = 128;  // its faces' texels (16..512, a power of two)
        float probePosition[4] = {0, 0, 0, 0};  // where it is captured if w = 1, else the scene's centre
        bool planarReflections = false;  // the mirror mesh reflects the scene rendered mirrored
        float planarScale = 0.5f;        // that render's size, of the frame's (0.1..1)
        bool authoredProbes = true;     // meshes flagged primvars:hdvk:reflectionProbe are the probes (probes.h)
        bool skinning = true;            // false: skinned meshes keep the last frame's joints (reflection renders)
        // OpenVDB volumes, raymarched over the shaded image (VolumePass).
        bool volumes = true;
        VolumePass::Settings volume;
        // Displacement (Material::displacement): off draws the undisplaced
        // surfaces; the scale multiplies every material's offset.
        bool displacement = true;
        float displacementScale = 1.0f;
        // Levels of detail (Scene builds them for heavy meshes): off draws
        // every mesh whole; a bias of 1 allows twice the screen error.
        bool lod = true;
        float lodBias = 0.0f;
    };

    // Host memory a frame is copied to, bottom row first (GL/Hydra order).
    // Null pointers skip that output's readback.
    struct Targets {
        void *color = nullptr;          // RGBA16F, 8 bytes per pixel
        float *depth = nullptr;         // window depth in [0, 1]
        int32_t *primId = nullptr;      // Mesh::primId, -1 where nothing was drawn
        int32_t *instanceId = nullptr;  // index into Mesh::transforms, -1 likewise
        uint16_t *velocity = nullptr;   // object motion vectors, two halves per pixel (see mesh.frag)
        // Extra AOVs (aov.h), packed floats; with Frame::extraAovs.
        float *aovs[Aovs::kCount] = {};
        uint8_t aovComponents[Aovs::kCount] = {};  // floats per pixel of each (0: Aovs::Components)
    };

    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kIdFormat = VK_FORMAT_R32_SINT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
    static constexpr uint32_t kCascades = 4;
    static constexpr uint32_t kMaxRtSkinned = 256;  // Frame::rtSkinnedInstances
    // Shadow atlas for sphere/rect/disk lights, in square tiles
    // (Frame::localShadowResolution).
    static constexpr uint32_t kShadowAtlasSize = 4096;

    explicit Renderer(Device &device);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // Draws `scene` and blocks until the requested outputs are in `out`.
    void Render(const Scene &scene, const Frame &frame, const Targets &out);

    // False while auto-exposure adapts or TAA is still accumulating a still
    // camera: keep rendering frames.
    bool IsConverged() const;

    // Instances that survived culling in the last frame (main view).
    uint32_t VisibleInstances() const;

    // What the last Render measured.
    struct Stats {
        double cpuMs = 0;  // Render's wall time: recording, the GPU's work, readback
        double gpuMs = 0;  // the frame on the GPU; 0 without timestamps
        // GPU milliseconds per pass, in the order they ran (see Render).
        std::vector<std::pair<const char *, double>> passMs;
        uint64_t triangles = 0;   // the camera drew, after culling
        uint32_t instances = 0;   // the camera drew, after culling
        uint32_t drawCalls = 0;   // the camera's indirect calls (each covers many meshes)
        uint32_t taaSamples = 0;  // frames in the TAA average (taaTarget: converged)
        uint32_t taaTarget = 0;
        double exposure = 1;      // the tonemap's multiplier of scene radiance
        uint64_t accelerationStructureBytes = 0;  // ray tracing's graphics memory (in Device's too)
    };
    const Stats &GetStats() const { return _stats; }

    // GPU sharing (interop.h): the last frame's images, in
    // TRANSFER_SRC_OPTIMAL until the next Render: colour as Targets::color
    // has it (RGBA16F) and depth (D32); null before a frame.
    VkImage ColorImage() const { return _frameColor; }
    VkImage DepthImage() const { return _frameColor ? _depth.image : VK_NULL_HANDLE; }
    uint32_t Width() const { return _width; }
    uint32_t Height() const { return _height; }
    // Copies the last frame's outputs (as Render's, extra AOVs aside) to
    // host memory, for consumers that ask after Render.
    void Read(const Targets &out);
    // The last frame's colour target: raw HDR, premultiplied (with
    // Frame::postProcess off), in TRANSFER_SRC_OPTIMAL. Planar reflections
    // sample another renderer's.
    VkImage HdrColorImage() const { return _color.image; }
    VkImageView HdrColorView() const { return _color.view; }

private:
    void _CreateDescriptors();
    void _CreatePipelines(bool displaced = false, uint32_t set = kNone);
    uint32_t _ShadingSet(uint32_t features);
    void _CreateConstantImages();
    void _Resize(uint32_t width, uint32_t height);
    void _CreateShadowMap(uint32_t size);
    void _UpdateDescriptors(const Scene::Gpu &gpu, bool prepass, VkImageView background);
    bool _FitCascades(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const;
    void _FitClusters(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const;
    bool _SetupFog(const Scene::Gpu &gpu, const Frame &frame, FrameData *data);
    struct ShadowTile;
    std::vector<ShadowTile> _PlanLocalShadows(const Scene::Gpu &gpu, const Frame &frame, float cutoff,
                                              uint32_t *rtLights, uint32_t rtSlots);
    void _RenderLocalShadows(const Scene::Gpu &gpu, const std::vector<ShadowTile> &tiles, bool tint);
    void _DrawTintCasters(const Scene::Gpu &gpu, VkDeviceSize commands);
    void _RenderSubsurface(const Scene::Gpu &gpu, const Frame &frame, const float *viewProj,
                           const float *invViewProj, VkRenderingAttachmentInfo depth);
    void _RenderTranslucency(const Scene::Gpu &gpu, const Frame &frame, FrameData *data, const float *viewProj,
                             uint32_t motion, VkRenderingAttachmentInfo depth);

    Device &_dev;
    Gtao _gtao;
    TemporalAA _taa;
    Lens _lens;
    PostProcess _post;
    Transparency _transparency;
    Subsurface _subsurface;
    Fog _fog;
    float _fogDepth = 0;  // last frame's froxel depth (Frame::fogMaxDistance or the scene's)
    uint64_t _fogKey = 0; // last frame's lighting and medium (history is kept while they stay)
    std::unique_ptr<Reflections> _reflections;  // made when a frame first asks for them
    // TAA state: frames in the history, jitter sequence, last camera.
    uint32_t _taaFrames = 0, _haltonIndex = 0;
    float _prevViewProj[16] = {};  // camera of the frame in the TAA history
    bool _taaValid = false;
    bool _historyValid = false;    // the TAA history holds last frame's image
    bool _taaWarmup = false;  // accumulating a fresh history for a still camera
    bool _taaConverged = true;
    std::chrono::steady_clock::time_point _lastFrame;
    bool _hasLastFrame = false;
    bool _adapting = false;       // last frame ran auto-exposure
    bool _lastAutoExposure = false;
    VkSampler _samplers[kWrapModes * kWrapModes] = {};  // materials: trilinear, aniso, per wrap s and t
    VkSampler _clampSampler = VK_NULL_HANDLE;   // LUT, environment cube
    VkSampler _shadowSampler = VK_NULL_HANDLE;  // depth compare
    VkSampler _minSampler = VK_NULL_HANDLE;     // minimum of the bilinear footprint (Device::samplerMinmax)
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    // Mesh pipelines in variants (Scene::Gpu::chunkKind >> 2): bit 0 eight
    // skinning influences, bit 1 dual-quaternion skinning (mesh.vert's
    // kWideSkin, kDualQuaternion); the shading ones also bit 2, a material
    // program (the GRAPH shaders), and bit 3 with it, one with noise
    // (program.glsl's kProcedurals); bit 4, lobes (the LAYERS shaders); bit
    // 5 displaced materials (every pass: displace.glsl's kDisplacement),
    // made once a scene has some (_displacedPipelines). Depth passes use the
    // kDepthVariants bits.
    static constexpr uint32_t kVariants = 64, kDepthVariants = 1 | 2 | 32;
    // The shading pipelines come in sets, each specialized with the optional
    // features (frame.glsl's kCompiledFeatures) _shadingSets has for it, its
    // variants at set * kVariants + variant; a frame draws with the set of
    // its own features (_shadingBase), made when a frame first needs it.
    static constexpr uint32_t kShadingSets = 8;
    std::vector<uint32_t> _shadingSets;
    uint32_t _shadingBase = 0;
    VkPipeline _prepassPipeline[kVariants] = {};      // depth only
    VkPipeline _translucentPipeline[kShadingSets * kVariants] = {};  // shading, blended over the opaque
    VkPipeline _oitPipeline[kShadingSets * kVariants] = {};  // shading, OIT accumulation (Transparency)
    // Translucent shadow casters into Transparency's tint maps: their
    // nearest depth, then their colours multiplied (shadow_tint.frag).
    VkPipeline _tintDepthPipeline[kVariants] = {}, _tintColorPipeline[kVariants] = {};
    VkPipeline _subsurfacePipeline[kShadingSets * kVariants] = {};  // shading, the subsurface lobe (Subsurface)
    // Cutout materials: prepass and shadows with mask.frag's alpha test.
    VkPipeline _prepassCutoutPipeline[kVariants] = {}, _shadowCutoutPipeline[kVariants] = {};
    VkPipeline _pipeline[kShadingSets * kVariants] = {};  // shading, depth read-only
    VkPipeline _shadowPipeline[kVariants] = {};
    bool _displacedPipelines = false;
    VkPipeline _skyPipeline = VK_NULL_HANDLE;
    VkPipeline _cullPipeline = VK_NULL_HANDLE;
    VkPipeline _lightsPipeline = VK_NULL_HANDLE;    // light cluster assignment
    VkPipeline _skinPipeline = VK_NULL_HANDLE;      // joint matrices from skinning transforms
    Buffer _clusters;                               // light lists, see shaders/lights.glsl
    Buffer _commands;               // culled indirect commands, one per mesh slot
    // Last frame's joint matrices for motion vectors, copied before skin.comp
    // rewrites them; usable while the joint pool is the same buffer.
    Buffer _prevJoints;
    Buffer _lodState;  // the camera's level of detail per instance slot (cull.comp)
    float _lodViewProj[16] = {};  // the camera the atlas last drew levels of detail for
    uint32_t _lodFrame = 0;       // frames with levels of detail (cull.comp's lodState)
    VkBuffer _prevJointsSource = VK_NULL_HANDLE;
    uint32_t _lastDrawCount = 0;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
    VkFence _fence = VK_NULL_HANDLE;
    // GPU timestamps: one at the start, then one closing each pass.
    static constexpr uint32_t kMaxTimestamps = 32;
    VkQueryPool _timestamps = VK_NULL_HANDLE;  // null: the queue has no timestamps
    double _timestampMs = 0;                   // milliseconds per tick
    std::vector<const char *> _passes;         // the pass each timestamp after the first closes
    Stats _stats;
    Image _color, _primId, _instanceId, _depth;
    Image _velocity;  // motion vectors (RG16F, uv), read by TAA
    Image _shadowMap;                           // kCascades-layer array view
    // The cascades' nearest depths for soft sun shadows (shadow_min.comp):
    // level 0 per 4 x 4 texels, mips to 1 x 1, in GENERAL; a view per level.
    static constexpr uint32_t kMaxShadowMinLevels = 12;
    Image _shadowMin;
    std::vector<VkImageView> _shadowMinLevels;
    VkPipeline _shadowMinPipeline = VK_NULL_HANDLE;
    uint32_t _shadowSize = 0;                   // its width and height (Frame::shadowResolution)
    float _anisotropy = 0;                      // _samplers' (Frame::anisotropy)
    Image _shadowAtlas;                         // local lights, see _PlanLocalShadows
    Buffer _lightShadows, _shadowTiles;         // per light: first tile or kNone; per tile: ShadowTile
    // What the atlas holds: redrawn only when the scene or the tiles change.
    uint64_t _atlasVersion = 0;
    std::vector<float> _atlasKey;               // the tiles' matrices and rects
    VkImageView _shadowLayers[kCascades] = {};  // per-cascade render targets
    Image _brdfLut;
    Image _blackCube;                           // bound when there is no dome
    Buffer _readback, _frameData;
    uint32_t _width = 0, _height = 0;
    LensDistortion _distortion{_dev};
    // Blend shape motion: last frame's blend shape weights, copied after
    // each frame's draws from the scene's pool (_prevBlendSource), which
    // the next frame's motion vectors read while it is the same buffer.
    // ponytail: a mesh whose weights moved within the pool (its shape count
    // changed) reads another mesh's for one frame.
    Buffer _prevBlendWeights;
    VkBuffer _prevBlendSource = VK_NULL_HANDLE;
    bool _prevBlendValid = false;
    Aovs _aovs{_dev};  // extra AOVs
    VkImage _frameColor = VK_NULL_HANDLE;  // GPU sharing: ColorImage()
    std::unique_ptr<RayTracing> _rt;  // null without Device::rayTracing
    std::unique_ptr<VolumePass> _volumePass;  // needs _setLayout
};

} // namespace vkr
