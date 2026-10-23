#pragma once

#include "device.h"

#include <functional>
#include <string>

namespace vkr {

// Cryptomatte's id of a name: MurmurHash3_x86_32 (seed 0) with exponent
// bits 0 and 255 flipped away, so its float is never denormal, inf or NaN.
uint32_t CryptomatteHash(const std::string &name);

// Extra AOVs beyond colour, depth and the ids (Renderer::Targets::aovs),
// rendered only when asked for: surface data from a G-buffer pass (the
// first opaque surface: shading normal, texture coordinates, base colour,
// emission), drawn after shading against its depth, and what follows from
// depth and the ids (eye-space position and depth, Cryptomatte). A compute
// pass writes each one packed (floats, bottom row first), then they are
// copied to memory the CPU reads. Where nothing was drawn all are 0.
// ponytail: Cryptomatte has one sample per pixel, so rank 0 holds the
// pixel's id at coverage 1 and edges don't blend; average jittered frames'
// ranks when mattes need soft edges. They show the view before lens
// distortion. Lighting splits (diffuse, specular) would need mesh.frag's
// light loops to keep their terms apart.
class Aovs {
public:
    enum Kind : uint32_t {
        kNormal,          // world-space shading normal, xyz
        kNormalEye,       // the same in camera space (Hydra's Neye)
        kPositionEye,     // camera-space position (Peye)
        kCameraDepth,     // camera-space z (negative in front, as hdEmbree)
        kSt,              // the mesh's texture coordinates, 2 floats
        kAlbedo,          // base colour, rgb
        kEmission,        // emitted radiance, rgb
        kCryptoObject,    // Cryptomatte rank 0 and 1: (id, coverage, id, coverage)
        kCryptoMaterial,
        kCount
    };
    static uint32_t Components(Kind kind);

    // Anything asked for; whether that needs the G-buffer pass.
    static bool Any(float *const targets[kCount]);
    static bool NeedsGeometry(float *const targets[kCount]);

    explicit Aovs(Device &device);
    ~Aovs();
    Aovs(const Aovs &) = delete;
    Aovs &operator=(const Aovs &) = delete;

    // The G-buffer pass's pipelines in Renderer's 16 mesh variants (bits:
    // kWideSkin, kDualQuaternion, GRAPH, and with it procedurals:
    // program.glsl's constant 8), made with `layout` on first use.
    const VkPipeline *Pipelines(VkPipelineLayout layout);

    struct Params {
        float view[16];          // world to camera (row-vector order)
        float invViewProj[16];   // window -> world, unjittered
        const uint32_t *cryptomatte = nullptr;  // per primId: object, material
        uint32_t cryptomatteIds = 0;
    };
    // `depth` and `primId` are the frame's (TRANSFER_SRC_OPTIMAL, left so).
    // `drawGeometry` records the opaque draws (Pipelines(), the scene's set
    // and buffers bound by it) into the G-buffer, if NeedsGeometry; pass
    // null when there is nothing to draw.
    void Record(VkCommandBuffer cmd, const Params &params, uint32_t width, uint32_t height, VkImage depth,
                VkImageView depthView, VkImage primId, VkImageView primIdView, float *const targets[kCount],
                const std::function<void()> &drawGeometry);
    // After the frame's fence: copies what Record wrote into `targets`.
    void Read(float *const targets[kCount]) const;

private:
    void _Resize(uint32_t width, uint32_t height);

    Device &_dev;
    VkSampler _nearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    VkPipeline _resolve = VK_NULL_HANDLE;
    VkPipeline _geometry[16] = {};  // null: procedurals without GRAPH
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    Image _normalSt, _albedo, _emission;  // the G-buffer
    Buffer _params;                       // view, invViewProj, then the Cryptomatte table
    Buffer _output;                       // every AOV asked for, packed (device-local)
    Buffer _readback;                     // its copy the CPU reads
    VkDeviceSize _offsets[kCount] = {};   // each one's place in _output (floats)
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
