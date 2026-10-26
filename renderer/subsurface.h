#pragma once

#include "device.h"

namespace vkr {

// Screen-space subsurface scattering, a separable blur of the diffuse
// light of subsurface materials (after Jimenez et al. 2015, with a
// Gaussian per colour channel of the material's radius as the profile).
// The Renderer shades the subsurface meshes a second time into Light()
// (their subsurface lobe only: subsurface colour times the diffuse light
// reaching them; alpha: the largest radius) and Id() (the radii's
// proportions, packed; 0 elsewhere). Blur then spreads that light over
// the surface, horizontally and vertically, weighting taps by their
// distance in the world (so it stops at depth jumps) and skipping other
// materials, and adds the difference to the colour.
class Subsurface {
public:
    static constexpr VkFormat kLightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kIdFormat = VK_FORMAT_R32_SINT;

    explicit Subsurface(Device &device);
    ~Subsurface();
    Subsurface(const Subsurface &) = delete;
    Subsurface &operator=(const Subsurface &) = delete;

    // Drops the targets; `depth` (the prepass's) must stay valid until the
    // next Resize.
    void Resize(uint32_t width, uint32_t height, VkImageView depth);

    // Makes the targets on first use (the GPU is idle there) and leaves
    // Light() and Id() in COLOR_ATTACHMENT_OPTIMAL for the subsurface pass
    // to clear.
    void BeginLighting(VkCommandBuffer cmd);
    const Image &Light() const { return _light; }
    const Image &Id() const { return _id; }

    // After the subsurface pass: blurs Light() over `samples` taps per
    // direction and adds blurred minus unblurred light to `color` (in
    // COLOR_ATTACHMENT_OPTIMAL). `invViewProj` (row-vector order) maps the
    // depth's NDC to world space.
    // With `moveSplits` the same difference moves in the light splits
    // `splits` (Aovs::SplitsView), shared between direct and indirect
    // diffuse by their unblurred shares, without the light path
    // intensities (`direct`, `indirect`) the colour carries.
    void Blur(VkCommandBuffer cmd, VkImageView color, const float invViewProj[16], uint32_t samples,
              VkImageView splits, bool moveSplits, float direct = 1.0f, float indirect = 1.0f);

private:
    void _Destroy();

    Device &_dev;
    VkSampler _sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _horizontal = VK_NULL_HANDLE, _vertical = VK_NULL_HANDLE;
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _sets[2] = {};  // per pass: what it blurs
    VkImageView _depth = VK_NULL_HANDLE;
    Image _light, _id, _blurred;  // _blurred: after the horizontal pass
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
