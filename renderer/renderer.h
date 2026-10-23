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
        PostProcess::Settings post;
    };

    // Host memory a frame is copied to, bottom row first (GL/Hydra order).
    // Null pointers skip that output's readback.
    struct Targets {
        void *color = nullptr;          // RGBA16F, 8 bytes per pixel
        float *depth = nullptr;         // window depth in [0, 1]
        int32_t *primId = nullptr;      // Mesh::primId, -1 where nothing was drawn
        int32_t *instanceId = nullptr;  // index into Mesh::transforms, -1 likewise
    };

    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kIdFormat = VK_FORMAT_R32_SINT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr uint32_t kCascades = 4;
    static constexpr uint32_t kShadowSize = 2048;

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

    Device &_dev;
    Gtao _gtao;
    TemporalAA _taa;
    PostProcess _post;
    // TAA state: frames in the history, jitter sequence, last camera.
    uint32_t _taaFrames = 0, _haltonIndex = 0;
    float _prevViewProj[16] = {};
    bool _taaValid = false;
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
    VkPipeline _pipeline = VK_NULL_HANDLE;         // shading, depth read-only
    VkPipeline _shadowPipeline = VK_NULL_HANDLE;
    VkPipeline _skyPipeline = VK_NULL_HANDLE;
    VkPipeline _cullPipeline = VK_NULL_HANDLE;
    Buffer _commands;               // culled indirect commands, one per mesh slot
    uint32_t _lastDrawCount = 0;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
    VkFence _fence = VK_NULL_HANDLE;
    Image _color, _primId, _instanceId, _depth;
    Image _shadowMap;                           // kCascades-layer array view
    VkImageView _shadowLayers[kCascades] = {};  // per-cascade render targets
    Image _brdfLut;
    Image _blackCube;                           // bound when there is no dome
    Buffer _readback, _frameData;
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
