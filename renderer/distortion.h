#pragma once

#include "device.h"

namespace vkr {

// Lens distortion (HdCamera's lensDistortion:*): the finished frame is
// warped the way a real lens bends the pinhole image, colour, depth and the
// picking ids alike, so a click picks what is shown there. Each output
// pixel looks up the pinhole image at Warp() of its screen point. The scene
// must cover what the warp reaches: the caller widens the camera's view by
// Overscan() (Params::overscan) for the warp to sample.
// ponytail: the overscanned view renders at the output resolution, so the
// centre loses detail by that factor (at most 2x); render it larger when it
// shows. Motion vectors (vkrtest only) stay unwarped.
class LensDistortion {
public:
    // Screen units: x spans [-1, 1] across the image, y the same scale
    // (square pixels), +y up; aspect = width / height.
    // "standard" is OpenCV's model, as Karma's lens material has it
    // (kma_physicallens opencv_k1..k6, opencv_p1, opencv_p2): radial
    // (1 + k1 r^2 + k2 r^4 + k3 r^6) / (1 + k4 r^2 + k5 r^4 + k6 r^6) and
    // tangential (2 p1 x y + p2 (r^2 + 2 x^2), p1 (r^2 + 2 y^2) + 2 p2 x y);
    // positive k1 is barrel, as there. HdCamera's k1 and k2 are its first
    // two terms.
    // ponytail: Karma's lens shader works in its NDC, which its docs leave
    // unclear for wide images; Eldr's r is in the screen units above (x
    // across the width). Compare with a husk render to settle it.
    struct Params {
        bool fisheye = false;   // lensDistortion:type "fisheye", else "standard"
        float k1 = 0, k2 = 0;   // radial; fisheye: of the angle (r * (1 + k1 r^2 + k2 r^4))
        float k3 = 0, k4 = 0, k5 = 0, k6 = 0;  // standard: radial, as above
        float p1 = 0, p2 = 0;   // tangential, as above
        float center[2] = {0, 0};
        float anaSq = 1;        // anamorphic squeeze: x scaled by it before the radial terms
        float asym[2] = {0, 0}; // asymmetry: + asym * r^2
        float scale = 1;        // > 1 zooms in
        float tanHalfWidth = 1; // fisheye: tan of half the camera's horizontal view (1 / projection[0][0])
        float overscan = 1;     // the rendered view is this much wider than the camera's

        // Anything to warp at all.
        bool Active() const;
    };

    // Where screen point s of the output looks in the camera's (pinhole)
    // image, in the same units; false where no pinhole ray exists (a
    // fisheye's view at 90 degrees and beyond). As shaders/distortion.frag.
    static bool Warp(const Params &p, float aspect, const float s[2], float source[2]);
    // How much wider than the camera's view the pinhole image must be for
    // the warp to stay inside it, 1..2.
    static float Overscan(const Params &p, float aspect);

    explicit LensDistortion(Device &device);
    ~LensDistortion();
    LensDistortion(const LensDistortion &) = delete;
    LensDistortion &operator=(const LensDistortion &) = delete;

    // Warps the frame in place: colour (RGBA16F), depth (D32), primId and
    // instanceId (R32_SINT), all width x height and in TRANSFER_SRC_OPTIMAL,
    // left so. Where nothing is seen the clear values go.
    void Record(VkCommandBuffer cmd, const Params &params, uint32_t width, uint32_t height, VkImage color,
                VkImageView colorView, VkImage depth, VkImageView depthView, VkImage primId,
                VkImageView primIdView, VkImage instanceId, VkImageView instanceIdView, const float clearColor[4],
                float clearDepth);

private:
    void _Resize(uint32_t width, uint32_t height);

    Device &_dev;
    VkSampler _linear = VK_NULL_HANDLE, _nearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    Image _color, _depth, _primId, _instanceId;  // copies the warp reads
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
