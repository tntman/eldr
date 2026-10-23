#pragma once

#include "device.h"
#include "gtao.h"
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
    VkPipeline _prepassPipeline = VK_NULL_HANDLE;  // depth only
    // Cutout materials: prepass and shadows with mask.frag's alpha test.
    VkPipeline _prepassCutoutPipeline = VK_NULL_HANDLE, _shadowCutoutPipeline = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;         // shading, depth read-only
    VkPipeline _shadowPipeline = VK_NULL_HANDLE;
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
