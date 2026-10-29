#pragma once

#include "device.h"

#include <functional>
#include <string>
#include <vector>

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
// Cryptomatte is 12 floats per pixel: 6 ranks of (id, coverage), sorted by
// coverage, read as 3 layers of (id, coverage, id, coverage). Under the film
// the ranks are the 6 ids with the most weight of the samples' filter (the
// colour AOVs' Gaussian: coverage = an id's weight / all weight, the
// background's included); otherwise (TAA, Params::cryptoSoft off) the pixel's
// id is rank 0 at coverage 1. Everything shows the view before lens
// distortion.
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
        kCryptoObject,    // Cryptomatte: 6 x (id, coverage), best first
        kCryptoMaterial,
        kPositionWorld,   // world-space position (Karma's P)
        kDepth,           // distance along the view axis, positive (Karma's depth, ray:hitPz)
        kDiffuseAlbedo,   // base colour * (1 - metallic), rgb (Karma's albedodiffuse, lpe:CDA)
        // Light splits, stored by the shading pass (mesh.frag): direct is
        // the lights' (distant, sphere, rect, disk) diffuse and glossy light
        // and, with Frame::splitDomeDirect (Karma's convention), the dome's
        // unblocked light; indirect the rest (GI, SSGI, probes,
        // reflections); combined: both.
        kDirectDiffuse, kIndirectDiffuse, kCombinedDiffuse,
        kDirectGlossy, kIndirectGlossy, kCombinedGlossy,
        // The film's beauty before Frame::denoise, linear and exposed, rgba:
        // the Renderer's own (no pass here), once the film converged.
        kBeautyRaw,
        kCount
    };
    // Floats per pixel each kind has (a target may ask for 1..4 instead:
    // missing ones are 0, extra ones dropped).
    static uint32_t Components(Kind kind);

    // Anything asked for; whether that needs the G-buffer pass.
    static bool Any(float *const targets[kCount]);
    static bool NeedsGeometry(float *const targets[kCount]);
    static bool NeedsSplits(float *const targets[kCount]);

    // The light splits' image (RGBA16F, layers direct diffuse, indirect
    // diffuse, direct glossy, indirect glossy; GENERAL): made at this size
    // (1 x 1 without splits) before the frame's descriptors are written;
    // ClearSplits zeroes it before the shading pass stores into it, with
    // `keepPrevious` first copying last frame's into PreviousSplitsView
    // (SHADER_READ_ONLY; SSR takes the light path intensities out of last
    // frame's colour by it). EnsureSplits returns true when it made them
    // anew (no previous splits then).
    bool EnsureSplits(uint32_t width, uint32_t height);
    VkImageView SplitsView() const { return _splits.view; }
    VkImageView PreviousSplitsView() const { return _prevSplits.view; }
    void ClearSplits(VkCommandBuffer cmd, bool keepPrevious = false);

    explicit Aovs(Device &device);
    ~Aovs();
    Aovs(const Aovs &) = delete;
    Aovs &operator=(const Aovs &) = delete;

    // The G-buffer pass's pipelines in Renderer's mesh variants of these
    // bits (kWideSkin, kDualQuaternion, GRAPH, and with it procedurals:
    // program.glsl's constant 8; displaced materials: displace.glsl's
    // constant 10), made with `layout` on first use.
    static constexpr uint32_t kVariants = 1 | 2 | 4 | 8 | 32;
    const VkPipeline *Pipelines(VkPipelineLayout layout);

    struct Params {
        float view[16];          // world to camera (row-vector order)
        float invViewProj[16];   // window -> world, unjittered
        const uint32_t *cryptomatte = nullptr;  // per primId: object, material
        uint32_t cryptomatteIds = 0;
        uint8_t components[kCount] = {};  // floats per pixel of each target (0: Components)
        // Accumulation: frames of a still camera and scene are averaged
        // (ids, Cryptomatte: the latest), up to maxFrames.
        float viewProj[16] = {};
        uint64_t version = 0;      // the scene's (Scene::Gpu::version)
        uint32_t maxFrames = 1;
        // Film samples: the colour AOVs (albedo, emission, the splits) are
        // reconstructed as the beauty is (the Gaussian filter of `filmRadius`
        // at this sample's `filmOffset` from the pixel centres, summed with
        // their weights); the rest take the sample nearest the pixel centre
        // so far (`filmNearer`, as the ids) instead of averaging.
        bool film = false, filmNearer = false;
        bool cryptoSoft = true;  // Cryptomatte takes the film's samples (else the nearest one's id)
        float filmOffset[2] = {}, filmRadius = 0;
        VkImageView filmWeights = VK_NULL_HANDLE;  // the film's (TemporalAA::FilmWeights), GENERAL
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
    VkPipeline _geometry[kVariants + 1] = {};  // null: procedurals without GRAPH, other bits
    VkDescriptorPool _pool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    Image _normalSt, _albedo, _emission;  // the G-buffer
    Image _splits;                        // light splits, EnsureSplits
    Image _prevSplits;                    // last frame's, ClearSplits
    uint32_t _splitsWidth = 0, _splitsHeight = 0;
    // Accumulation state (Record).
    uint32_t _frames = 0, _accumWidth = 0, _accumHeight = 0;
    float _viewProj[16] = {};
    uint64_t _version = 0;
    std::vector<uint32_t> _accumLayout;
    VkBuffer _accumBuffer = VK_NULL_HANDLE, _accumSums = VK_NULL_HANDLE;
    Buffer _params;                       // view, invViewProj, then the Cryptomatte table
    Buffer _output;                       // every AOV asked for, packed (device-local)
    Buffer _sums;                         // the colour AOVs' film sums (shader), weights
    VkDeviceSize _sumsOffsets[kCount] = {};
    Buffer _readback;                     // its copy the CPU reads
    VkDeviceSize _offsets[kCount] = {};   // each one's place in _output (floats)
    uint32_t _components[kCount] = {};    // and its floats per pixel
    uint32_t _width = 0, _height = 0;
};

} // namespace vkr
