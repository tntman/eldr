#pragma once

#include "device.h"

namespace vkr {

// The camera's lens: depth of field (a thin lens, before TAA, which
// averages its per-frame turned taps into a smooth blur) and motion blur
// (each pixel's motion over the shutter, after TAA). Both spread past an
// object's own pixels through 32x32 tiles of the longest motion and the
// largest near-field circle of confusion (McGuire 2012).
class Lens {
public:
    struct Params {  // mirror of the push block in lens.glsl
        float reprojection[16];    // NDC -> last frame's clip space (row-vector order)
        float depthToDistance[4];  // see lens.glsl
        // Circle of confusion radius in pixels = cocScale * |distance -
        // focusDistance| / distance, at most maxRadius (cocScale 0: none).
        float cocScale, focusDistance, maxRadius, frame;
        // Shutter in frames (0: no motion blur), longest half blur in
        // pixels; 1/width, 1/height (filled in by Lens).
        float shutter, maxHalfBlur, texel[2];
        // Residual motion blur: the sample's stratum, from window[0] to
        // window[1] strata around its time (zw unused).
        float window[4];
    };

    explicit Lens(Device &device);
    ~Lens();
    Lens(const Lens &) = delete;
    Lens &operator=(const Lens &) = delete;

    // `color` is the HDR target depth of field rewrites; `resolved` is
    // TAA's output, rewritten by motion blur from `history` (TAA's copy of
    // it). All must stay valid until the next Resize.
    // width x height: the scene's targets; outWidth x outHeight: TAA's
    // resolve (larger while upscaling), where motion blur draws.
    void Resize(uint32_t width, uint32_t height, uint32_t outWidth, uint32_t outHeight, VkImage color,
                VkImageView colorView, VkImageView depth, VkImageView velocity, VkImage resolved,
                VkImageView resolvedView, VkImageView history);

    // Each frame with either effect, first: the tiles (depth read-only,
    // motion vectors SHADER_READ_ONLY_OPTIMAL).
    void RecordTiles(VkCommandBuffer cmd, Params params);
    // Blurs `color` (COLOR_ATTACHMENT_OPTIMAL, left so) by each pixel's
    // circle of confusion.
    void RecordDepthOfField(VkCommandBuffer cmd, Params params);
    // Spreads `color` (as RecordDepthOfField) over the time sample's
    // stratum of the shutter: params.shutter is a stratum's (the residual
    // blur between motion time samples, motion_residual.frag).
    void RecordResidualMotionBlur(VkCommandBuffer cmd, Params params);
    // Blurs TAA's resolve (SHADER_READ_ONLY_OPTIMAL, left so) along each
    // pixel's motion over the shutter.
    void RecordMotionBlur(VkCommandBuffer cmd, Params params);

private:
    void _Destroy();
    void _Fill(Params *params) const;
    void _RecordOnColor(VkCommandBuffer cmd, Params params, VkPipeline pipeline);

    Device &_dev;
    VkSampler _nearest = VK_NULL_HANDLE, _linear = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _tilesPipeline = VK_NULL_HANDLE, _dof = VK_NULL_HANDLE, _blur = VK_NULL_HANDLE,
               _residual = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _dofSet = VK_NULL_HANDLE, _blurSet = VK_NULL_HANDLE;
    Image _scratch;  // copy of `color` depth of field reads
    Image _tiles;    // RGBA16F, one texel per 32x32 pixels, GENERAL layout
    VkImage _color = VK_NULL_HANDLE, _resolved = VK_NULL_HANDLE;
    VkImageView _colorView = VK_NULL_HANDLE, _resolvedView = VK_NULL_HANDLE;
    uint32_t _width = 0, _height = 0;        // the scene's
    uint32_t _outWidth = 0, _outHeight = 0;  // the resolve's (motion blur)
};

} // namespace vkr
