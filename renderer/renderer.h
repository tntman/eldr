#pragma once

#include "device.h"
#include "gtao.h"
#include "lens.h"
#include "post.h"
#include "scene.h"
#include "taa.h"

#include <chrono>

namespace vkr {

struct FrameData;

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
        bool localShadows = true;     // sphere/rect/disk lights' shadow atlas
        bool softShadows = true;      // local lights' penumbrae from their size (PCSS); off: 3x3 PCF
        uint32_t taaSamples = 16;     // jitter positions a still camera accumulates
        float cullPixels = 0.5f;      // instances under this screen radius are skipped (0: none)
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
    };

    // Host memory a frame is copied to, bottom row first (GL/Hydra order).
    // Null pointers skip that output's readback.
    struct Targets {
        void *color = nullptr;          // RGBA16F, 8 bytes per pixel
        float *depth = nullptr;         // window depth in [0, 1]
        int32_t *primId = nullptr;      // Mesh::primId, -1 where nothing was drawn
        int32_t *instanceId = nullptr;  // index into Mesh::transforms, -1 likewise
        uint16_t *velocity = nullptr;   // object motion vectors, two halves per pixel (see mesh.frag)
    };

    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kIdFormat = VK_FORMAT_R32_SINT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
    static constexpr uint32_t kCascades = 4;
    static constexpr uint32_t kShadowSize = 2048;
    // Shadow atlas for sphere/rect/disk lights, in square tiles.
    static constexpr uint32_t kShadowAtlasSize = 4096, kShadowTileSize = 512;

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
    };
    const Stats &GetStats() const { return _stats; }

private:
    void _CreateDescriptors();
    void _CreatePipelines();
    void _CreateConstantImages();
    void _Resize(uint32_t width, uint32_t height);
    void _UpdateDescriptors(const Scene::Gpu &gpu);
    bool _FitCascades(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const;
    void _FitClusters(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const;
    struct ShadowTile;
    std::vector<ShadowTile> _PlanLocalShadows(const Scene::Gpu &gpu, const Frame &frame, float cutoff);
    void _RenderLocalShadows(const Scene::Gpu &gpu, const std::vector<ShadowTile> &tiles);

    Device &_dev;
    Gtao _gtao;
    TemporalAA _taa;
    Lens _lens;
    PostProcess _post;
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
    VkSampler _sampler = VK_NULL_HANDLE;        // materials: trilinear, aniso, repeat
    VkSampler _clampSampler = VK_NULL_HANDLE;   // LUT, environment cube
    VkSampler _shadowSampler = VK_NULL_HANDLE;  // depth compare
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    // Mesh pipelines in variants (Scene::Gpu::chunkKind >> 2): bit 0 eight
    // skinning influences, bit 1 dual-quaternion skinning (mesh.vert's
    // kWideSkin, kDualQuaternion); the shading ones also bit 2, a material
    // program (the GRAPH shaders).
    VkPipeline _prepassPipeline[4] = {};      // depth only
    VkPipeline _translucentPipeline[8] = {};  // shading, blended over the opaque
    // Cutout materials: prepass and shadows with mask.frag's alpha test.
    VkPipeline _prepassCutoutPipeline[4] = {}, _shadowCutoutPipeline[4] = {};
    VkPipeline _pipeline[8] = {};             // shading, depth read-only
    VkPipeline _shadowPipeline[4] = {};
    VkPipeline _skyPipeline = VK_NULL_HANDLE;
    VkPipeline _cullPipeline = VK_NULL_HANDLE;
    VkPipeline _lightsPipeline = VK_NULL_HANDLE;    // light cluster assignment
    VkPipeline _skinPipeline = VK_NULL_HANDLE;      // joint matrices from skinning transforms
    Buffer _clusters;                               // light lists, see shaders/lights.glsl
    Buffer _commands;               // culled indirect commands, one per mesh slot
    // Last frame's joint matrices for motion vectors, copied before skin.comp
    // rewrites them; usable while the joint pool is the same buffer.
    Buffer _prevJoints;
    VkBuffer _prevJointsSource = VK_NULL_HANDLE;
    uint32_t _lastDrawCount = 0;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
    VkFence _fence = VK_NULL_HANDLE;
    // GPU timestamps: one at the start, then one closing each pass.
    static constexpr uint32_t kMaxTimestamps = 16;
    VkQueryPool _timestamps = VK_NULL_HANDLE;  // null: the queue has no timestamps
    double _timestampMs = 0;                   // milliseconds per tick
    std::vector<const char *> _passes;         // the pass each timestamp after the first closes
    Stats _stats;
    Image _color, _primId, _instanceId, _depth;
    Image _velocity;  // motion vectors (RG16F, uv), read by TAA
    Image _shadowMap;                           // kCascades-layer array view
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
};

} // namespace vkr
