#pragma once

#include "device.h"

#include <vector>

namespace vkr {

// HDR scene -> display: histogram auto-exposure with temporal adaptation,
// bloom (13-tap down / tent up mip chain) and ACES tonemapping.
class PostProcess {
public:
    struct Settings {
        bool autoExposure = true;
        float exposureCompensation = 0.0f;  // EV, also the fixed exposure without auto
        float bloomIntensity = 0.04f;       // 0 disables bloom
    };

    explicit PostProcess(Device &device);
    ~PostProcess();
    PostProcess(const PostProcess &) = delete;
    PostProcess &operator=(const PostProcess &) = delete;

    // (Re)creates size-dependent targets; `hdr` and `depth` must stay valid
    // until the next Resize.
    void Resize(uint32_t width, uint32_t height, VkImageView hdr, VkImageView depth);

    // Records every pass. Expects hdr and depth in SHADER_READ_ONLY_OPTIMAL
    // and leaves Output() in TRANSFER_SRC_OPTIMAL. `skyBackground`: pixels
    // without geometry show the dome, so they get exposed and tonemapped too.
    void Record(VkCommandBuffer cmd, const Settings &settings, bool skyBackground, float dt,
                bool resetExposure);

    VkImage Output() const { return _output.image; }

    // After the frame's fence: true once the adapted exposure reached its target.
    bool ExposureConverged() const;

private:
    VkDescriptorSet _CreateSet(VkImageView src);
    void _Fullscreen(VkCommandBuffer cmd, VkPipeline pipeline, VkDescriptorSet set,
                     VkImageView target, uint32_t width, uint32_t height, bool load,
                     const float params[8]);
    void _DestroyTargets();

    Device &_dev;
    VkSampler _linear = VK_NULL_HANDLE, _nearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;  // recreated by Resize
    VkPipeline _histogram = VK_NULL_HANDLE, _exposure = VK_NULL_HANDLE;
    VkPipeline _down = VK_NULL_HANDLE, _up = VK_NULL_HANDLE, _tonemap = VK_NULL_HANDLE;
    Buffer _histogramBuffer, _exposureBuffer;  // exposure state persists across frames

    uint32_t _width = 0, _height = 0;
    VkImageView _hdr = VK_NULL_HANDLE, _depth = VK_NULL_HANDLE;
    Image _bloom;                         // half resolution, one mip per bloom level
    std::vector<VkImageView> _bloomMips;  // single-mip views
    Image _output;
    VkDescriptorSet _hdrSet = VK_NULL_HANDLE;  // src = HDR scene
    std::vector<VkDescriptorSet> _mipSets;     // src = bloom mip i
};

} // namespace vkr
