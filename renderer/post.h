#pragma once

#include "device.h"

#include <vector>

namespace vkr {

// HDR scene -> display: histogram auto-exposure with temporal adaptation,
// bloom (13-tap down / tent up mip chain) and tonemapping (ACES, AgX or none).
class PostProcess {
public:
    struct Settings {
        bool autoExposure = true;
        float exposureCompensation = 0.0f;  // EV, also the fixed exposure without auto
        float bloomIntensity = 0.04f;       // 0 disables bloom
        // AgX tonemapping: the curve from scene to display (shaders/tonemap.frag).
        enum class Tonemapper { Aces, Agx, AgxPunchy, None };  // None: exposed linear, unclamped
        Tonemapper tonemapper = Tonemapper::Aces;
    };

    explicit PostProcess(Device &device);
    ~PostProcess();
    PostProcess(const PostProcess &) = delete;
    PostProcess &operator=(const PostProcess &) = delete;

    // (Re)creates size-dependent targets; `hdr` must stay valid until the
    // next Resize. It holds the scene premultiplied by coverage (alpha 0
    // where only the app's background shows).
    void Resize(uint32_t width, uint32_t height, VkImageView hdr);

    // Records every pass. Expects hdr in SHADER_READ_ONLY_OPTIMAL and leaves
    // Output() in TRANSFER_SRC_OPTIMAL. The tonemapped scene is composited
    // over `background` (display-referred) by coverage.
    // `linear`: also writes LinearOutput(), the same image exposed but
    // neither bloomed nor tonemapped (an EXR's beauty).
    void Record(VkCommandBuffer cmd, const Settings &settings, float dt, bool resetExposure,
                const float background[4], bool linear = false);

    VkImage Output() const { return _output.image; }
    VkImageView OutputView() const { return _output.view; }
    // In TRANSFER_SRC_OPTIMAL after Record(.., linear).
    VkImage LinearOutput() const { return _exr.image; }

    // After the frame's fence: true once the adapted exposure reached its target.
    bool ExposureConverged() const;
    // Multiplier the last frame's tonemap applied to scene radiance.
    float Exposure() const;

private:
    VkDescriptorSet _CreateSet(VkImageView src);
    void _DestroyTargets();

    Device &_dev;
    VkSampler _linear = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;  // recreated by Resize
    VkPipeline _histogram = VK_NULL_HANDLE, _exposure = VK_NULL_HANDLE;
    VkPipeline _down = VK_NULL_HANDLE, _up = VK_NULL_HANDLE, _tonemap = VK_NULL_HANDLE;
    Buffer _histogramBuffer, _exposureBuffer;  // exposure state persists across frames

    uint32_t _width = 0, _height = 0;
    VkImageView _hdr = VK_NULL_HANDLE;
    Image _bloom;                         // half resolution, one mip per bloom level
    std::vector<VkImageView> _bloomMips;  // single-mip views
    Image _output;
    Image _exr;  // made when first asked for
    VkDescriptorSet _hdrSet = VK_NULL_HANDLE;  // src = HDR scene
    std::vector<VkDescriptorSet> _mipSets;     // src = bloom mip i
};

} // namespace vkr
