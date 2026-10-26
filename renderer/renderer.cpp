#include "renderer.h"

#include "ibl.h"
#include "probes.h"
#include "vkmath.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <functional>
#include <random>
#include <tuple>
#include <map>
#include <vector>

namespace vkr {

// std140 mirror of FrameData in shaders/frame.glsl.
struct FrameData {
    float viewProj[16];
    float invViewProj[16];
    float cameraPos[4];
    float cameraForward[4];
    float viewport[4];
    uint32_t counts[4];  // lightCount, shadowLight, envLevels, envLatlong
    float cascadeViewProj[4][16];
    float cascadeSplits[4];
    float cascadeTexel[4];
    float envSh[9][4];
    float envWorldToLocal[16];
    float envRadiance[4];
    float prevViewProj[16];
    float ssr[4];  // enabled, noise index
    float clusters[4];       // near, far, log2(far / near), light cutoff radiance
    uint32_t localLights[4]; // sphere/rect light count
    uint32_t features[4];    // kFeature* flags, SSR steps, soft shadow samples
    uint32_t styles[4];      // kStyle* flags, toon bands for every material
    float translucency[4];   // see frame.glsl
    // Volumetric fog (shaders/fog.glsl).
    float fogMedium[4];      // density, height falloff, base height, phase g
    float fogAlbedo[4];      // scattering colour, froxel depth
    uint32_t fogGrid[4];     // froxels x, y, z; heights along Z
    float fogTemporal[4];    // slice jitter, history weight, last frame's froxel depth, orthographic
    // Reflection probe and planar reflections (shaders/probes.glsl).
    uint32_t reflections[4];     // probes, -, flags (kProbeNormalize, kFogGiAmbient, kLightTextures)
    float probeCenter[8][4];     // capture position, prefiltered levels - 1
    float probeMin[8][4];        // influence box, blend distance (0: everywhere)
    float probeMax[8][4];
    float probeSh[8 * 4][4];     // irradiance SH L0, L1 per probe
    float planarPlane[4][4]; // the mirrors' planes, facing the camera
    uint32_t planarInfo[4];  // the mirrors' mesh slots, kNone: unused
    float geometry[4];       // displacement scale, w graphs (kFeatureDisplacement); LOD bias, frame (kFeatureLod)
    // Sample dimensions (frame.glsl's SampleNoise): this sample's value per
    // stochastic effect (kDim*); x of sampleInfo 1 with them on.
    float sampleDims[3][4];
    uint32_t sampleInfo[4];
    uint32_t aovs[4];        // light splits (aov.h): x 1 = the shading pass stores them, y 1 = the dome's visible light is direct
    float lightClamp[4];     // brightness limits of direct, indirect light (0: off)
    float screenTrace[4];    // thickness estimator (SSR) on, back-face hits' share of light, SSR hit filter on
    uint32_t shadowJitter[4];  // x 1: light shapes jittered this sample (Frame::shadowJitter), hard lookups
    uint32_t lodFade[4];     // frames a level of detail crossfade lasts (0: none)
    float lightPath[4];      // Light path intensity: direct, indirect multiplier; z 1: either isn't 1 (mesh.frag)
    float deform[4];         // x: time sample's offset from this frame (frames), y: 1 = Mesh::velocities on,
                             // z: 1 = a time sample (motion vectors from the shutter's capture),
                             // w: its capture holds joints (+1) and blend shape weights (+2)
    uint32_t rtRefraction[4];  // x 1: refracting front faces read the traced refraction (Frame::rtRefraction)
};

namespace {

const uint32_t kMeshVert[] =
#include "mesh.vert.inc"
;
const uint32_t kMeshDepthVert[] =
#include "mesh_depth.vert.inc"
;
const uint32_t kMeshFrag[] =
#include "mesh.frag.inc"
;
const uint32_t kMeshGraphVert[] =
#include "mesh_graph.vert.inc"
;
const uint32_t kMeshGraphFrag[] =
#include "mesh_graph.frag.inc"
;
const uint32_t kMeshLayersFrag[] =
#include "mesh_layers.frag.inc"
;
const uint32_t kMeshGraphLayersFrag[] =
#include "mesh_graph_layers.frag.inc"
;
const uint32_t kMaskFrag[] =
#include "mask.frag.inc"
;
const uint32_t kFullscreenVert[] =
#include "fullscreen.vert.inc"
;
const uint32_t kSkyFrag[] =
#include "sky.frag.inc"
;
const uint32_t kCullComp[] =
#include "cull.comp.inc"
;
const uint32_t kLightsComp[] =
#include "lights.comp.inc"
;
const uint32_t kSkinComp[] =
#include "skin.comp.inc"
;
const uint32_t kShutterComp[] =
#include "shutter.comp.inc"
;
const uint32_t kShadowTintFrag[] =
#include "shadow_tint.frag.inc"
;
const uint32_t kMatteFrag[] =
#include "matte.frag.inc"
;
const uint32_t kMatteGraphFrag[] =
#include "matte_graph.frag.inc"
;
const uint32_t kShadowMinComp[] =
#include "shadow_min.comp.inc"
;

// FrameData::features[0] (Frame switches the shaders read), as in frame.glsl.
constexpr uint32_t kFeatureNormalMaps = 1, kFeatureMaterialGraphs = 2, kFeatureClusters = 4;
constexpr uint32_t kFeatureProcedurals = 256;
constexpr uint32_t kFeatureDisplacement = 1024, kFeatureLod = 2048;
constexpr uint32_t kLodFadeFrames = 8;  // a level of detail change crossfades over (Frame::lodFade)
// Material lobes, as shaders/brdf.glsl numbers them.
constexpr uint32_t kFeatureSpecularWorkflow = 8, kFeatureClearcoat = 16, kFeatureSheen = 32, kFeatureThinFilm = 64,
                   kFeatureAnisotropy = 128;
// FrameData::styles[0], as brdf.glsl.
constexpr uint32_t kStyleToon = 1, kStyleHair = 2, kStyleTexturedLobes = 4, kStyleTexturedTransmission = 8;
constexpr uint32_t kFeatureToon = 512;  // FrameData::styles[1] > 0
// Ray tracing's (shaders/gi.glsl).
constexpr uint32_t kFeatureRtShadows = 1u << 20, kFeatureRtReflections = 1u << 21, kFeatureGi = 1u << 22;
constexpr uint32_t kFeatureSsgi = 1u << 23;  // gi.glsl
// The TLAS's custom index (a mesh slot) has 24 bits, and so has its count.
constexpr uint32_t kMaxRtInstances = (1u << 24) - 1;
// Translucency (transparency.glsl).
constexpr uint32_t kFeatureRefraction = 4096, kFeatureTranslucentShadows = 8192;
// Subsurface scattering (subsurface.glsl).
constexpr uint32_t kFeatureSubsurface = 16384;
// Holdouts (transparency.glsl).
constexpr uint32_t kFeatureHoldouts = 32768;
constexpr uint32_t kFeatureSoftSun = 65536, kFeatureFog = 131072, kFeatureProbe = 262144, kFeaturePlanar = 524288;
// The ones only the shading pipelines with their code have (frame.glsl's
// kCompiledFeatures, Renderer::_ShadingSet).
constexpr uint32_t kOptionalFeatures = kFeatureRtShadows | kFeatureRtReflections | kFeatureGi | kFeatureSsgi |
                                       kFeatureRefraction |
                                       kFeatureTranslucentShadows | kFeatureSubsurface | kFeatureHoldouts |
                                       kFeatureSoftSun | kFeatureFog | kFeatureProbe | kFeaturePlanar | kFeatureToon;

// A material sampler: trilinear, `anisotropy` (1: none), wrap modes
// s + kWrapModes * t (Scene::Gpu::textureWraps).
VkSampler CreateMaterialSampler(VkDevice device, float anisotropy, uint32_t wrap)
{
    static const VkSamplerAddressMode modes[kWrapModes] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = modes[wrap % kWrapModes];
    sci.addressModeV = modes[wrap / kWrapModes % kWrapModes];
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;  // Storm's HdWrapBlack
    sci.anisotropyEnable = anisotropy > 1.0f;
    sci.maxAnisotropy = anisotropy;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler sampler = VK_NULL_HANDLE;
    Check(vkCreateSampler(device, &sci, nullptr, &sampler), "vkCreateSampler");
    return sampler;
}

// Light clusters (shaders/lights.glsl): 16 x 9 x 24, a count and 63
// indices each.
constexpr VkDeviceSize kClusterBytes = 16 * 9 * 24 * 64 * sizeof(uint32_t);

// Push constants of cull.comp; the vertex stages read only the first 64 bytes
// (mat4 viewProj) of the same range.
struct CullParams {
    float planes[6][4];
    uint32_t instanceCapacity;
    uint32_t drawCount;
    float pixelScale;    // perspective: pixels per world unit at view depth 1; ortho: per unit
    float nearDistance;  // view depth of the near plane
    float minRadius;     // in pixels: smaller instances are dropped
    float orthographic;
    uint32_t listBase;     // list of instance indices written (see kVisibleLists)
    uint32_t commandBase;  // first of its draw commands
};

constexpr VkShaderStageFlags kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

// What the vertex stage reads of the same push range.
struct DrawParams {
    float viewProj[16];
    uint32_t listBase;
    uint32_t motion;  // see mesh.vert
};

void PushDraw(VkCommandBuffer cmd, VkPipelineLayout layout, const float *viewProj, uint32_t listBase,
              uint32_t motion = 0)
{
    DrawParams p;
    std::memcpy(p.viewProj, viewProj, sizeof(p.viewProj));
    p.listBase = listBase;
    p.motion = motion;
    vkCmdPushConstants(cmd, layout, kPushStages, 0, sizeof(p), &p);
}

// The opaque and cutout draws of one command list (commands[first] at
// offset), one indirect call per chunk (Scene::Gpu::drawChunks), each with
// its pipeline: `cutout` for chunks of cutout materials, in the chunk's
// variant (chunkKind >> 2, within `variants`: depth passes have no program
// variants). Translucent chunks are left to the translucent pass (and cast
// no shadows).
// `fading`: levels of detail crossfade, so their chunks take `cutout` too
// (mask.frag's dither; the camera's depth prepass).
void DrawChunks(VkCommandBuffer cmd, const Scene::Gpu &gpu, VkBuffer commands, VkDeviceSize offset,
                const VkPipeline *opaque, const VkPipeline *cutout, uint8_t variants, bool fading = false)
{
    constexpr VkDeviceSize stride = sizeof(VkDrawIndexedIndirectCommand);
    VkPipeline bound = VK_NULL_HANDLE;
    for (size_t i = 0; i + 1 < gpu.drawChunks.size(); ++i) {
        const uint8_t kind = gpu.chunkKind[i] & (Scene::Gpu::kWide - 1);
        if (kind == Scene::Gpu::kTranslucent) {
            continue;
        }
        const bool masked = kind == Scene::Gpu::kCutout || (fading && gpu.chunkLevels[i]);
        const VkPipeline pipeline = (masked ? cutout : opaque)[(gpu.chunkKind[i] >> 2) & variants];
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        const uint32_t first = gpu.drawChunks[i];
        vkCmdDrawIndexedIndirect(cmd, commands, offset + first * stride, gpu.drawChunks[i + 1] - first,
                                 uint32_t(stride));
    }
}

// World-space frustum planes (xyz . p + w >= 0 inside) of a GL clip-space
// viewProj in row-vector order (Gribb/Hartmann).
void FrustumPlanes(const float *vp, float planes[6][4])
{
    auto column = [&](int j, float out[4]) {
        for (int i = 0; i < 4; ++i) {
            out[i] = vp[i * 4 + j];
        }
    };
    float c[4][4];
    for (int j = 0; j < 4; ++j) {
        column(j, c[j]);
    }
    for (int p = 0; p < 6; ++p) {
        const float sign = (p % 2 == 0) ? 1.0f : -1.0f;  // left/right, bottom/top, near/far
        for (int i = 0; i < 4; ++i) {
            planes[p][i] = c[3][i] + sign * c[p / 2][i];
        }
        const float len = std::sqrt(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] +
                                    planes[p][2] * planes[p][2]);
        for (int i = 0; i < 4; ++i) {
            planes[p][i] /= len > 0 ? len : 1.0f;
        }
    }
}

constexpr uint32_t kBrdfLutSize = 64;

// Stages that sample the shadow maps: shading and the volumetric fog.
constexpr VkPipelineStageFlags2 kShadowReaders =
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

// Spots with a cone up to this get one perspective atlas tile, wider
// lights a cube of six. Must match kSpotShadowCone in mesh.frag.
constexpr float kSpotShadowCone = 1.3962634f;  // 80 degrees

VkImageSubresourceRange Range(VkImageAspectFlags aspect, uint32_t layers = 1)
{
    return {aspect, 0, 1, 0, layers};
}

Mat4 ToMat4(const float *m)
{
    Mat4 r;
    std::memcpy(r.m, m, sizeof(r.m));
    return r;
}

// Low-discrepancy sub-pixel offsets in [0, 1) for TAA jitter.
float Halton(uint32_t index, uint32_t base)
{
    float f = 1.0f, r = 0.0f;
    for (; index > 0; index /= base) {
        f /= float(base);
        r += f * float(index % base);
    }
    return r;
}

// Sample dimensions (frame.glsl's kDim*): a Sobol dimension of its own per
// stochastic effect (the pixel jitter is Halton 2, 3), so effects never
// share a pattern and each is stratified at every power of two samples;
// with the blue noise, fract(noise + value) per pixel.
constexpr uint32_t kSampleDims = 12, kDimAo = 0, kDimSsgi = 1, kDimSsrDenoise = 10, kDimSsgiJitter = 11;
constexpr uint32_t kBlueNoiseSize = 64;

// Sobol dimensions 2..13 (Joe and Kuo's new-joe-kuo-6.21201: degree s,
// coefficients a, initial m) at `index`, all 32 bits.
uint32_t SobolBits(uint32_t index, uint32_t dim)
{
    struct Poly {
        uint32_t s, a, m[5];
    };
    static const Poly kPolys[kSampleDims] = {
        {1, 0, {1}},        {2, 1, {1, 3}},        {3, 1, {1, 3, 1}},        {3, 2, {1, 1, 1}},
        {4, 1, {1, 1, 3, 3}}, {4, 4, {1, 3, 5, 13}}, {5, 2, {1, 1, 5, 5, 17}}, {5, 4, {1, 1, 5, 5, 5}},
        {5, 7, {1, 1, 7, 11, 19}}, {5, 11, {1, 1, 5, 1, 1}}, {5, 13, {1, 1, 1, 3, 11}}, {5, 14, {1, 3, 5, 5, 31}},
    };
    const Poly &p = kPolys[dim];
    uint32_t v[32];
    for (uint32_t i = 0; i < 32; ++i) {
        if (i < p.s) {
            v[i] = p.m[i] << (31 - i);
        } else {
            v[i] = v[i - p.s] ^ (v[i - p.s] >> p.s);
            for (uint32_t k = 1; k < p.s; ++k) {
                v[i] ^= ((p.a >> (p.s - 1 - k)) & 1u) * v[i - k];
            }
        }
    }
    uint32_t x = 0;
    for (uint32_t i = 0; index; ++i, index >>= 1) {
        x ^= (index & 1u) * v[i];
    }
    return x;
}

// The same in [0, 1).
float Sobol(uint32_t index, uint32_t dim)
{
    return float(SobolBits(index, dim) >> 8) / 16777216.0f;
}

uint32_t ReverseBits(uint32_t x)
{
    x = (x << 16) | (x >> 16);
    x = ((x & 0x00FF00FFu) << 8) | ((x & 0xFF00FF00u) >> 8);
    x = ((x & 0x0F0F0F0Fu) << 4) | ((x & 0xF0F0F0F0u) >> 4);
    x = ((x & 0x33333333u) << 2) | ((x & 0xCCCCCCCCu) >> 2);
    return ((x & 0x55555555u) << 1) | ((x & 0xAAAAAAAAu) >> 1);
}

uint32_t HashU32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

// Owen scrambling of 32-bit fixed point (Burley 2020, "Practical
// Hash-based Owen Scrambling": the Laine-Karras permutation on reversed bits).
uint32_t NestedUniformScramble(uint32_t x, uint32_t seed)
{
    x = ReverseBits(x) + seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return ReverseBits(x);
}

// Points of the light shapes and the lens, made on the CPU per sample:
// Burley's shuffled, Owen-scrambled Sobol (van der Corput, then Sobol
// dimensions 2 and 3), one sequence per seed. Its first two dimensions are
// a (0, 2)-sequence: a disc's or sphere's projections converge about twice
// as fast as through the sample dimensions' unscrambled pairs (measured on
// 128 samples: 0.02-0.03 worst error in a slab's share, not 0.06-0.08).
constexpr uint32_t kSeedLightShape = 1, kSeedLens = 2, kSeedShutter = 3, kSeedSsrGgx = 4, kSeedRtShadow = 5;
void ScrambledSobol(uint32_t index, uint32_t seed, float *out, uint32_t dims)
{
    index = NestedUniformScramble(index, HashU32(seed));
    for (uint32_t d = 0; d < dims; ++d) {
        const uint32_t x = d == 0 ? ReverseBits(index) : SobolBits(index, d - 1);
        out[d] = float(NestedUniformScramble(x, HashU32(seed * 8 + 1 + d)) >> 8) / 16777216.0f;
    }
}

// Blue noise (Ulichney's void-and-cluster, Gaussian sigma 1.5, toroidal):
// a rank per texel, 0..255 in 4 independent channels (RGBA8).
std::vector<uint8_t> BuildBlueNoise()
{
    constexpr int N = int(kBlueNoiseSize), kR = 6;
    std::vector<uint8_t> out(N * N * 4);
    float kernel[2 * kR + 1][2 * kR + 1];
    for (int y = -kR; y <= kR; ++y) {
        for (int x = -kR; x <= kR; ++x) {
            kernel[y + kR][x + kR] = std::exp(-float(x * x + y * y) / (2.0f * 1.5f * 1.5f));
        }
    }
    for (int channel = 0; channel < 4; ++channel) {
        std::vector<float> energy(N * N, 0.0f);
        std::vector<uint8_t> on(N * N, 0);
        auto splat = [&](int i, float s) {
            for (int y = -kR; y <= kR; ++y) {
                for (int x = -kR; x <= kR; ++x) {
                    energy[((i / N + y) & (N - 1)) * N + ((i % N + x) & (N - 1))] += s * kernel[y + kR][x + kR];
                }
            }
        };
        auto extreme = [&](uint8_t state, bool most) {  // tightest cluster of ones, largest void of zeros
            int best = -1;
            for (int i = 0; i < N * N; ++i) {
                if (on[i] == state && (best < 0 || (most ? energy[i] > energy[best] : energy[i] < energy[best]))) {
                    best = i;
                }
            }
            return best;
        };
        std::mt19937 rng(1234u + uint32_t(channel));
        int ones = 0;
        while (ones < N * N / 10) {
            const int i = int(rng() % uint32_t(N * N));
            if (!on[i]) {
                on[i] = 1;
                splat(i, 1.0f);
                ++ones;
            }
        }
        for (;;) {  // spread the initial pattern evenly
            const int cluster = extreme(1, true);
            on[cluster] = 0;
            splat(cluster, -1.0f);
            const int gap = extreme(0, false);
            on[gap] = 1;
            splat(gap, 1.0f);
            if (gap == cluster) {
                break;
            }
        }
        std::vector<int> rank(N * N);
        const std::vector<uint8_t> initial = on;
        const std::vector<float> initialEnergy = energy;
        for (int r = ones - 1; r >= 0; --r) {
            const int cluster = extreme(1, true);
            on[cluster] = 0;
            splat(cluster, -1.0f);
            rank[cluster] = r;
        }
        on = initial;
        energy = initialEnergy;
        // ponytail: phase 3 keeps filling the largest void (no inversion);
        // fine for 8-bit ranks.
        for (int r = ones; r < N * N; ++r) {
            const int gap = extreme(0, false);
            on[gap] = 1;
            splat(gap, 1.0f);
            rank[gap] = r;
        }
        for (int i = 0; i < N * N; ++i) {
            out[i * 4 + channel] = uint8_t(rank[i] * 256 / (N * N));
        }
    }
    return out;
}

// While the camera moves the TAA blend weight stays at 1 / (kTaaMoving + 1)
// = 0.1; a still camera accumulates Frame::taaSamples.
constexpr uint32_t kTaaMoving = 9;

} // namespace

Renderer::Renderer(Device &device)
    : _dev(device)
    , _gtao(device)
    , _taa(device)
    , _lens(device)
    , _post(device)
    , _transparency(device)
    , _subsurface(device)
    , _ssr(device)
    , _fog(device)
{
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = _dev.queueFamily;
    Check(vkCreateCommandPool(_dev.device, &pci, nullptr, &_pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = _pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(_dev.device, &ai, &_cmd), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(vkCreateFence(_dev.device, &fci, nullptr, &_fence), "vkCreateFence");

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_dev.physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families);
    vkGetPhysicalDeviceQueueFamilyProperties(_dev.physical, &families, family.data());
    if (family[_dev.queueFamily].timestampValidBits > 0) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(_dev.physical, &props);
        _timestampMs = double(props.limits.timestampPeriod) * 1e-6;
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kMaxTimestamps;
        Check(vkCreateQueryPool(_dev.device, &qci, nullptr, &_timestamps), "vkCreateQueryPool");
    }

    _dev.Reserve(_frameData, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    _clusters = _dev.CreateDeviceBuffer(kClusterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    _CreateDescriptors();
    // The shaders here key the device's pipeline cache file.
    // ponytail: other files' shaders aren't in the key; the driver skips
    // their stale entries.
    uint64_t hash = 14695981039346656037ull;  // FNV-1a
    for (const auto &[words, bytes] :
         {std::pair{kMeshVert, sizeof(kMeshVert)}, {kMeshDepthVert, sizeof(kMeshDepthVert)},
          {kMeshFrag, sizeof(kMeshFrag)}, {kMeshGraphVert, sizeof(kMeshGraphVert)},
          {kMeshGraphFrag, sizeof(kMeshGraphFrag)}, {kMeshLayersFrag, sizeof(kMeshLayersFrag)},
          {kMeshGraphLayersFrag, sizeof(kMeshGraphLayersFrag)}, {kMaskFrag, sizeof(kMaskFrag)},
          {kSkyFrag, sizeof(kSkyFrag)}, {kCullComp, sizeof(kCullComp)}, {kLightsComp, sizeof(kLightsComp)},
          {kSkinComp, sizeof(kSkinComp)}, {kShadowTintFrag, sizeof(kShadowTintFrag)},
          {kShadowMinComp, sizeof(kShadowMinComp)}}) {
        for (size_t i = 0; i < bytes / sizeof(uint32_t); ++i) {
            hash = (hash ^ words[i]) * 1099511628211ull;
        }
    }
    _dev.LoadPipelineCache(hash);
    _CreatePipelines();
    _fog.CreatePipelines(_layout);
    _CreateConstantImages();
    if (_dev.rayTracing) {
        _rt = std::make_unique<RayTracing>(_dev, _setLayout);
    }
    _volumePass = std::make_unique<VolumePass>(_dev, _setLayout);
}

Renderer::~Renderer()
{
    {
        std::lock_guard<std::mutex> lock(_prewarmMutex);
        _prewarmStop = true;
    }
    _prewarmChanged.notify_all();
    if (_prewarmThread.joinable()) {
        _prewarmThread.join();
    }
    _MergePrewarmCache();
    for (const Prewarmed &p : _prewarmed) {
        for (VkPipeline pipeline : p.pipelines) {
            vkDestroyPipeline(_dev.device, pipeline, nullptr);
        }
    }
    vkDestroyPipelineCache(_dev.device, _prewarmCache, nullptr);
    vkDeviceWaitIdle(_dev.device);
    _rt.reset();
    _volumePass.reset();
    for (VkImageView v : _shadowLayers) {
        vkDestroyImageView(_dev.device, v, nullptr);
    }
    for (Image *img : {&_color, &_primId, &_instanceId, &_depth, &_velocity, &_shadowMap, &_shadowAtlas, &_brdfLut,
                       &_blackCube, &_blueNoise, &_up[0], &_up[1], &_up[2], &_up[3], &_filmIds[0], &_filmIds[1],
                       &_filmIds[2]}) {
        _dev.Destroy(*img);
    }
    _dev.Destroy(_readback);
    _dev.Destroy(_frameData);
    _dev.Destroy(_commands);
    _dev.Destroy(_prevJoints);
    _dev.Destroy(_lodState);
    _dev.Destroy(_prevBlendWeights);
    for (Buffer *b : {&_shutterRows, &_shutterJoints, &_shutterWeights, &_savedInstances, &_savedJoints,
                      &_savedWeights}) {
        _dev.Destroy(*b);
    }
    _dev.Destroy(_clusters);
    _dev.Destroy(_lightShadows);
    _dev.Destroy(_shadowTiles);
    for (VkPipeline p : {_skyPipeline, _cullPipeline, _lightsPipeline, _skinPipeline, _shadowMinPipeline,
                         _shutterPipeline}) {
        vkDestroyPipeline(_dev.device, p, nullptr);
    }
    for (VkImageView v : _shadowMinLevels) {
        vkDestroyImageView(_dev.device, v, nullptr);
    }
    _dev.Destroy(_shadowMin);
    for (uint32_t v = 0; v < kVariants; ++v) {
        for (VkPipeline p : {_prepassPipeline[v], _shadowPipeline[v], _prepassCutoutPipeline[v],
                             _shadowCutoutPipeline[v], _tintDepthPipeline[v], _tintColorPipeline[v],
                             _mattePipeline[v]}) {
            vkDestroyPipeline(_dev.device, p, nullptr);
        }
    }
    for (uint32_t v = 0; v < kShadingSets * kVariants; ++v) {
        vkDestroyPipeline(_dev.device, _pipeline[v], nullptr);
        vkDestroyPipeline(_dev.device, _translucentPipeline[v], nullptr);
        vkDestroyPipeline(_dev.device, _oitPipeline[v], nullptr);
        vkDestroyPipeline(_dev.device, _refractBackPipeline[v], nullptr);
        vkDestroyPipeline(_dev.device, _subsurfacePipeline[v], nullptr);
    }
    vkDestroyPipelineLayout(_dev.device, _layout, nullptr);
    vkDestroyDescriptorPool(_dev.device, _descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(_dev.device, _setLayout, nullptr);
    for (VkSampler s : _samplers) {
        vkDestroySampler(_dev.device, s, nullptr);
    }
    for (VkSampler s : {_clampSampler, _shadowSampler, _minSampler}) {
        vkDestroySampler(_dev.device, s, nullptr);
    }
    vkDestroyFence(_dev.device, _fence, nullptr);
    vkDestroyQueryPool(_dev.device, _timestamps, nullptr);
    vkDestroyCommandPool(_dev.device, _pool, nullptr);
}

void Renderer::_CreateDescriptors()
{
    // The materials' samplers are made by Render (Frame::anisotropy).
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxAnisotropy = 1.0f;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_clampSampler), "vkCreateSampler");

    // Outside the cascade reads as depth 1, i.e. lit.
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    sci.maxLod = 0.0f;
    Check(vkCreateSampler(_dev.device, &sci, nullptr, &_shadowSampler), "vkCreateSampler");
    if (_dev.samplerMinmax) {
        VkSamplerReductionModeCreateInfo reduction{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
        reduction.reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN;
        VkSamplerCreateInfo mci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &reduction};
        mci.magFilter = mci.minFilter = VK_FILTER_LINEAR;
        mci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        mci.maxLod = VK_LOD_CLAMP_NONE;
        mci.addressModeU = mci.addressModeV = mci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Check(vkCreateSampler(_dev.device, &mci, nullptr, &_minSampler), "vkCreateSampler");
    }

    constexpr VkShaderStageFlags kFragment = VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr VkShaderStageFlags kGeometry = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 0 is the bindless array, partially bound and indexed by material
    // data. 7..10 are the GPU-driven scene: instances, draw infos, visible
    // lists and the culled indirect commands; 11 is the GTAO result, 12
    // and 13 last frame's color and this frame's depth for reflections, 14
    // the light clusters (written by lights.comp, which also reads 2 and 3),
    // 15, 16, 21 and 22 skinning (SkinVertex, joint matrices built by
    // skin.comp, skinning transforms, per-joint rest bounds), 23 and 24
    // blend shape offsets and weights, 25 the UDIM tile table, 26 and 27
    // last frame's joints and moved rigid transforms (motion vectors), 17..19
    // the local lights' shadow atlas, first tile per light and the tiles, 20
    // the atlas again without depth compare (soft shadows' blocker search),
    // 28..32 material programs: ops, constants, primvar table, primvar values
    // and vertex-rate primvars; 52..59 ray tracing's (RayTracing; partially
    // bound, 52 only exists with Device::rayTracing); 60 last frame's blend
    // shape weights. 47 and 48 the volumetric fog's volumes
    // (Fog::Descriptors; the fog also reads the lights, the clusters and
    // the shadow maps 4 and 17..19), 49 the sun's cascades without depth
    // compare (soft sun shadows' blocker search), 50 the reflection probe
    // and 51 the planar reflection (Reflections).
    // 63..66 the volumes (volume.frag): the depth (bound with or without
    // the prepass), NanoVDB grids, GpuVolumes and the half resolution march.
    constexpr VkShaderStageFlags kLighting = kFragment | VK_SHADER_STAGE_COMPUTE_BIT;
    // Ray tracing stages read the materials, lights, frame and dome too.
    const VkShaderStageFlags kRt = _dev.rayTracing ? VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                                                         VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR
                                                   : 0;
    // Displacement (displace.glsl) reads textures, the frame and the UDIM
    // table in the vertex stage too.
    constexpr VkShaderStageFlags kVertex = VK_SHADER_STAGE_VERTEX_BIT;
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures,
         kFragment | kRt | kVertex | VK_SHADER_STAGE_COMPUTE_BIT},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment | kRt},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kLighting | kRt},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kLighting | kRt | kVertex},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kLighting},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment | kRt},
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry | kFragment | kRt},  // kRt: rt_refraction.rgen
        {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry | kFragment | kRt},
        {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment | kRt},
        {14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kLighting},
        {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kLighting},
        {18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kLighting},
        {19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kLighting},
        {20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        {25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment | kRt | kVertex | VK_SHADER_STAGE_COMPUTE_BIT},
        {26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
        {27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kGeometry},
        // Also displacement programs (displace.glsl) and traced refraction's
        // hits (rt_refraction.rgen).
        {28, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment | kVertex | kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        {29, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment | kVertex | kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        {30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment | kVertex | kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        {31, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment | kVertex | kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        {32, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        // Deformation blur: point velocities (Scene::Gpu::velocities).
        {33, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
        // Ray tracing: TLAS, traced shadows and reflections (storage
        // images), vertex and index pools, mesh slots, probe atlases, uniforms.
        {52, _dev.rayTracing ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         _dev.rayTracing ? 1u : 0u, kRt},
        {53, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kFragment | kRt},
        {54, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kFragment | kRt},
        {55, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        {56, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        {57, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        {58, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, kFragment | kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        {59, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kFragment | kRt | VK_SHADER_STAGE_COMPUTE_BIT},
        // Traced refraction (RayTracing::Work::refraction), bound while on.
        {86, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kFragment | kRt},
        {60, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT},
        // Translucency (Transparency): the refraction background, the
        // translucent shadows' tint maps.
        {42, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {43, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {44, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {45, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {46, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        // Volumetric fog's volumes, the sun's raw depths, the reflection
        // probe, the planar reflection.
        {47, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3, VK_SHADER_STAGE_COMPUTE_BIT},
        {48, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {49, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment | VK_SHADER_STAGE_COMPUTE_BIT},
        {50, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Reflections::kMaxProbes, kFragment},
        {51, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Scene::Gpu::kMaxMirrors, kFragment},
        {63, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {64, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment | kRt},  // traced shadows march the volumes too
        {65, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kFragment | kRt},
        {66, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        {68, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment},
        // Displacement per material, levels of detail per mesh slot and the
        // camera's level per instance slot (cull.comp).
        {39, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kVertex | kFragment | VK_SHADER_STAGE_COMPUTE_BIT | kRt},
        {40, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        {41, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
        // The cascades' nearest depths (soft sun shadows): read minimum
        // filtered, written per level by shadow_min.comp.
        {61, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kFragment | VK_SHADER_STAGE_COMPUTE_BIT},
        {62, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxShadowMinLevels, VK_SHADER_STAGE_COMPUTE_BIT},
        // Blue noise for the sample dimensions (frame.glsl's SampleNoise).
        {90, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kLighting | kRt},
        // Light splits (aov.h), stored by the shading pass.
        {67, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kFragment},
        // Motion blur time samples (shutter.comp): last frame's rigid rows,
        // joints and blend shape weights.
        {69, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT},
        {70, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT},
        {71, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT},
        // Denoised SSR (SsrDenoise): what mesh.frag traces for it, bound
        // once it is on (partially bound, last).
        {72, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kFragment},
        {73, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kFragment},
        {74, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kFragment},
        {75, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kFragment},
    };
    constexpr uint32_t kBindings = uint32_t(std::size(bindings));
    VkDescriptorBindingFlags bindingFlags[kBindings] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
    std::fill(bindingFlags + 33, bindingFlags + 43, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);  // 33, 52..59, 86
    std::fill(bindingFlags + kBindings - 4, bindingFlags + kBindings, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = kBindings;
    flags.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags};
    slci.bindingCount = kBindings;
    slci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(_dev.device, &slci, nullptr, &_setLayout),
          "vkCreateDescriptorSetLayout");

    // The pool holds exactly what the layout's bindings add up to, so a new
    // binding can't leave it short.
    std::map<VkDescriptorType, uint32_t> counts;
    for (const VkDescriptorSetLayoutBinding &binding : bindings) {
        counts[binding.descriptorType] += binding.descriptorCount;
    }
    std::vector<VkDescriptorPoolSize> sizes;
    for (const auto &[type, count] : counts) {
        if (count > 0) {
            sizes.push_back({type, count});
        }
    }
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = uint32_t(sizes.size());
    dpci.pPoolSizes = sizes.data();
    Check(vkCreateDescriptorPool(_dev.device, &dpci, nullptr, &_descriptorPool),
          "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = _descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &_setLayout;
    Check(vkAllocateDescriptorSets(_dev.device, &dsai, &_set), "vkAllocateDescriptorSets");
}

// `displaced`: only the mesh pipelines of displaced materials (variant
// bit 5), made once a scene has some, so others never compile them.
// Without `shading` the depth, compute and sky pipelines; with it only the
// shading pipelines specialized with `features`, into its arrays (reads no
// other member: Frame::pipelinePrewarm's worker calls it too). Each pass's
// variants compile in parallel, through `cache` (else the device's).
void Renderer::_CreatePipelines(bool displaced, uint32_t features, const ShadingTargets *shading,
                                VkPipelineCache cache)
{
    const VkDevice device = _dev.device;
    const bool first = !displaced && !shading;
    cache = cache ? cache : _dev.pipelineCache;

    // One range for both users (vertex: mat4 viewProj; cull: CullParams), so
    // every push names the same stages and the ranges never partly overlap.
    VkPushConstantRange range{kPushStages, 0, sizeof(CullParams)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &_setLayout;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &range;
    if (first) {
        Check(vkCreatePipelineLayout(_dev.device, &lci, nullptr, &_layout), "vkCreatePipelineLayout");
    }
    for (auto [code, bytes, pipeline] : {std::tuple{kCullComp, sizeof(kCullComp), &_cullPipeline},
                                         std::tuple{kLightsComp, sizeof(kLightsComp), &_lightsPipeline},
                                         std::tuple{kSkinComp, sizeof(kSkinComp), &_skinPipeline},
                                         std::tuple{kShutterComp, sizeof(kShutterComp), &_shutterPipeline},
                                         std::tuple{kShadowMinComp, sizeof(kShadowMinComp), &_shadowMinPipeline}}) {
        if (!first) {
            break;
        }
        VkShaderModule module = CreateShaderModule(device, code, bytes);
        VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
        cci.layout = _layout;
        const VkResult r = vkCreateComputePipelines(device, _dev.pipelineCache, 1, &cci, nullptr, pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        Check(r, "vkCreateComputePipelines");
    }
    VkShaderModule meshVert = CreateShaderModule(device, kMeshVert, sizeof(kMeshVert));
    VkShaderModule depthVert = CreateShaderModule(device, kMeshDepthVert, sizeof(kMeshDepthVert));
    VkShaderModule meshFrag = CreateShaderModule(device, kMeshFrag, sizeof(kMeshFrag));
    VkShaderModule graphVert = CreateShaderModule(device, kMeshGraphVert, sizeof(kMeshGraphVert));
    VkShaderModule graphFrag = CreateShaderModule(device, kMeshGraphFrag, sizeof(kMeshGraphFrag));
    VkShaderModule layersFrag = CreateShaderModule(device, kMeshLayersFrag, sizeof(kMeshLayersFrag));
    VkShaderModule graphLayersFrag = CreateShaderModule(device, kMeshGraphLayersFrag, sizeof(kMeshGraphLayersFrag));
    VkShaderModule maskFrag = CreateShaderModule(device, kMaskFrag, sizeof(kMaskFrag));
    VkShaderModule tintFrag = CreateShaderModule(device, kShadowTintFrag, sizeof(kShadowTintFrag));
    VkShaderModule matteFrag = CreateShaderModule(device, kMatteFrag, sizeof(kMatteFrag));
    VkShaderModule matteGraphFrag = CreateShaderModule(device, kMatteGraphFrag, sizeof(kMatteGraphFrag));
    VkShaderModule skyVert = CreateShaderModule(device, kFullscreenVert, sizeof(kFullscreenVert));
    VkShaderModule skyFrag = CreateShaderModule(device, kSkyFrag, sizeof(kSkyFrag));
    auto stage = [](VkShaderStageFlagBits s, VkShaderModule m) {
        return VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                               nullptr, 0, s, m, "main"};
    };

    // Per-instance data comes from SSBOs (scene.glsl), not vertex attributes.
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {3, 0, VK_FORMAT_R16G16B16A16_SFLOAT, offsetof(Vertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo meshInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    meshInput.vertexBindingDescriptionCount = 1;
    meshInput.pVertexBindingDescriptions = &binding;
    meshInput.vertexAttributeDescriptionCount = 4;
    meshInput.pVertexAttributeDescriptions = attributes;
    VkPipelineVertexInputStateCreateInfo noInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;  // ponytail: all double-sided; honor doubleSided later
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Prepass and shadows write depth; shading (and the sky) test against
    // the finished prepass depth, and write it only without a prepass
    // (dynamic depth write, Frame::depthPrepass).
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineDepthStencilStateCreateInfo depthReadOnly = depth;
    depthReadOnly.depthWriteEnable = VK_FALSE;
    depthReadOnly.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // Attachments: color, primId, instanceId, velocity. Identical blend
    // states, so no independentBlend feature is needed (the sky writes -1 ids
    // and no velocity).
    VkPipelineColorBlendAttachmentState attachments[4] = {};
    for (VkPipelineColorBlendAttachmentState &a : attachments) {
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 4;
    blend.pAttachments = attachments;
    const VkFormat colorFormats[4] = {kColorFormat, kIdFormat, kIdFormat, kVelocityFormat};
    VkDynamicState dynamicStates[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                       VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    const VkDynamicState shadingStates[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE};
    VkPipelineDynamicStateCreateInfo shadingDynamic = dynamic;
    shadingDynamic.dynamicStateCount = 3;
    shadingDynamic.pDynamicStates = shadingStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 4;
    rendering.pColorAttachmentFormats = colorFormats;
    rendering.depthAttachmentFormat = kDepthFormat;

    VkPipelineShaderStageCreateInfo stages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, meshVert),
                                                 stage(VK_SHADER_STAGE_FRAGMENT_BIT, meshFrag)};
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &meshInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depthReadOnly;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &shadingDynamic;
    ci.layout = _layout;
    // Every mesh pipeline four times, its vertex stage specialized with
    // kWideSkin (variant bit 0: eight skinning influences) and
    // kDualQuaternion (bit 1), see mesh.vert; the shading ones four more
    // with the GRAPH shaders (bit 2: material programs) and four more of
    // those with noise (bit 3: program.glsl's kProcedurals); all of those
    // again with the LAYERS fragment shaders (bit 4: lobes, brdf.glsl); all
    // of them again for displaced materials (bit 5: displace.glsl's
    // kDisplacement in both stages), made by _CreatePipelines(true).
    const VkSpecializationMapEntry vertexEntries[3] = {
        {0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}, {10, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
    // The fragment constants: transparency.glsl's kShadingPass (`pass`: 1
    // OIT, 2 subsurface), program.glsl's kProcedurals, kDisplacement,
    // frame.glsl's kCompiledFeatures (the shading set's; others all).
    const VkSpecializationMapEntry fragmentEntries[4] = {{12, 0, sizeof(uint32_t)},
                                                         {8, sizeof(uint32_t), sizeof(VkBool32)},
                                                         {10, 2 * sizeof(uint32_t), sizeof(VkBool32)},
                                                         {13, 3 * sizeof(uint32_t), sizeof(uint32_t)}};
    // Pipelines are queued, then compiled on as many threads (drivers
    // compile each on one): a shading set's four passes at once.
    struct Job {
        VkBool32 vertexData[3];
        uint32_t fragmentData[4];
        VkSpecializationInfo vertexSpec, fragmentSpec;
        VkPipelineShaderStageCreateInfo stages[2];
        VkGraphicsPipelineCreateInfo create;
        VkPipeline *out;
    };
    std::vector<Job> jobs;
    jobs.reserve(5 * kVariants);  // no reallocation: the infos point into the jobs
    auto queueMesh = [&](const VkGraphicsPipelineCreateInfo &info, VkPipeline *out, uint32_t variants,
                         uint32_t pass = 0, VkShaderModule programFrag = VK_NULL_HANDLE) {
        for (uint32_t v = 0; v < kVariants; ++v) {
            if ((v & ~variants) || ((v & 8) && !(v & 4)) || bool(v & 32) != displaced) {
                continue;  // not this pass's or call's; noise comes with a program
            }
            Job &j = jobs.emplace_back();
            j = Job{{v & 1 ? VK_TRUE : VK_FALSE, v & 2 ? VK_TRUE : VK_FALSE, v & 32 ? VK_TRUE : VK_FALSE},
                    {pass, v & 8 ? VK_TRUE : VK_FALSE, v & 32 ? VK_TRUE : VK_FALSE, features}};
            j.vertexSpec = {3, vertexEntries, sizeof(j.vertexData), j.vertexData};
            j.fragmentSpec = {4, fragmentEntries, sizeof(j.fragmentData), j.fragmentData};
            std::copy(info.pStages, info.pStages + info.stageCount, j.stages);
            j.stages[0].pSpecializationInfo = &j.vertexSpec;  // the vertex stage comes first
            j.stages[1].pSpecializationInfo = &j.fragmentSpec;
            if (programFrag && (v & 4)) {  // a program's pass with a fragment stage of its own (matte)
                j.stages[0].module = graphVert;
                j.stages[1].module = programFrag;
            } else if (v & (4 | 16)) {  // shading pipelines only
                const bool program = v & 4;
                const VkShaderModule frags[4] = {meshFrag, graphFrag, layersFrag, graphLayersFrag};
                j.stages[0].module = program ? graphVert : meshVert;
                j.stages[1].module = frags[(program ? 1 : 0) + (v & 16 ? 2 : 0)];
            }
            j.create = info;
            j.create.pStages = j.stages;
            j.out = &out[v];
        }
    };
    auto compile = [&] {
        std::atomic<size_t> next{0};
        std::atomic<VkResult> res{VK_SUCCESS};
        auto work = [&] {
            for (size_t i; (i = next++) < jobs.size() && !_prewarmStop;) {
                const VkResult r = vkCreateGraphicsPipelines(device, cache, 1, &jobs[i].create, nullptr, jobs[i].out);
                if (r != VK_SUCCESS) {
                    res = r;
                }
            }
        };
        // The prewarm leaves half the cores to the frames meanwhile.
        const size_t cores = std::max(std::thread::hardware_concurrency(), 2u) / (cache == _prewarmCache ? 2 : 1);
        std::vector<std::thread> threads(std::clamp<size_t>(cores, 1, std::max<size_t>(jobs.size(), 1)) - 1);
        for (std::thread &t : threads) {
            t = std::thread(work);
        }
        work();
        for (std::thread &t : threads) {
            t.join();
        }
        jobs.clear();
        return res.load();
    };
    auto createMesh = [&](const VkGraphicsPipelineCreateInfo &info, VkPipeline *out, uint32_t variants,
                          VkShaderModule programFrag = VK_NULL_HANDLE) {
        queueMesh(info, out, variants, 0, programFrag);
        return compile();
    };
    VkResult r = VK_SUCCESS;
    if (shading) {
        queueMesh(ci, shading->opaque, kVariants - 1);

        // Translucent: premultiplied blending over the opaque colour (and
        // its coverage alpha); ids are written (picking), motion vectors
        // stay the opaque surface's.
        VkPipelineColorBlendAttachmentState over[4] = {attachments[0], attachments[1], attachments[2],
                                                       attachments[3]};
        over[0].blendEnable = VK_TRUE;
        over[0].srcColorBlendFactor = over[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        over[0].dstColorBlendFactor = over[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        over[0].colorBlendOp = over[0].alphaBlendOp = VK_BLEND_OP_ADD;
        over[3].colorWriteMask = 0;
        VkPipelineColorBlendStateCreateInfo overBlend = blend;
        overBlend.pAttachments = over;
        VkGraphicsPipelineCreateInfo tci = ci;
        tci.pColorBlendState = &overBlend;
        queueMesh(tci, shading->translucent, kVariants - 1);
        // Refracting meshes' back faces, seen from inside: blended the same.
        queueMesh(tci, shading->refractBack, kVariants - 1, 3);

        // Weighted blended OIT (Transparency): colour and coverage summed,
        // ids as above, location 3 the revealage, multiplied by 1 - coverage.
        VkPipelineColorBlendAttachmentState oit[4] = {attachments[0], attachments[1], attachments[2],
                                                      attachments[3]};
        oit[0].blendEnable = VK_TRUE;
        oit[0].srcColorBlendFactor = oit[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        oit[0].dstColorBlendFactor = oit[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        oit[3].blendEnable = VK_TRUE;
        oit[3].srcColorBlendFactor = oit[3].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        oit[3].dstColorBlendFactor = oit[3].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        VkPipelineColorBlendStateCreateInfo oitBlend = blend;
        oitBlend.pAttachments = oit;
        const VkFormat oitFormats[4] = {Transparency::kAccumFormat, kIdFormat, kIdFormat,
                                        Transparency::kRevealageFormat};
        VkPipelineRenderingCreateInfo oitRendering = rendering;
        oitRendering.pColorAttachmentFormats = oitFormats;
        VkGraphicsPipelineCreateInfo oci = ci;
        oci.pNext = &oitRendering;
        oci.pColorBlendState = &oitBlend;
        queueMesh(oci, shading->oit, kVariants - 1, 1);

        // Subsurface pass (Subsurface): the subsurface lobe's light and the
        // radii (locations 0 and 1), nothing else written.
        VkPipelineColorBlendAttachmentState sss[4] = {attachments[0], attachments[1], attachments[2],
                                                      attachments[3]};
        sss[2].colorWriteMask = sss[3].colorWriteMask = 0;
        VkPipelineColorBlendStateCreateInfo sssBlend = blend;
        sssBlend.pAttachments = sss;
        const VkFormat sssFormats[4] = {Subsurface::kLightFormat, Subsurface::kIdFormat, kIdFormat, kVelocityFormat};
        VkPipelineRenderingCreateInfo sssRendering = rendering;
        sssRendering.pColorAttachmentFormats = sssFormats;
        VkGraphicsPipelineCreateInfo sci = ci;
        sci.pNext = &sssRendering;
        sci.pColorBlendState = &sssBlend;
        queueMesh(sci, shading->subsurface, kVariants - 1, 2);
        r = compile();
    }

    // Depth prepass: depth only, with mesh.vert's DEPTH_ONLY variant (same
    // positions, invariant). Shadow: plus slope-scaled bias.
    if (!shading && r == VK_SUCCESS) {
        const VkPipelineShaderStageCreateInfo depthStages[1] = {stage(VK_SHADER_STAGE_VERTEX_BIT, depthVert)};
        VkPipelineRenderingCreateInfo depthRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        depthRendering.depthAttachmentFormat = kDepthFormat;
        VkPipelineColorBlendStateCreateInfo noBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo pci = ci;
        pci.pNext = &depthRendering;
        pci.stageCount = 1;
        pci.pStages = depthStages;
        pci.pDepthStencilState = &depth;
        pci.pColorBlendState = &noBlend;
        pci.pDynamicState = &dynamic;
        r = createMesh(pci, _prepassPipeline, kDepthVariants);

        VkPipelineRasterizationStateCreateInfo shadowRaster = raster;
        shadowRaster.depthBiasEnable = VK_TRUE;
        VkPipelineDynamicStateCreateInfo shadowDynamic = dynamic;
        shadowDynamic.dynamicStateCount = 3;
        VkGraphicsPipelineCreateInfo sci = pci;
        sci.pRasterizationState = &shadowRaster;
        sci.pDynamicState = &shadowDynamic;
        if (r == VK_SUCCESS) {
            r = createMesh(sci, _shadowPipeline, kDepthVariants);
        }

        // Cutouts: the same two with the alpha test.
        const VkPipelineShaderStageCreateInfo maskStages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, depthVert),
                                                               stage(VK_SHADER_STAGE_FRAGMENT_BIT, maskFrag)};
        pci.stageCount = sci.stageCount = 2;
        pci.pStages = sci.pStages = maskStages;
        if (r == VK_SUCCESS) {
            r = createMesh(pci, _prepassCutoutPipeline, kDepthVariants);
        }
        if (r == VK_SUCCESS) {
            r = createMesh(sci, _shadowCutoutPipeline, kDepthVariants);
        }

        // Translucent shadow casters (Transparency's tint maps): depth like
        // the shadows, colour masked; then no depth test, colour
        // multiplied.
        const VkPipelineShaderStageCreateInfo tintStages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, depthVert),
                                                               stage(VK_SHADER_STAGE_FRAGMENT_BIT, tintFrag)};
        const VkFormat tintFormat = Transparency::kTintFormat;
        VkPipelineRenderingCreateInfo tintRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        tintRendering.colorAttachmentCount = 1;
        tintRendering.pColorAttachmentFormats = &tintFormat;
        tintRendering.depthAttachmentFormat = Transparency::kTintDepthFormat;
        VkPipelineColorBlendAttachmentState tintAttachment{};
        VkPipelineColorBlendStateCreateInfo tintBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        tintBlend.attachmentCount = 1;
        tintBlend.pAttachments = &tintAttachment;
        VkGraphicsPipelineCreateInfo tci = sci;
        tci.pNext = &tintRendering;
        tci.pStages = tintStages;
        tci.pColorBlendState = &tintBlend;
        if (r == VK_SUCCESS) {
            r = createMesh(tci, _tintDepthPipeline, kDepthVariants);
        }
        tintAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        tintAttachment.blendEnable = VK_TRUE;
        tintAttachment.srcColorBlendFactor = tintAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        tintAttachment.dstColorBlendFactor = tintAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        VkPipelineDepthStencilStateCreateInfo noDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        tci.pDepthStencilState = &noDepth;
        if (r == VK_SUCCESS) {
            r = createMesh(tci, _tintColorPipeline, kDepthVariants);
        }

        // Translucent holdouts' matte (matte.frag): the colour and its
        // alpha times 1 - their opacity, tested against the opaque depth.
        const VkPipelineShaderStageCreateInfo matteStages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, depthVert),
                                                                stage(VK_SHADER_STAGE_FRAGMENT_BIT, matteFrag)};
        VkPipelineRenderingCreateInfo matteRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        matteRendering.colorAttachmentCount = 1;
        matteRendering.pColorAttachmentFormats = &kColorFormat;
        matteRendering.depthAttachmentFormat = kDepthFormat;
        VkPipelineColorBlendAttachmentState matteAttachment = attachments[0];
        matteAttachment.blendEnable = VK_TRUE;
        matteAttachment.srcColorBlendFactor = matteAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        matteAttachment.dstColorBlendFactor = matteAttachment.dstAlphaBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkPipelineColorBlendStateCreateInfo matteBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        matteBlend.attachmentCount = 1;
        matteBlend.pAttachments = &matteAttachment;
        VkGraphicsPipelineCreateInfo mci = pci;
        mci.pNext = &matteRendering;
        mci.stageCount = 2;
        mci.pStages = matteStages;
        mci.pDepthStencilState = &depthReadOnly;
        mci.pColorBlendState = &matteBlend;
        if (r == VK_SUCCESS) {
            // Materials with a program (bit 4, with noise bit 8) run it for their opacity.
            r = createMesh(mci, _mattePipeline, kDepthVariants | 4 | 8, matteGraphFrag);
        }
    }

    // Sky: full-screen triangle at depth 1, drawn after geometry.
    if (r == VK_SUCCESS && first) {
        VkPipelineShaderStageCreateInfo skyStages[2] = {stage(VK_SHADER_STAGE_VERTEX_BIT, skyVert),
                                                        stage(VK_SHADER_STAGE_FRAGMENT_BIT, skyFrag)};
        VkGraphicsPipelineCreateInfo kci = ci;
        kci.pStages = skyStages;
        kci.pVertexInputState = &noInput;
        r = vkCreateGraphicsPipelines(device, cache, 1, &kci, nullptr, &_skyPipeline);
    }

    for (VkShaderModule m : {meshVert, depthVert, meshFrag, graphVert, graphFrag, layersFrag, graphLayersFrag,
                             maskFrag, skyVert, skyFrag, tintFrag, matteFrag, matteGraphFrag}) {
        vkDestroyShaderModule(device, m, nullptr);
    }
    Check(r, "vkCreateGraphicsPipelines");
}

// The set of shading pipelines specialized with `features` (optional
// frame.glsl features), made when first asked for.
// ponytail: past kShadingSets - 1 sets every new mask shares one with all
// of them; evict unused sets if sessions switch features that often.
uint32_t Renderer::_ShadingSet(uint32_t features)
{
    auto it = std::find(_shadingSets.begin(), _shadingSets.end(), features);
    if (it == _shadingSets.end() && _shadingSets.size() + 1 >= kShadingSets) {
        features = kOptionalFeatures;
        it = std::find(_shadingSets.begin(), _shadingSets.end(), features);
    }
    if (it != _shadingSets.end()) {
        return uint32_t(it - _shadingSets.begin());
    }
    _shadingSets.push_back(features);
    const uint32_t set = uint32_t(_shadingSets.size() - 1);
    const ShadingTargets targets = _SetTargets(set);
    // The prewarm's, if it has them (or is making them); else made here.
    bool prewarmed = false, prewarmedDisplaced = false;
    {
        std::unique_lock<std::mutex> lock(_prewarmMutex);
        auto queued = std::find_if(_prewarmQueue.begin(), _prewarmQueue.end(),
                                   [&](const PrewarmJob &j) { return j.features == features; });
        if (queued != _prewarmQueue.end()) {
            _prewarmQueue.erase(queued);
        }
        _prewarmChanged.wait(lock, [&] { return _prewarmCurrent != features; });
        auto done = std::find_if(_prewarmed.begin(), _prewarmed.end(),
                                 [&](const Prewarmed &p) { return p.job.features == features; });
        if (done != _prewarmed.end()) {
            VkPipeline *const out[5] = {targets.opaque, targets.translucent, targets.oit, targets.subsurface,
                                        targets.refractBack};
            for (uint32_t pass = 0; pass < 5; ++pass) {
                std::copy_n(done->pipelines.data() + pass * kVariants, kVariants, out[pass]);
            }
            prewarmed = true;
            prewarmedDisplaced = done->job.displaced;
            _prewarmed.erase(done);
            ++_stats.prewarmedSets;
        }
    }
    if (!prewarmed) {
        _CreatePipelines(false, features, &targets);
    }
    if (_displacedPipelines && !prewarmedDisplaced) {
        _CreatePipelines(true, features, &targets);
    }
    if (_savePipelines && !prewarmed) {
        _dev.SavePipelineCache();  // the next run starts with them
    }
    return set;
}

// Frame::pipelinePrewarm: queues the shading sets of `features` with one
// optional feature toggled that this scene can use, and starts the worker.
// ponytail: single toggles, then pairs of gi/ssgi/fog/refraction/translucent
// shadows; other pairs wait for the second's set, which the frame after the
// first queues.
void Renderer::_Prewarm(uint32_t features, const Scene::Gpu &gpu, bool save)
{
    _MergePrewarmCache();
    if (features == _prewarmFor) {
        return;
    }
    _prewarmFor = features;
    // Settings switch these on; scene-made ones (refraction, translucent
    // shadows, subsurface, holdouts) only off, below. GI and SSGI first.
    uint32_t toggles = kFeatureSsgi | kFeatureFog | kFeatureProbe | kFeaturePlanar | kFeatureToon | features;
    toggles |= _rt ? kFeatureGi | kFeatureRtShadows | kFeatureRtReflections : 0;
    toggles |= gpu.shadowLight != kNone && gpu.shadowAngle > 0 ? kFeatureSoftSun : 0;
    // What to flip: GI, SSGI and both (often turned on together) first.
    std::vector<uint32_t> order;
    for (uint32_t bits : {kFeatureGi, kFeatureSsgi, kFeatureGi | kFeatureSsgi}) {
        if ((toggles & bits) == bits) {
            order.push_back(bits);
        }
    }
    for (uint32_t bit = 1; bit; bit <<= 1) {
        if ((toggles & bit) && bit != kFeatureGi && bit != kFeatureSsgi) {
            order.push_back(bit);
        }
    }
    // Then the pairs a VFX artist switches together (GI+SSGI is above);
    // volumes are a pass, not a shading feature.
    constexpr uint32_t kPairable = kFeatureGi | kFeatureSsgi | kFeatureFog | kFeatureRefraction |
                                   kFeatureTranslucentShadows;
    for (uint32_t a = kPairable; a; a &= a - 1) {
        for (uint32_t b = a & (a - 1); b; b &= b - 1) {
            const uint32_t bits = (a & -a) | (b & -b);
            if ((toggles & bits) == bits && bits != (kFeatureGi | kFeatureSsgi)) {
                order.push_back(bits);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(_prewarmMutex);
        _prewarmSave = save;
        for (uint32_t bits : order) {
            const uint32_t next = features ^ bits;
            const bool known = std::find(_shadingSets.begin(), _shadingSets.end(), next) != _shadingSets.end() ||
                               next == _prewarmCurrent ||
                               std::any_of(_prewarmQueue.begin(), _prewarmQueue.end(),
                                           [&](const PrewarmJob &j) { return j.features == next; }) ||
                               std::any_of(_prewarmed.begin(), _prewarmed.end(),
                                           [&](const Prewarmed &p) { return p.job.features == next; });
            if (!known && _shadingSets.size() + _prewarmQueue.size() + _prewarmed.size() + 1 < kShadingSets) {
                _prewarmQueue.push_back({next, _displacedPipelines});
            }
        }
    }
    if (!_prewarmThread.joinable() && !_prewarmQueue.empty()) {
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        Check(vkCreatePipelineCache(_dev.device, &ci, nullptr, &_prewarmCache), "vkCreatePipelineCache");
        _prewarmThread = std::thread(&Renderer::_PrewarmWorker, this);
    }
    _prewarmChanged.notify_all();
}

void Renderer::_PrewarmWorker()
{
    std::unique_lock<std::mutex> lock(_prewarmMutex);
    for (;;) {
        _prewarmChanged.wait(lock, [&] { return _prewarmStop || !_prewarmQueue.empty(); });
        if (_prewarmStop) {
            return;
        }
        Prewarmed p{_prewarmQueue.front(), std::vector<VkPipeline>(5 * kVariants, VK_NULL_HANDLE)};
        _prewarmQueue.erase(_prewarmQueue.begin());
        _prewarmCurrent = p.job.features;
        lock.unlock();
        VkPipeline *base = p.pipelines.data();
        const ShadingTargets targets{base, base + kVariants, base + 2 * kVariants, base + 3 * kVariants,
                                     base + 4 * kVariants};
        bool ok = true;
        try {
            _CreatePipelines(false, p.job.features, &targets, _prewarmCache);
            if (p.job.displaced) {
                _CreatePipelines(true, p.job.features, &targets, _prewarmCache);
            }
        } catch (const std::exception &) {
            ok = false;  // the frame that needs it compiles it (and reports why)
        }
        lock.lock();
        _prewarmMerge = true;  // the render thread merges (_MergePrewarmCache)
        _prewarmCurrent = kNone;
        if (ok && !_prewarmStop) {
            _prewarmed.push_back(std::move(p));
        } else {
            for (VkPipeline pipeline : p.pipelines) {
                vkDestroyPipeline(_dev.device, pipeline, nullptr);
            }
        }
        _prewarmChanged.notify_all();
    }
}

// Merges what the worker compiled into the device's cache. Only the render
// thread (and the destructor, after the join) does: vkMergePipelineCaches needs
// its destination externally synchronized, and the render thread creates
// pipelines (and saves) with that cache, so the worker merging into it raced.
void Renderer::_MergePrewarmCache()
{
    bool save;
    {
        std::lock_guard<std::mutex> lock(_prewarmMutex);
        if (!_prewarmMerge || !_dev.pipelineCache) {
            return;
        }
        _prewarmMerge = false;
        save = _prewarmSave;
    }
    if (vkMergePipelineCaches(_dev.device, _dev.pipelineCache, 1, &_prewarmCache) == VK_SUCCESS && save) {
        _dev.SavePipelineCache();
    }
}

void Renderer::WaitForPrewarm()
{
    std::unique_lock<std::mutex> lock(_prewarmMutex);
    _prewarmChanged.wait(lock, [&] { return _prewarmQueue.empty() && _prewarmCurrent == kNone; });
}

// The sun's cascades, `size` texels square; Render makes them (again) when
// Frame::shadowResolution changes. The GPU is idle there.
void Renderer::_CreateShadowMap(uint32_t size)
{
    for (VkImageView &v : _shadowLayers) {
        vkDestroyImageView(_dev.device, v, nullptr);
        v = VK_NULL_HANDLE;
    }
    for (VkImageView v : _shadowMinLevels) {
        vkDestroyImageView(_dev.device, v, nullptr);
    }
    _shadowMinLevels.clear();
    _dev.Destroy(_shadowMin);
    const uint32_t minSize = (size + 3) / 4;
    uint32_t minLevels = 1;
    while (minSize >> minLevels) {
        ++minLevels;
    }
    _shadowMin = _dev.CreateImage(VK_FORMAT_R32_SFLOAT, minSize, minSize, kCascades, minLevels,
                                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                  VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    for (uint32_t level = 0; level < minLevels; ++level) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = _shadowMin.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.format = VK_FORMAT_R32_SFLOAT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, kCascades};
        Check(vkCreateImageView(_dev.device, &vci, nullptr, &_shadowMinLevels.emplace_back()), "vkCreateImageView");
    }
    _dev.Submit([&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, _shadowMin.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, minLevels, 0, kCascades},
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    });
    _dev.Destroy(_shadowMap);
    _shadowMap = _dev.CreateImage(kDepthFormat, size, size, kCascades, 1,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                  VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    for (uint32_t c = 0; c < kCascades; ++c) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = _shadowMap.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kDepthFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, c, 1};
        Check(vkCreateImageView(_dev.device, &vci, nullptr, &_shadowLayers[c]), "vkCreateImageView");
    }
    _shadowSize = size;
}

// The local lights' atlas, BRDF LUT and the no-dome placeholder never change
// size.
void Renderer::_CreateConstantImages()
{
    _shadowAtlas = _dev.CreateImage(kDepthFormat, kShadowAtlasSize, kShadowAtlasSize, 1, 1,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _brdfLut = _dev.CreateImage(VK_FORMAT_R32G32_SFLOAT, kBrdfLutSize, kBrdfLutSize, 1, 1,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _blueNoise = _dev.CreateImage(VK_FORMAT_R8G8B8A8_UNORM, kBlueNoiseSize, kBlueNoiseSize, 1, 1,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _blackCube = _dev.CreateImage(VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 6, 1,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE,
                                  VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);

    const std::vector<float> lut = BuildBrdfLut(kBrdfLutSize);
    const std::vector<uint8_t> noise = BuildBlueNoise();
    Buffer staging;
    const VkDeviceSize lutBytes = lut.size() * sizeof(float);
    _dev.Reserve(staging, lutBytes + noise.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, lut.data(), lutBytes);
    std::memcpy(static_cast<uint8_t *>(staging.mapped) + lutBytes, noise.data(), noise.size());
    _dev.Submit([&](VkCommandBuffer cmd) {
        for (const Image *img : {&_brdfLut, &_blackCube, &_blueNoise}) {
            ImageBarrier(cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT, img == &_blackCube ? 6 : 1),
                         VK_PIPELINE_STAGE_2_NONE, 0,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kBrdfLutSize, kBrdfLutSize, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, _brdfLut.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.bufferOffset = lutBytes;
        copy.imageExtent = {kBlueNoiseSize, kBlueNoiseSize, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, _blueNoise.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        const VkClearColorValue black{};
        const VkImageSubresourceRange cube = Range(VK_IMAGE_ASPECT_COLOR_BIT, 6);
        vkCmdClearColorImage(cmd, _blackCube.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &cube);
        for (const Image *img : {&_brdfLut, &_blackCube, &_blueNoise}) {
            ImageBarrier(cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT, img == &_blackCube ? 6 : 1),
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });
    _dev.Destroy(staging);
    _gtao.SetBlueNoise(_blueNoise.view);
    _ssr.SetBlueNoise(_blueNoise.view);
}

// The frame's size: TAA's resolve and history, post and the readback; the
// scene's targets start at it too (_ResizeScene).
void Renderer::_Resize(uint32_t width, uint32_t height)
{
    _outWidth = width;
    _outHeight = height;
    _dev.Reserve(_readback, VkDeviceSize(width) * height * (8 + 4 + 4 + 4 + 4 + 8),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    _taa.Resize(width, height);
    _post.Resize(width, height, _taa.Output());
    _ResizeScene(width, height);
    _taaValid = false;
    _historyValid = false;
}

// The scene's targets: the frame's size, or Frame::renderScale of it
// while upscaling (TAA's history stays, at the frame's size).
void Renderer::_ResizeScene(uint32_t width, uint32_t height)
{
    for (Image *img : {&_color, &_primId, &_instanceId, &_depth, &_velocity, &_up[0], &_up[1], &_up[2], &_up[3],
                       &_filmIds[0], &_filmIds[1], &_filmIds[2]}) {
        _dev.Destroy(*img);
    }
    _width = width;
    _height = height;
    // TRANSFER_DST: the film puts its nearest sample's ids back (_filmIds).
    constexpr VkImageUsageFlags kTarget = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    _color = _dev.CreateImage(kColorFormat, width, height, 1, 1, kTarget,
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _primId = _dev.CreateImage(kIdFormat, width, height, 1, 1, kTarget,
                               VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _instanceId = _dev.CreateImage(kIdFormat, width, height, 1, 1, kTarget,
                                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _velocity = _dev.CreateImage(kVelocityFormat, width, height, 1, 1,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _depth = _dev.CreateImage(kDepthFormat, width, height, 1, 1,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
    _gtao.Resize(width, height, _depth.view);
    _taa.SetInput(width, height, _color.view, _depth.view, _velocity.view);
    _lens.Resize(width, height, _color.image, _color.view, _depth.view, _velocity.view, _taa.OutputImage(),
                 _taa.Output(), _taa.History());
    _transparency.Resize(width, height);
    _subsurface.Resize(width, height, _depth.view);
    _ssr.Resize(width, height, _depth.view);
    if (width != _outWidth || height != _outHeight) {
        const VkFormat formats[4] = {kDepthFormat, kIdFormat, kIdFormat, kVelocityFormat};
        for (int i = 0; i < 4; ++i) {
            _up[i] = _dev.CreateImage(formats[i], _outWidth, _outHeight, 1, 1,
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT,  // (a view needs one)
                                      i == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_VIEW_TYPE_2D);
        }
    }
}

// The GPU is idle here (Render waits for every frame), so rewriting the set
// in place is legal without update-after-bind.
void Renderer::_UpdateDescriptors(const Scene::Gpu &gpu, bool prepass, VkImageView background)
{
    std::vector<VkDescriptorImageInfo> images(gpu.textures.size());
    for (size_t i = 0; i < images.size(); ++i) {
        images[i] = {_samplers[gpu.textureWraps[i]], gpu.textures[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    const VkDescriptorBufferInfo materials{gpu.materials, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lights{gpu.lights, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo frame{_frameData.buffer, 0, sizeof(FrameData)};
    const VkDescriptorImageInfo shadow{_shadowSampler, _shadowMap.view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo shadowDepth{_clampSampler, _shadowMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo env{_clampSampler, gpu.environment ? gpu.environment : _blackCube.view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo lut{_clampSampler, _brdfLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo instances{gpu.instances, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo draws{gpu.draws, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo visible{gpu.visible, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo commands{_commands.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo ao{_clampSampler, _gtao.Output(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo previous{_clampSampler, _taa.History(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    // Without the prepass the main pass writes depth and nothing reads it
    // there (no SSR): the LUT stands in, so the attachment isn't also bound
    // for sampling.
    const VkDescriptorImageInfo depth = prepass
        ? VkDescriptorImageInfo{_clampSampler, _depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}
        : VkDescriptorImageInfo{_clampSampler, _brdfLut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo clusters{_clusters.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo skin{gpu.skin, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo joints{gpu.joints, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo skinXforms{gpu.skinXforms, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo jointBounds{gpu.jointBounds, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo blendOffsets{gpu.blendOffsets, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo blendWeights{gpu.blendWeights, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo udimTiles{gpu.udimTiles, 0, VK_WHOLE_SIZE};
    // Without skinned instances the joint pool may not exist yet: bind the
    // (unused) motion rows instead.
    const VkDescriptorBufferInfo prevJoints{_prevJoints.buffer ? _prevJoints.buffer : gpu.motion, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo motion{gpu.motion, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo programCode{gpu.programCode, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo programConstants{gpu.programConstants, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo primvarTable{gpu.primvarTable, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo primvarData{gpu.primvarData, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo primvarVertices{gpu.primvarVertices, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo velocities{gpu.velocities, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo displacements{gpu.displacements, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lods{gpu.lods, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo lodState{_lodState.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo prevBlendWeights{
        _prevBlendWeights.buffer ? _prevBlendWeights.buffer : gpu.blendWeights, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo atlas{_shadowSampler, _shadowAtlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo atlasDepth{_clampSampler, _shadowAtlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo lightShadows{_lightShadows.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo shadowTiles{_shadowTiles.buffer, 0, VK_WHOLE_SIZE};
    // Without refraction this frame the LUT stands in for its background.
    const VkDescriptorImageInfo refraction{_clampSampler, background ? background : _brdfLut.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const Transparency::TintMap &sunTint = _transparency.SunTint(), &atlasTint = _transparency.AtlasTint();
    const VkSampler nearest = _transparency.NearestSampler();
    const VkDescriptorImageInfo tints[4] = {
        {_clampSampler, sunTint.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest, sunTint.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {_clampSampler, atlasTint.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest, atlasTint.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkDescriptorImageInfo volumeDepth{_clampSampler, _depth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo volumeGrids{gpu.volumeGrids, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo volumes{gpu.volumes, 0, VK_WHOLE_SIZE};

    std::vector<VkWriteDescriptorSet> writes;
    auto write = [&](uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo *buffer,
                     const VkDescriptorImageInfo *image, uint32_t count = 1) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = _set;
        w.dstBinding = binding;
        w.descriptorCount = count;
        w.descriptorType = type;
        w.pBufferInfo = buffer;
        w.pImageInfo = image;
        writes.push_back(w);
    };
    write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &materials, nullptr);
    write(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lights, nullptr);
    write(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &frame, nullptr);
    write(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &shadow);
    write(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &env);
    write(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &lut);
    write(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &instances, nullptr);
    write(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &draws, nullptr);
    write(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &visible, nullptr);
    write(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &commands, nullptr);
    write(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &ao);
    write(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &previous);
    write(13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &depth);
    write(14, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &clusters, nullptr);
    write(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &skin, nullptr);
    write(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &joints, nullptr);
    write(17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &atlas);
    write(18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lightShadows, nullptr);
    write(19, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &shadowTiles, nullptr);
    write(20, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &atlasDepth);
    write(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &skinXforms, nullptr);
    write(22, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &jointBounds, nullptr);
    write(23, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &blendOffsets, nullptr);
    write(24, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &blendWeights, nullptr);
    write(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &udimTiles, nullptr);
    write(26, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &prevJoints, nullptr);
    write(27, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &motion, nullptr);
    write(28, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &programCode, nullptr);
    write(29, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &programConstants, nullptr);
    write(30, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &primvarTable, nullptr);
    write(31, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &primvarData, nullptr);
    write(32, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &primvarVertices, nullptr);
    write(33, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &velocities, nullptr);
    write(39, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &displacements, nullptr);
    write(40, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lods, nullptr);
    write(41, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &lodState, nullptr);
    write(42, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &refraction);
    for (uint32_t i = 0; i < 4; ++i) {
        write(43 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &tints[i]);
    }
    write(60, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &prevBlendWeights, nullptr);
    // Until a time sample made them, the motion rows stand in.
    const VkDescriptorBufferInfo shutter[3] = {
        {_shutterRows.buffer ? _shutterRows.buffer : gpu.motion, 0, VK_WHOLE_SIZE},
        {_shutterJoints.buffer ? _shutterJoints.buffer : gpu.motion, 0, VK_WHOLE_SIZE},
        {_shutterWeights.buffer ? _shutterWeights.buffer : gpu.motion, 0, VK_WHOLE_SIZE}};
    for (uint32_t i = 0; i < 3; ++i) {
        write(69 + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &shutter[i], nullptr);
    }
    VkDescriptorImageInfo fogStorage[3], fogVolume;
    _fog.Descriptors(fogStorage, &fogVolume, _clampSampler);
    write(47, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, fogStorage, 3);
    write(48, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &fogVolume);
    write(49, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &shadowDepth);
    const VkDescriptorImageInfo shadowMin{_minSampler ? _minSampler : _clampSampler, _shadowMin.view,
                                          VK_IMAGE_LAYOUT_GENERAL};
    write(61, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &shadowMin);
    VkDescriptorImageInfo shadowMinLevels[kMaxShadowMinLevels];
    for (uint32_t i = 0; i < kMaxShadowMinLevels; ++i) {  // levels past the last: the last again
        shadowMinLevels[i] = {VK_NULL_HANDLE, _shadowMinLevels[std::min<size_t>(i, _shadowMinLevels.size() - 1)],
                              VK_IMAGE_LAYOUT_GENERAL};
    }
    write(62, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, shadowMinLevels, kMaxShadowMinLevels);
    const VkDescriptorImageInfo splits{VK_NULL_HANDLE, _aovs.SplitsView(), VK_IMAGE_LAYOUT_GENERAL};
    write(67, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, &splits);
    // Without a probe or planar reflection: placeholders the shaders skip.
    VkDescriptorImageInfo probeInfos[Reflections::kMaxProbes];
    for (uint32_t i = 0; i < Reflections::kMaxProbes; ++i) {
        const bool probe = _reflections && i < _reflections->Probes().size();
        probeInfos[i] = {_clampSampler, probe ? _reflections->Probes()[i].cube.view : _blackCube.view,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkDescriptorImageInfo planarInfos[Scene::Gpu::kMaxMirrors];
    for (uint32_t i = 0; i < Scene::Gpu::kMaxMirrors; ++i) {
        const bool planar = _reflections && i < _reflections->Planars().size();
        planarInfos[i] = {_clampSampler, planar ? _reflections->Planars()[i].view : _brdfLut.view,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    write(50, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, probeInfos, Reflections::kMaxProbes);
    write(51, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, planarInfos, Scene::Gpu::kMaxMirrors);
    write(63, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &volumeDepth);
    write(64, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &volumeGrids, nullptr);
    write(65, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &volumes, nullptr);
    // Without half resolution volumes yet, the LUT stands in.
    const VkDescriptorImageInfo halfVolumes{_clampSampler, _volumePass->Half() ? _volumePass->Half() : _brdfLut.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    write(66, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &halfVolumes);
    const VkDescriptorImageInfo ssrImages[4] = {{VK_NULL_HANDLE, _ssr.Rays(), VK_IMAGE_LAYOUT_GENERAL},
                                                {VK_NULL_HANDLE, _ssr.Directions(), VK_IMAGE_LAYOUT_GENERAL},
                                                {VK_NULL_HANDLE, _ssr.Normals(), VK_IMAGE_LAYOUT_GENERAL},
                                                {VK_NULL_HANDLE, _ssr.Factor(), VK_IMAGE_LAYOUT_GENERAL}};
    for (uint32_t i = 0; i < 4 && _ssr.Rays(); ++i) {
        write(72 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr, &ssrImages[i]);
    }
    const VkDescriptorImageInfo translucentDepth{
        _clampSampler, _volumePass->TranslucentDepth() ? _volumePass->TranslucentDepth() : _brdfLut.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    write(68, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &translucentDepth);
    const VkDescriptorImageInfo blueNoise{_clampSampler, _blueNoise.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    write(90, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &blueNoise);
    if (!images.empty()) {
        write(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, images.data(),
              uint32_t(images.size()));
    }
    vkUpdateDescriptorSets(_dev.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
}

// Splits the camera frustum (clamped to the scene) into Frame::shadowCascades
// slices and fits a stable, texel-snapped orthographic shadow frustum to
// each. Splits past the last are -1, so Shadow() finds none there.
bool Renderer::_FitCascades(const Scene::Gpu &gpu, const Frame &frame, const float toLightDir[3],
                            FrameData *data) const
{
    if (gpu.shadowLight == kNone || !gpu.hasBounds) {
        return false;
    }
    const Mat4 invViewProj = ToMat4(frame.invViewProj);
    Vec3 nearC[4], farC[4], nearCenter, farCenter;
    for (int k = 0; k < 4; ++k) {
        const float x = (k & 1) ? 1.0f : -1.0f, y = (k & 2) ? 1.0f : -1.0f;
        nearC[k] = TransformPoint(invViewProj, {x, y, -1});
        farC[k] = TransformPoint(invViewProj, {x, y, 1});
        nearCenter = nearCenter + nearC[k] * 0.25f;
        farCenter = farCenter + farC[k] * 0.25f;
    }
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    const Vec3 forward = Normalize(farCenter - nearCenter);
    const float n = Dot(nearCenter - eye, forward), f = Dot(farCenter - eye, forward);

    Vec3 corners[8];
    float sceneFar = n;
    for (int k = 0; k < 8; ++k) {
        corners[k] = {(k & 1) ? gpu.boundsMax[0] : gpu.boundsMin[0],
                      (k & 2) ? gpu.boundsMax[1] : gpu.boundsMin[1],
                      (k & 4) ? gpu.boundsMax[2] : gpu.boundsMin[2]};
        sceneFar = std::max(sceneFar, Dot(corners[k] - eye, forward));
    }
    const float shadowFar = std::min(f, sceneFar);
    if (!(shadowFar > n) || !(f > n)) {
        return false;
    }

    const Vec3 toLight = Normalize({toLightDir[0], toLightDir[1], toLightDir[2]});
    const Mat4 lightView = LookAlong(toLight * -1.0f);
    float zMin = 1e30f, zMax = -1e30f;  // scene depth range in light view space
    for (const Vec3 &c : corners) {
        const float z = TransformPoint(lightView, c).z;
        zMin = std::min(zMin, z);
        zMax = std::max(zMax, z);
    }
    // Volume shadows: their boxes too (VolumePass::RecordShadow marches
    // the cascades' depth range).
    if (frame.volumes && frame.volume.shadows && gpu.volumeCount > 0) {
        for (int k = 0; k < 8; ++k) {
            const Vec3 c{(k & 1) ? gpu.volumeMax[0] : gpu.volumeMin[0], (k & 2) ? gpu.volumeMax[1] : gpu.volumeMin[1],
                         (k & 4) ? gpu.volumeMax[2] : gpu.volumeMin[2]};
            const float z = TransformPoint(lightView, c).z;
            zMin = std::min(zMin, z);
            zMax = std::max(zMax, z);
        }
    }
    const float margin = 0.01f * (zMax - zMin) + 1e-3f;

    const uint32_t cascades = std::clamp(frame.shadowCascades, 1u, kCascades);
    std::fill(data->cascadeSplits, data->cascadeSplits + kCascades, -1.0f);
    float splitNear = n;
    for (uint32_t c = 0; c < cascades; ++c) {
        // Practical split scheme: 75% logarithmic, 25% uniform.
        const float t = float(c + 1) / float(cascades);
        const float logSplit = n > 0 ? n * std::pow(shadowFar / n, t) : shadowFar * t;
        const float splitFar = 0.75f * logSplit + 0.25f * (n + (shadowFar - n) * t);

        Vec3 slice[8], center;
        for (int k = 0; k < 4; ++k) {
            slice[k] = nearC[k] + (farC[k] - nearC[k]) * ((splitNear - n) / (f - n));
            slice[k + 4] = nearC[k] + (farC[k] - nearC[k]) * ((splitFar - n) / (f - n));
        }
        for (const Vec3 &p : slice) {
            center = center + p * 0.125f;
        }
        float radius = 0;
        for (const Vec3 &p : slice) {
            radius = std::max(radius, Length(p - center));
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;  // quantized so it doesn't shimmer

        // Snap the center to whole shadow texels in light space.
        const float texel = 2 * radius / float(_shadowSize);
        Vec3 lc = TransformPoint(lightView, center);
        lc.x = std::floor(lc.x / texel) * texel;
        lc.y = std::floor(lc.y / texel) * texel;
        const Mat4 proj = Ortho(lc.x - radius, lc.x + radius, lc.y - radius, lc.y + radius,
                                -zMax - margin, -zMin + margin);
        const Mat4 viewProj = lightView * proj;
        std::memcpy(data->cascadeViewProj[c], viewProj.m, sizeof(viewProj.m));
        data->cascadeSplits[c] = splitFar;
        data->cascadeTexel[c] = texel;
        splitNear = splitFar;
    }
    data->cameraForward[0] = forward.x;
    data->cameraForward[1] = forward.y;
    data->cameraForward[2] = forward.z;
    return true;
}

// Depth range of the light clusters (near plane to the scene's far end,
// along the view axis) and the cutoff that bounds each light's range: 1% of
// the radiance the current exposure maps to middle gray.
void Renderer::_FitClusters(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const
{
    const Mat4 invViewProj = ToMat4(frame.invViewProj);
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    const Vec3 nearCenter = TransformPoint(invViewProj, {0, 0, -1});
    const Vec3 forward = Normalize(TransformPoint(invViewProj, {0, 0, 0}) - nearCenter);
    float n = Dot(nearCenter - eye, forward);
    float f = Dot(TransformPoint(invViewProj, {0, 0, 1}) - eye, forward);
    if (!(f < 1e30f)) {
        f = 1e30f;  // infinite far plane
    }
    if (gpu.hasBounds) {
        float sceneFar = n;
        for (int k = 0; k < 8; ++k) {
            const Vec3 c{(k & 1) ? gpu.boundsMax[0] : gpu.boundsMin[0], (k & 2) ? gpu.boundsMax[1] : gpu.boundsMin[1],
                         (k & 4) ? gpu.boundsMax[2] : gpu.boundsMin[2]};
            sceneFar = std::max(sceneFar, Dot(c - eye, forward));
        }
        f = std::min(f, sceneFar);
    }
    n = std::max(n, 1e-5f * std::max(f, 1e-3f));  // log slices need near > 0
    f = std::max(f, n * 1.01f);
    const float exposure = frame.postProcess ? _post.Exposure() : 1.0f;
    data->clusters[0] = n;
    data->clusters[1] = f;
    data->clusters[2] = std::log2(f / n);
    data->clusters[3] = 0.01f * 0.18f / exposure;
    data->cameraForward[0] = forward.x;
    data->cameraForward[1] = forward.y;
    data->cameraForward[2] = forward.z;
}

// Volumetric fog's medium, froxel grid and temporal state for this frame
// (shaders/fog.glsl); false with fog off (its history is dropped then).
// The froxels reach Frame::fogMaxDistance, or the scene's far end along the
// view (as the clusters do).
bool Renderer::_SetupFog(const Scene::Gpu &gpu, const Frame &frame, FrameData *data)
{
    if (!frame.fog || !(frame.fogDensity > 0.0f)) {
        _fog.Skip();
        return false;
    }
    const Mat4 inv = ToMat4(frame.invViewProj);
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    const Vec3 nearCenter = TransformPoint(inv, {0, 0, -1});
    const Vec3 forward = Normalize(TransformPoint(inv, {0, 0, 0}) - nearCenter);
    float depth = frame.fogMaxDistance;
    if (!(depth > 0.0f)) {
        depth = Dot(TransformPoint(inv, {0, 0, 1}) - eye, forward);
        if (!(depth < 1e30f)) {
            depth = 1e30f;  // infinite far plane
        }
        if (gpu.hasBounds) {
            float sceneFar = 0.0f;
            for (int k = 0; k < 8; ++k) {
                const Vec3 c{(k & 1) ? gpu.boundsMax[0] : gpu.boundsMin[0], (k & 2) ? gpu.boundsMax[1] : gpu.boundsMin[1],
                             (k & 4) ? gpu.boundsMax[2] : gpu.boundsMin[2]};
                sceneFar = std::max(sceneFar, Dot(c - eye, forward));
            }
            depth = std::min(depth, sceneFar);
        }
        depth = std::max(depth, 1e-3f);
    }
    const uint32_t x = std::clamp(frame.fogGridSize, 16u, 512u);
    const uint32_t y = std::max(1u, uint32_t(std::lround(float(x) * float(_height) / float(_width))));
    constexpr uint32_t kSlices = 64;
    const bool orthographic = std::abs(frame.viewProj[3]) + std::abs(frame.viewProj[7]) + std::abs(frame.viewProj[11]) < 1e-9f;
    data->features[0] |= kFeatureFog;
    const float medium[4] = {frame.fogDensity, std::max(frame.fogHeightFalloff, 0.0f), frame.fogHeight,
                             std::clamp(frame.fogAnisotropy, -0.95f, 0.95f)};
    std::copy(medium, medium + 4, data->fogMedium);
    const float albedo[4] = {frame.fogColor[0], frame.fogColor[1], frame.fogColor[2], depth};
    std::copy(albedo, albedo + 4, data->fogAlbedo);
    const uint32_t grid[4] = {x, y, kSlices, frame.fogZUp ? 1u : 0u};
    std::copy(grid, grid + 4, data->fogGrid);
    // Last frame's grid is the history while the lights and the medium
    // stay the same (moving geometry keeps it: its shadows lag a little).
    uint64_t key = gpu.lightingHash;
    for (const float v : {medium[0], medium[1], medium[2], medium[3], albedo[0], albedo[1], albedo[2]}) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        key = (key ^ bits) * 1099511628211ull;
    }
    // A film sample (Frame::fogFilmJitter): each froxel at a point of its
    // own, new per sample, and no history; the film averages them.
    const bool usable = _fog.Resize(x, y, kSlices);
    const bool history = usable && !_filmFroxels && _fogDepth > 0.0f && key == _fogKey;
    data->sampleInfo[1] = _filmFroxels ? 1 : 0;
    _fogKey = key;
    // History: 90% of last frame's grid, sampled half a slice apart on average.
    const float temporal[4] = {history ? _fog.Jitter() : 0.5f, history ? 0.9f : 0.0f, history ? _fogDepth : depth,
                               orthographic ? 1.0f : 0.0f};
    std::copy(temporal, temporal + 4, data->fogTemporal);
    data->cameraForward[0] = forward.x;
    data->cameraForward[1] = forward.y;
    data->cameraForward[2] = forward.z;
    _fogDepth = depth;
    return true;
}

// std430 mirror of ShadowTile in mesh.frag.
struct Renderer::ShadowTile {
    float viewProj[16];
    float rect[4];    // atlas uv offset, uv scale
    float params[4];  // shadow texel size per unit of distance from the light, near, far, light size
    float origin[4];  // where the light is seen from: its centre, or a point of its shape (Frame::shadowJitter)
};

// Gives atlas tiles to the shadow-casting lights whose range reaches into
// the view, brightest at the camera first, until the atlas is full; the
// rest stay unshadowed. Writes each light's first tile (or kNone).
// The first rtSlots lights by priority are ray-traced instead (written to
// rtLights; tiles only with tileTraced, from what the others leave).
// With shapeSample (three values in [0, 1), Frame::shadowJitter) each
// light is seen from that point of its shape, and looked up hard.
std::vector<Renderer::ShadowTile> Renderer::_PlanLocalShadows(const Scene::Gpu &gpu, const Frame &frame,
                                                              float cutoff, uint32_t *rtLights, uint32_t rtSlots,
                                                              bool tileTraced, const float *shapeSample)
{
    const uint32_t lights = gpu.lightCount + gpu.localLightCount;
    _dev.Reserve(_lightShadows, std::max(lights, 1u) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto *firstTile = static_cast<uint32_t *>(_lightShadows.mapped);
    std::fill(firstTile, firstTile + lights, kNone);
    std::vector<ShadowTile> tiles;

    float planes[6][4];
    FrustumPlanes(frame.viewProj, planes);
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    struct Candidate {
        const Scene::Gpu::ShadowCaster *caster;
        float range, priority;
    };
    std::vector<Candidate> candidates;
    for (const Scene::Gpu::ShadowCaster &c : gpu.shadowCasters) {
        if (!frame.localShadows || !frame.localLights) {
            break;
        }
        // Rounded up to a power of sqrt 2: the cutoff follows the exposure,
        // which drifts while it adapts, and every change of a tile's far
        // plane redraws the atlas.
        const float range = std::exp2(std::ceil(std::log2(std::sqrt(c.rangeIntensity / cutoff)) * 2.0f) * 0.5f) +
                            c.extent;
        bool inView = range > 0.0f;
        for (const auto &p : planes) {
            inView = inView && p[0] * c.position[0] + p[1] * c.position[1] + p[2] * c.position[2] + p[3] >= -range;
        }
        if (inView) {
            const Vec3 d = Vec3{c.position[0], c.position[1], c.position[2]} - eye;
            candidates.push_back({&c, range, c.rangeIntensity / std::max(Dot(d, d), 1e-12f)});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) { return a.priority > b.priority; });

    // Pick by priority, then place in light order: as long as the same lights
    // win, the tiles (and the cached atlas) stay where they were.
    // Tiles shrink (halving, down to 64) until every light that needs one fits
    // (all of them on 4096x4096 up to ~680 sphere lights at 64 texels); past
    // that the farthest or dimmest go unshadowed.
    uint32_t tileSize = std::clamp(frame.localShadowResolution, 64u, kShadowAtlasSize / 2);
    uint32_t demand = 0;
    for (size_t i = tileTraced ? 0 : std::min<size_t>(rtSlots, candidates.size()); i < candidates.size(); ++i) {
        demand += candidates[i].caster->coneAngle <= kSpotShadowCone ? 1 : 6;
    }
    while (tileSize > 64 && (kShadowAtlasSize / tileSize) * (kShadowAtlasSize / tileSize) < demand) {
        tileSize /= 2;
    }
    const uint32_t perRow = kShadowAtlasSize / tileSize, capacity = perRow * perRow;
    // With tileTraced the traced lights get tiles too, after the others (whose
    // tiles stay as without), for what the traced image misses.
    std::vector<Candidate> chosen, traced;
    uint32_t used = 0;
    auto place = [&](const Candidate &cand) {
        const uint32_t need = cand.caster->coneAngle <= kSpotShadowCone ? 1 : 6;
        if (used + need <= capacity) {
            chosen.push_back(cand);
            used += need;
        }
    };
    for (const Candidate &cand : candidates) {
        if (rtSlots > 0) {
            *rtLights++ = cand.caster->light;
            --rtSlots;
            traced.push_back(cand);
            continue;
        }
        place(cand);
    }
    if (tileTraced) {
        for (const Candidate &cand : traced) {
            place(cand);
        }
    }
    std::sort(chosen.begin(), chosen.end(),
              [](const Candidate &a, const Candidate &b) { return a.caster->light < b.caster->light; });

    const float tileUv = float(tileSize) / float(kShadowAtlasSize);
    const Vec3 cube[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Candidate &cand : chosen) {
        const Scene::Gpu::ShadowCaster &c = *cand.caster;
        const bool spot = c.coneAngle <= kSpotShadowCone;
        firstTile[c.light] = uint32_t(tiles.size());
        Vec3 position{c.position[0], c.position[1], c.position[2]};
        float n = std::max(c.radius * 1.05f, cand.range * 1e-4f);
        Vec3 offset{};  // the sample point's offset from the light's centre
        using Shape = Scene::Gpu::ShadowCaster::Shape;
        if (shapeSample && frame.softShadows) {
            // Uniform over a rect's or disk's area, a cylinder's axis; in a
            // sphere with density 1 / sqrt(1 - r^2), whose projection along
            // any direction is uniform over the disc it shows a receiver
            // (the Abel transform of (1 - r^2)^-1/2 is constant).
            // A cylinder: uniform along its axis, and across it the disc
            // of its section with density 1 / sqrt(1 - r^2) (the same
            // Abel pair, one dimension down: every chord integrates to pi).
            // A sphere's near plane clears its own radius (a cylinder's, its
            // section's) along each face axis from the offset point, no
            // more (see add()).
            const float u = shapeSample[0], v = shapeSample[1], phi = 6.2831853f * v;
            const Vec3 hw{c.halfWidth[0], c.halfWidth[1], c.halfWidth[2]};
            const Vec3 hh{c.halfHeight[0], c.halfHeight[1], c.halfHeight[2]};
            if (c.shape == Shape::Sphere) {
                // Radius sin(t): its CDF is (2t - sin 2t) / pi, inverted by Newton.
                float t = 0.7853982f;
                for (int i = 0; i < 8; ++i) {
                    const float g = (2.0f * t - std::sin(2.0f * t)) / 3.14159265f - shapeSample[2];
                    t = std::clamp(t - g / std::max((2.0f - 2.0f * std::cos(2.0f * t)) / 3.14159265f, 1e-4f), 0.0f,
                                   1.5707963f);
                }
                const float z = 1.0f - 2.0f * u, r = std::sqrt(std::max(1.0f - z * z, 0.0f));
                offset = Vec3{r * std::cos(phi), r * std::sin(phi), z} * (std::sin(t) * c.size);
            } else if (c.shape == Shape::Rect) {
                offset = hw * (2.0f * u - 1.0f) + hh * (2.0f * v - 1.0f);
            } else if (c.shape == Shape::Disk) {
                offset = (hw * std::cos(phi) + hh * std::sin(phi)) * std::sqrt(u);
            } else {
                const Vec3 a = Normalize(hw);
                const Vec3 b = Normalize(Cross(a, std::abs(a.x) < 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
                const float w = 1.0f - shapeSample[1], r = std::sqrt(std::max(1.0f - w * w, 0.0f)) * c.cylinderRadius,
                            ang = 6.2831853f * shapeSample[2];
                offset = hw * (2.0f * u - 1.0f) + b * (r * std::cos(ang)) + Cross(a, b) * (r * std::sin(ang));
            }
            position = position + offset;
        }
        const float f = std::max(cand.range, (n + c.radius) * 2.0f);
        const bool jitter = shapeSample && frame.softShadows;
        const Vec3 tubeAxis = Normalize(Vec3{c.halfWidth[0], c.halfWidth[1], c.halfWidth[2]});
        // A few texels of margin: PCF near a tile's (or cube face's) edge
        // must not read the neighbour.
        const float margin = 1.0f + 6.0f / float(tileSize);
        const float tanHalf = (spot ? std::tan(c.coneAngle) : 1.0f) * margin;
        auto add = [&](Vec3 direction) {
            ShadowTile t{};
            // The sphere reaches radius - offset.direction in front of the
            // sample point along this axis, a cylinder's section through
            // the sample point its projected radius - the radial offset's
            // share; clear that and not more.
            const float along = Dot(tubeAxis, direction);
            const float extent = c.shape == Shape::Sphere ? c.radius
                               : c.shape == Shape::Cylinder
                                   ? c.cylinderRadius * std::sqrt(std::max(1.0f - along * along, 0.0f))
                                   : -1.0f;
            const Vec3 own = c.shape == Shape::Cylinder ? offset - tubeAxis * Dot(offset, tubeAxis) : offset;
            const float nf = jitter && extent >= 0.0f
                                 ? std::max(extent * 1.05f - Dot(own, direction), cand.range * 1e-4f)
                                 : n;
            const Mat4 viewProj = Translate(position * -1.0f) * LookAlong(direction) * Perspective(tanHalf, nf, f);
            std::memcpy(t.viewProj, viewProj.m, sizeof(t.viewProj));
            const uint32_t k = uint32_t(tiles.size());
            t.rect[0] = float(k % perRow) * tileUv;
            t.rect[1] = float(k / perRow) * tileUv;
            t.rect[2] = t.rect[3] = tileUv;
            t.params[0] = 2.0f * tanHalf / float(tileSize);
            t.params[1] = nf;
            t.params[2] = f;
            t.params[3] = frame.softShadows && !shapeSample ? c.size : 0.0f;
            t.origin[0] = position.x;
            t.origin[1] = position.y;
            t.origin[2] = position.z;
            tiles.push_back(t);
        };
        if (spot) {
            add({c.axis[0], c.axis[1], c.axis[2]});
        } else {
            for (const Vec3 &d : cube) {
                add(d);  // same face order as LocalShadow() in mesh.frag
            }
        }
    }
    _dev.Reserve(_shadowTiles, std::max<size_t>(tiles.size(), 1) * sizeof(ShadowTile),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::copy(tiles.begin(), tiles.end(), static_cast<ShadowTile *>(_shadowTiles.mapped));
    return tiles;
}

// Draws the tiles one after another, each culled on the GPU against its
// own frustum (dropping casters under half a shadow texel) into the shared
// tile list, unless the atlas already holds exactly these tiles of this
// scene version (static scenes and lights: drawn once, e.g. for all of TAA's
// accumulation frames). Without tiles the atlas is only transitioned for
// its descriptor.
// ponytail: all or nothing; a moving caster redraws every light's tiles.
void Renderer::_RenderLocalShadows(const Scene::Gpu &gpu, const std::vector<ShadowTile> &tiles, bool tint,
                                   const VolumePass::Settings *volume)
{
    const VkImageSubresourceRange range = Range(VK_IMAGE_ASPECT_DEPTH_BIT);
    std::vector<float> key;
    for (const ShadowTile &t : tiles) {
        key.insert(key.end(), t.viewProj, t.viewProj + 16);
        key.insert(key.end(), t.rect, t.rect + 4);
    }
    key.push_back(tint ? float(_transparency.AtlasTint().size) : 0.0f);
    key.push_back(volume ? float(gpu.volumeVersion % (1u << 24)) + 1.0f : 0.0f);  // ponytail: a stamp wrapping at 2^24 can miss a change
    if (!tiles.empty() && gpu.version == _atlasVersion && key == _atlasKey) {
        return;  // still in SHADER_READ_ONLY_OPTIMAL from when it was drawn
    }
    _atlasVersion = tiles.empty() ? 0 : gpu.version;
    _atlasKey = std::move(key);
    if (tiles.empty()) {
        ImageBarrier(_cmd, _shadowAtlas.image, range, VK_PIPELINE_STAGE_2_NONE, 0,
                     kShadowReaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return;
    }
    constexpr VkPipelineStageFlags2 kTests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    auto memoryBarrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                             VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(_cmd, &dep);
    };
    // Clear the whole atlas once; each tile then loads and draws its rect.
    ImageBarrier(_cmd, _shadowAtlas.image, range, VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearDepthStencilValue far{1.0f, 0};
    vkCmdClearDepthStencilImage(_cmd, _shadowAtlas.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &range);
    if (tint) {
        _transparency.BeginTint(_cmd, _transparency.AtlasTint(), 1);
    }
    ImageBarrier(_cmd, _shadowAtlas.image, range, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 kTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    const VkDeviceSize commandBytes = VkDeviceSize(gpu.drawCount) * sizeof(VkDrawIndexedIndirectCommand);
    const VkDeviceSize tileCommands = (1 + kCascades) * commandBytes;
    const uint32_t tileList = (1 + kCascades) * gpu.instanceCapacity;
    const VkDeviceSize vertexOffset = 0;
    for (uint32_t tileIndex = 0; tileIndex < tiles.size(); ++tileIndex) {
        const ShadowTile &t = tiles[tileIndex];
        // The previous tile's draw is done with the list and commands.
        memoryBarrier(VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | kTests,
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | kTests,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        const VkBufferCopy reset{0, tileCommands, commandBytes};
        vkCmdCopyBuffer(_cmd, gpu.commandTemplate, _commands.buffer, 1, &reset);
        memoryBarrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        CullParams cull{};
        FrustumPlanes(t.viewProj, cull.planes);
        cull.instanceCapacity = gpu.instanceCapacity;
        cull.drawCount = gpu.drawCount;
        cull.pixelScale = 1.0f / t.params[0];  // shadow texels per unit at distance 1
        cull.nearDistance = t.params[1];
        cull.minRadius = 0.5f;
        cull.listBase = tileList;
        cull.commandBase = (1 + kCascades) * gpu.drawCount;
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(cull), &cull);
        vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);
        memoryBarrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        const int32_t x = int32_t(std::lround(t.rect[0] * kShadowAtlasSize));
        const int32_t y = int32_t(std::lround(t.rect[1] * kShadowAtlasSize));
        const uint32_t size = uint32_t(std::lround(t.rect[2] * kShadowAtlasSize));
        VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = _shadowAtlas.view;
        att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{x, y}, {size, size}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &att;
        vkCmdBeginRendering(_cmd, &ri);
        const VkViewport vp{float(x), float(y), float(size), float(size), 0, 1};
        vkCmdSetViewport(_cmd, 0, 1, &vp);
        vkCmdSetScissor(_cmd, 0, 1, &ri.renderArea);
        // GTAO bound its own set; the shadow pass needs the scene's.
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline[0]);
        vkCmdSetDepthBias(_cmd, 0.0f, 0.0f, 1.5f);
        vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
        vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
        PushDraw(_cmd, _layout, t.viewProj, tileList);
        DrawChunks(_cmd, gpu, _commands.buffer, tileCommands, _shadowPipeline, _shadowCutoutPipeline, kDepthVariants);
        vkCmdEndRendering(_cmd);
        if (tint) {
            const Transparency::TintMap &map = _transparency.AtlasTint();
            const float scale = float(map.size) / float(kShadowAtlasSize);
            const int32_t tx = int32_t(std::lround(x * scale)), ty = int32_t(std::lround(y * scale));
            const uint32_t tsize = std::max(uint32_t(std::lround(size * scale)), 1u);
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = map.color.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingAttachmentInfo depth = color;
            depth.imageView = map.depth.view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            VkRenderingInfo tri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            tri.renderArea = {{tx, ty}, {tsize, tsize}};
            tri.layerCount = 1;
            tri.colorAttachmentCount = 1;
            tri.pColorAttachments = &color;
            tri.pDepthAttachment = &depth;
            vkCmdBeginRendering(_cmd, &tri);
            const VkViewport tvp{float(tx), float(ty), float(tsize), float(tsize), 0, 1};
            vkCmdSetViewport(_cmd, 0, 1, &tvp);
            vkCmdSetScissor(_cmd, 0, 1, &tri.renderArea);
            if (volume) {
                _volumePass->RecordTileShadow(_cmd, _set, gpu.volumeCount, tileIndex, uint32_t(tx), uint32_t(ty),
                                              tsize, *volume);
                vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
                PushDraw(_cmd, _layout, t.viewProj, tileList);  // the volume pipeline's layout dropped them
            }
            _DrawTintCasters(gpu, tileCommands);
            vkCmdEndRendering(_cmd);
        }
    }
    if (tint) {
        _transparency.EndTint(_cmd, _transparency.AtlasTint(), 1);
    }
    ImageBarrier(_cmd, _shadowAtlas.image, range, kTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 kShadowReaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Translucent meshes after the opaque pass and the sky, back to front by
// their bounds' centres; they test the opaque depth but write none.
// Frame::oit sums them, weighted by distance, into the Transparency
// targets and resolves that over the colour; otherwise each is blended
// over the last. Both draw back to front, so the nearest mesh's ids win
// picking.
// ponytail: sorted per mesh, not per instance or triangle (OIT's weights
// need no order but only approximate it).
void Renderer::_RenderTranslucency(const Scene::Gpu &gpu, const Frame &frame, FrameData *data,
                                   const float *viewProj, uint32_t motion, VkRenderingAttachmentInfo depth)
{
    std::vector<std::pair<float, size_t>> order;
    for (size_t i = 0; i < gpu.translucent.size(); ++i) {
        const float *c = &gpu.translucentCenters[i * 3];
        const Vec3 d{c[0] - frame.cameraPos[0], c[1] - frame.cameraPos[1], c[2] - frame.cameraPos[2]};
        order.push_back({Dot(d, d), i});
    }
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    data->translucency[2] = 1.0f / std::max(std::sqrt(order.back().first), 1e-6f);
    data->translucency[3] = std::max(frame.oitDepthWeight, 0.0f);

    // The opaque pass's colour, ids and (without a prepass) depth are
    // written before this pass loads them.
    VkMemoryBarrier2 opaque{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    opaque.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    opaque.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    opaque.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    opaque.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &opaque;
    vkCmdPipelineBarrier2(_cmd, &dep);

    const bool refraction = data->features[0] & kFeatureRefraction;
    // With light path intensities the copy takes them out again (from the
    // light splits), so glass scales what it refracts once.
    const VkImageView unscale = data->lightPath[2] != 0.0f ? _aovs.SplitsView() : VK_NULL_HANDLE;
    auto background = [&]() {
        _transparency.RecordBackground(_cmd, _color.image, unscale, data->lightPath[0], data->lightPath[1]);
    };
    if (refraction) {
        background();
    }
    // Renders into _color (and the ids), or with `oit` into the
    // Transparency targets; the depth stays the opaque one's.
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = frame.depthPrepass ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
    const VkDeviceSize vertexOffset = 0;
    auto begin = [&](bool oit) {
        const VkImageView views[4] = {oit ? _transparency.Accum().view : _color.view, _primId.view,
                                      _instanceId.view, oit ? _transparency.Revealage().view : _velocity.view};
        VkRenderingAttachmentInfo atts[4] = {};
        for (int i = 0; i < 4; ++i) {
            atts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            atts[i].imageView = views[i];
            atts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            atts[i].loadOp = oit && (i == 0 || i == 3) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        atts[3].clearValue.color.float32[0] = 1.0f;  // revealage: nothing in front yet
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {_width, _height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 4;
        ri.pColorAttachments = atts;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(_cmd, &ri);
        const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
        vkCmdSetViewport(_cmd, 0, 1, &vp);
        vkCmdSetScissor(_cmd, 0, 1, &ri.renderArea);
        // Dynamic state does not survive other pipelines bound meanwhile
        // (the sky's, volumes'): set per rendering, before any draw.
        vkCmdSetDepthWriteEnable(_cmd, VK_FALSE);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
        PushDraw(_cmd, _layout, viewProj, 0, motion);
        vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
        vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
    };
    VkPipeline bound = VK_NULL_HANDLE;
    auto drawSlot = [&](const VkPipeline *pipelines, size_t i) {
        if (pipelines[gpu.translucentVariant[i]] != bound) {
            bound = pipelines[gpu.translucentVariant[i]];
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
        }
        vkCmdDrawIndexedIndirect(_cmd, _commands.buffer,
                                 VkDeviceSize(gpu.translucent[i]) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                 sizeof(VkDrawIndexedIndirectCommand));
    };

    // Refraction layers (Frame::refractionBackFaces, multiRefraction):
    // refracting meshes back to front, each its back faces (seen from inside
    // the medium) and then its front faces, blended over the colour before
    // the other translucent meshes. With multiRefraction the background is
    // copied again after each layer drawn (up to Frame::refractionLayers
    // copies), so glass behind glass, and a ball's far side, show through
    // the layers in front of them.
    // ponytail: a copy per layer (about 0.1 ms at 1280x720 each); depth
    // peeling would also order the triangles within a layer.
    const bool layers = refraction && (frame.refractionBackFaces || frame.multiRefraction);
    if (layers) {
        uint32_t copies = frame.multiRefraction ? std::clamp(frame.refractionLayers, 1u, 16u) : 0;
        bool drawn = false;  // since the last copy
        begin(false);
        auto copy = [&]() {
            if (copies == 0 || !drawn) {
                return;
            }
            vkCmdEndRendering(_cmd);
            background();
            begin(false);
            bound = VK_NULL_HANDLE;
            drawn = false;
            --copies;
        };
        for (const auto &[distance, i] : order) {
            if (!(gpu.translucentFlags[i] & Scene::Gpu::kRefracts)) {
                continue;
            }
            copy();
            if (frame.refractionBackFaces) {
                drawSlot(_refractBackPipeline + _shadingBase, i);
                drawn = true;
                copy();
            }
            drawSlot(_translucentPipeline + _shadingBase, i);
            drawn = true;
        }
        vkCmdEndRendering(_cmd);
        vkCmdPipelineBarrier2(_cmd, &dep);  // the colour and ids again, for the pass below
        bound = VK_NULL_HANDLE;
    }

    const bool oit = frame.oit;
    if (oit) {
        _transparency.BeginAccumulation(_cmd);
    }
    begin(oit);
    const VkPipeline *pipelines = (oit ? _oitPipeline : _translucentPipeline) + _shadingBase;
    for (const auto &[distance, i] : order) {
        if (!layers || !(gpu.translucentFlags[i] & Scene::Gpu::kRefracts)) {
            drawSlot(pipelines, i);
        }
    }
    vkCmdEndRendering(_cmd);
    if (oit) {
        _transparency.Resolve(_cmd, _color.view);
    }

    // Translucent holdouts (Frame::holdoutAlpha): the shading passes drew
    // them as nothing; their matte cuts the colour and its alpha by their
    // opacity.
    // ponytail: everything at the pixel, also what's in front of them.
    if (frame.holdouts && frame.holdoutAlpha &&
        std::any_of(gpu.translucentFlags.begin(), gpu.translucentFlags.end(),
                    [](uint8_t f) { return f & Scene::Gpu::kHoldout; })) {
        vkCmdPipelineBarrier2(_cmd, &dep);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = _color.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {_width, _height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(_cmd, &ri);
        const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
        vkCmdSetViewport(_cmd, 0, 1, &vp);
        vkCmdSetScissor(_cmd, 0, 1, &ri.renderArea);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
        PushDraw(_cmd, _layout, viewProj, 0, motion);
        vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
        vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
        bound = VK_NULL_HANDLE;
        for (const auto &[distance, i] : order) {
            if (gpu.translucentFlags[i] & Scene::Gpu::kHoldout) {
                const VkPipeline pipeline = _mattePipeline[gpu.translucentVariant[i] & (kDepthVariants | 4 | 8)];
                if (pipeline != bound) {
                    bound = pipeline;
                    vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
                }
                vkCmdDrawIndexedIndirect(_cmd, _commands.buffer,
                                         VkDeviceSize(gpu.translucent[i]) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                         sizeof(VkDrawIndexedIndirectCommand));
            }
        }
        vkCmdEndRendering(_cmd);
    }
}

// The translucent shadow casters of one command list (commands[first] at
// offset `commands`): their nearest depth, then their colours multiplied
// (in a rendering into a tint map; push constants and buffers bound).
void Renderer::_DrawTintCasters(const Scene::Gpu &gpu, VkDeviceSize commands)
{
    vkCmdSetDepthBias(_cmd, 0.0f, 0.0f, 1.5f);
    for (const VkPipeline *pipelines : {_tintDepthPipeline, _tintColorPipeline}) {
        VkPipeline bound = VK_NULL_HANDLE;
        for (size_t i = 0; i < gpu.translucent.size(); ++i) {
            if (pipelines[gpu.translucentVariant[i] & kDepthVariants] != bound) {
                bound = pipelines[gpu.translucentVariant[i] & kDepthVariants];
                vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
            }
            vkCmdDrawIndexedIndirect(_cmd, _commands.buffer,
                                     commands + VkDeviceSize(gpu.translucent[i]) * sizeof(VkDrawIndexedIndirectCommand),
                                     1, sizeof(VkDrawIndexedIndirectCommand));
        }
    }
}

// Volumes under transparency: the nearest translucent surfaces' depth
// (VolumePass::TranslucentDepth), drawn with the depth prepass's pipelines
// and the camera's culled commands; left readable by fragment shaders.
// ponytail: whole meshes, their transparent texels and cutouts included.
void Renderer::_RenderTranslucentDepth(const Scene::Gpu &gpu, const float *viewProj)
{
    const Image &target = _volumePass->EnsureTranslucentDepth(_width, _height);
    const VkImageSubresourceRange range = Range(VK_IMAGE_ASPECT_DEPTH_BIT);
    ImageBarrier(_cmd, target.image, range, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = target.view;
    att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {_width, _height}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &att;
    vkCmdBeginRendering(_cmd, &ri);
    const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
    vkCmdSetViewport(_cmd, 0, 1, &vp);
    vkCmdSetScissor(_cmd, 0, 1, &ri.renderArea);
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    PushDraw(_cmd, _layout, viewProj, 0);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
    vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
    VkPipeline bound = VK_NULL_HANDLE;
    for (size_t i = 0; i < gpu.translucent.size(); ++i) {
        const VkPipeline pipeline = _prepassPipeline[gpu.translucentVariant[i] & kDepthVariants];
        if (pipeline != bound) {
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        vkCmdDrawIndexedIndirect(_cmd, _commands.buffer, VkDeviceSize(gpu.translucent[i]) * sizeof(VkDrawIndexedIndirectCommand),
                                 1, sizeof(VkDrawIndexedIndirectCommand));
    }
    vkCmdEndRendering(_cmd);
    ImageBarrier(_cmd, target.image, range,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Subsurface meshes shaded again after the opaque pass, their subsurface
// lobe only, into Subsurface's targets (testing the opaque depth), which
// it then blurs into the colour.
void Renderer::_RenderSubsurface(const Scene::Gpu &gpu, const Frame &frame, const float *viewProj,
                                 const float *invViewProj, VkRenderingAttachmentInfo depth, bool splits)
{
    // The opaque pass's ids, velocity and colour are written before this
    // pass loads (and the blur adds to) them.
    VkMemoryBarrier2 opaque{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    opaque.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    opaque.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    opaque.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    opaque.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &opaque;
    vkCmdPipelineBarrier2(_cmd, &dep);

    _subsurface.BeginLighting(_cmd);
    const VkImageView views[4] = {_subsurface.Light().view, _subsurface.Id().view, _instanceId.view, _velocity.view};
    VkRenderingAttachmentInfo atts[4] = {};
    for (int i = 0; i < 4; ++i) {
        atts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        atts[i].imageView = views[i];
        atts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[i].loadOp = i < 2 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {_width, _height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 4;
    ri.pColorAttachments = atts;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(_cmd, &ri);
    const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
    vkCmdSetViewport(_cmd, 0, 1, &vp);
    vkCmdSetScissor(_cmd, 0, 1, &ri.renderArea);
    vkCmdSetDepthWriteEnable(_cmd, VK_FALSE);  // see _RenderTranslucency
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    PushDraw(_cmd, _layout, viewProj, 0);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
    vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
    VkPipeline bound = VK_NULL_HANDLE;
    for (size_t i = 0; i < gpu.subsurface.size(); ++i) {
        if (_subsurfacePipeline[_shadingBase + gpu.subsurfaceVariant[i]] != bound) {
            bound = _subsurfacePipeline[_shadingBase + gpu.subsurfaceVariant[i]];
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
        }
        vkCmdDrawIndexedIndirect(_cmd, _commands.buffer,
                                 VkDeviceSize(gpu.subsurface[i]) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }
    vkCmdEndRendering(_cmd);
    _subsurface.Blur(_cmd, _color.view, invViewProj, frame.subsurfaceSamples, _aovs.SplitsView(), splits,
                     std::max(frame.directIntensity, 0.0f), std::max(frame.indirectIntensity, 0.0f));
}

bool Renderer::IsConverged() const
{
    return (!_adapting || _post.ExposureConverged()) && _taaConverged && (!_rt || _rt->Converged()) &&
           !(_reflections && _reflections->ProbePending()) && _lodFadeLeft == 0;
}

uint32_t Renderer::VisibleInstances() const
{
    const auto *commands = static_cast<const VkDrawIndexedIndirectCommand *>(_commands.mapped);
    uint32_t total = 0;
    for (uint32_t i = 0; i < _lastDrawCount; ++i) {
        total += commands[i].instanceCount;
    }
    return total;
}

// ponytail: synchronous, one frame in flight. The Hydra layer's GPU sharing
// (interop.h) skips the readback; frames in flight would come next.
void Renderer::Render(const Scene &scene, const Frame &appFrame, const Targets &out)
{
    std::lock_guard<std::mutex> lock(_frameMutex);
    // The app's camera, or this film sample's through a point of the lens
    // (Frame::dofJitter, set once the film's state is known below); the
    // film's and TAA's motion tests compare the app's.
    Frame lensed = appFrame;
    const Frame &frame = lensed;
    if (frame.width == 0 || frame.height == 0) {
        return;
    }
    const auto cpuStart = std::chrono::steady_clock::now();
    _frameColor = _linearColor = VK_NULL_HANDLE;
    if (frame.width != _outWidth || frame.height != _outHeight) {
        _Resize(frame.width, frame.height);
    }
    const Scene::Gpu &gpu = scene.GetGpu();
    // Upscaling (TAAU, Frame::renderScale): while TAA resolves, the scene
    // renders smaller and TAA reconstructs the frame's size. Not for the
    // film (its next frame would start it: full size from then on), final
    // quality, nor the passes that are full size only. The depth and ids
    // reach the frame's size by blits, if the formats allow them.
    if (_upscaleBlit < 0) {
        _upscaleBlit = 1;
        for (const VkFormat f : {kDepthFormat, kIdFormat, kVelocityFormat}) {
            VkFormatProperties p;
            vkGetPhysicalDeviceFormatProperties(_dev.physical, f, &p);
            constexpr VkFormatFeatureFlags kBlit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
            _upscaleBlit = (p.optimalTilingFeatures & kBlit) == kBlit ? _upscaleBlit : 0;
        }
    }
    const bool moving = _historyValid && (std::memcmp(appFrame.viewProj, _prevViewProj, sizeof(_prevViewProj)) != 0 ||
                                          gpu.moved);
    const bool upscale = frame.renderScale < 1.0f && _upscaleBlit == 1 && frame.postProcess && frame.taa &&
                         _taaValid && !frame.finalQuality &&
                         (moving || !frame.filmAccumulation ||
                          (_filmSamples == 0 && _stillFrames + 1 < kFilmStillFrames)) &&
                         !(frame.depthOfField && frame.fStop > 0) && !(frame.motionBlur && frame.shutter > 0) &&
                         !(frame.lensDistortion && frame.distortion.Active()) &&
                         !(frame.extraAovs && Aovs::Any(out.aovs));
    const float scale = upscale ? std::max(frame.renderScale, 0.25f) : 1.0f;
    const uint32_t sceneWidth = std::max(uint32_t(float(frame.width) * scale + 0.5f), 1u);
    const uint32_t sceneHeight = std::max(uint32_t(float(frame.height) * scale + 0.5f), 1u);
    if (sceneWidth != _width || sceneHeight != _height) {
        _ResizeScene(sceneWidth, sceneHeight);
    }
    _upscaled = _width != _outWidth || _height != _outHeight;
    _stats.renderWidth = _width;
    _stats.renderHeight = _height;
    _savePipelines = frame.pipelineCache;
    if ((gpu.displaced || gpu.facing) && !_displacedPipelines) {  // the first displaced or facing mesh
        _CreatePipelines(true);
        for (uint32_t set = 0; set < _shadingSets.size(); ++set) {
            const ShadingTargets targets = _SetTargets(set);
            _CreatePipelines(true, _shadingSets[set], &targets);
        }
        _displacedPipelines = true;
    }
    const uint32_t shadowSize = std::clamp(frame.shadowResolution, 256u, 8192u);
    if (shadowSize != _shadowSize) {
        _CreateShadowMap(shadowSize);
    }
    const float anisotropy = float(std::clamp(frame.anisotropy, 1u, 16u));
    if (anisotropy != _anisotropy) {
        for (uint32_t wrap = 0; wrap < kWrapModes * kWrapModes; ++wrap) {
            vkDestroySampler(_dev.device, _samplers[wrap], nullptr);
            _samplers[wrap] = CreateMaterialSampler(_dev.device, anisotropy, wrap);
        }
        _anisotropy = anisotropy;
    }

    // Reflections beyond the screen: their own renders of the scene, done
    // before this frame records (probes.h).
    if (frame.reflectionProbe || frame.planarReflections) {
        if (!_reflections) {
            _reflections = std::make_unique<Reflections>(_dev);
        }
        _reflections->Update(scene, frame);
    } else {
        _reflections.reset();
    }

    FrameData &fd = *static_cast<FrameData *>(_frameData.mapped);
    fd = FrameData{};
    // No cascades (traced sun, shadows off): every split behind the camera, so
    // lookups (volume.frag, fog, translucency) see no shadow instead of an
    // empty cascade 0 (cameraForward is 0 too, so depth 0 fell into it).
    std::fill_n(fd.cascadeSplits, kCascades, -1.0f);
    fd.features[0] = (frame.normalMaps ? kFeatureNormalMaps : 0) | (frame.materialGraphs ? kFeatureMaterialGraphs : 0) |
                     (frame.clusteredLights ? kFeatureClusters : 0);
    fd.features[0] |= frame.proceduralTextures ? kFeatureProcedurals : 0;
    fd.features[0] |= (frame.specularWorkflow ? kFeatureSpecularWorkflow : 0) | (frame.clearcoat ? kFeatureClearcoat : 0) |
                      (frame.sheen ? kFeatureSheen : 0) | (frame.thinFilm ? kFeatureThinFilm : 0) |
                      (frame.anisotropicSpecular ? kFeatureAnisotropy : 0);
    fd.styles[0] = (frame.toon ? kStyleToon : 0) | (frame.hairShading ? kStyleHair : 0) |
                   (frame.texturedLobes ? kStyleTexturedLobes : 0) |
                   (frame.texturedTransmission ? kStyleTexturedTransmission : 0);
    fd.styles[1] = frame.toon ? std::min(frame.toonBands, 64u) : 0;
    fd.features[0] |= fd.styles[1] > 0 ? kFeatureToon : 0;
    fd.features[1] = std::clamp(frame.ssrSteps, 1u, 256u);
    fd.features[2] = std::clamp(frame.softShadowSamples, 1u, 16u);
    if (frame.displacement && gpu.displaced) {
        fd.features[0] |= kFeatureDisplacement;
        fd.geometry[0] = frame.displacementScale;
        fd.geometry[3] = frame.displacementGraphs ? 1.0f : 0.0f;
    }
    fd.lightClamp[0] = std::max(frame.directClamp, 0.0f);
    fd.lightClamp[1] = std::max(frame.indirectClamp, 0.0f);
    fd.screenTrace[0] = frame.ssgiThickness ? 1.0f : 0.0f;
    // SSR's back-face hits: their share of light (1 without Frame::screenBackface: as before).
    fd.screenTrace[1] = frame.screenBackface ? std::clamp(frame.screenBackfaceScale, 0.0f, 1.0f) : 1.0f;
    if (frame.lod) {
        fd.features[0] |= kFeatureLod;
        fd.geometry[1] = frame.lodBias;
        fd.geometry[2] = float(++_lodFrame & 0xFFFFFFu);  // exact as a float
        // ponytail: a fixed length, the TAA's history; a setting if it matters.
        fd.lodFade[0] = frame.lodFade && gpu.drawCount > gpu.meshCount ? kLodFadeFrames : 0;
    }
    std::memcpy(fd.viewProj, frame.viewProj, sizeof(fd.viewProj));
    std::memcpy(fd.invViewProj, frame.invViewProj, sizeof(fd.invViewProj));

    // TAA: offset the main pass by a sub-pixel Halton jitter. The camera
    // matrices used for culling, shadows and reprojection stay unjittered.
    const bool taa = frame.postProcess && frame.taa;
    float jitteredViewProj[16];
    Mat4 unjitter;          // jittered NDC -> unjittered NDC
    const uint32_t taaSamples = std::clamp(frame.taaSamples, 1u, 256u);
    uint32_t sample = 0;    // jitter (and GTAO noise) index, 1..taaSamples (the film's: its sample)
    // The camera's lens: motion blur with an open shutter when the camera
    // or the scene moved. That frame is final as it is: still frames after
    // it would only average the blur away (usdrecord renders until converged).
    const bool cameraMoved = _historyValid && std::memcmp(appFrame.viewProj, _prevViewProj, sizeof(_prevViewProj)) != 0;
    // The camera the motion starts from: the last frame's, or with sampled
    // motion the scene's own one frame before (then no history is needed).
    const float *motionViewProj = frame.sampledMotion ? appFrame.prevViewProj : _prevViewProj;
    const float *motionCameraPos = frame.sampledMotion ? appFrame.prevCameraPos : _prevCameraPos;
    const bool motionHistory = _historyValid || frame.sampledMotion;
    const bool cameraMotion = motionHistory && std::memcmp(appFrame.viewProj, motionViewProj, sizeof(_prevViewProj)) != 0;
    const bool motionBlur = frame.postProcess && frame.motionBlur && frame.shutter > 0 && motionHistory &&
                            (cameraMotion || gpu.moved);
    // Film: anything the image depends on besides the camera (the app's
    // scene and settings, the renderer's geometry, lights and probe) starts
    // it over; a fresh history (first frame, resize) starts it at once.
    uint64_t filmKey = frame.sceneVersion;
    for (const uint64_t v : {gpu.version, gpu.lightingHash, uint64_t(_reflections && _reflections->ProbePending())}) {
        filmKey = (filmKey ^ v) * 1099511628211ull;
    }
    const bool fresh = !_taaValid;
    const bool changed = !fresh && (cameraMoved || gpu.moved || filmKey != _filmKey);
    _filmKey = filmKey;
    _stillFrames = changed ? 0 : _stillFrames + 1;
    _freshRun = fresh || (_freshRun && !changed);
    if (fresh || changed) {
        _accumStart = std::chrono::steady_clock::now();
        _accumDone = false;
    }
    // DDGI preconverge: fresh GI probes (their first 1 / (1 - hysteresis)
    // updates, weighted 1 / (n + 1)) would stay in the film's average, so
    // TAA resolves until they settled and the film starts after.
    // The updates run in one frame (RayTracing::Record's giBurst), so this
    // holds for one frame only.
    const bool giSettling = frame.giPreconverge && frame.gi && _rt && !_rt->Converged();
    // Motion blur time samples: the film samples the shutter, so motion
    // no longer keeps it off.
    const bool shutterOn = frame.motionBlurTimeSamples && frame.postProcess && frame.motionBlur && frame.shutter > 0;
    const bool film = taa && frame.filmAccumulation && (!motionBlur || shutterOn) && !giSettling &&
                      (fresh || frame.finalQuality || _stillFrames >= kFilmStillFrames || (_filmSamples > 0 && !changed));
    // 0 adds, 1 starts over, 2 starts over from the TAA image.
    const uint32_t filmMode = !film                            ? 0
                              : fresh                          ? 1
                              : changed || _filmSamples == 0 ? (frame.finalQuality || _freshRun ? 1 : 2)
                                                               : 0;
    _filmSamples = !film ? 0 : filmMode != 0 ? 1 : _filmSamples + 1;
    // A run is final if it started so (filmMode 1); one that started from
    // the TAA image (2) stays interactive.
    _filmFinal = film && (filmMode == 1 || (filmMode == 0 && _filmFinal));
    _filmRuns += filmMode != 0 ? 1 : 0;
    float filmOffset[2] = {0, 0};  // the samples' offset from their pixel centres
    // The sample dimensions' sample: the film's, free running; while TAA
    // resolves (the camera or the scene moves) a loop of the first
    // interactiveSamples, a periodic pattern TAA's history settles on.
    const uint32_t noiseSample = film ? _filmSamples
                               : taa  ? _haltonIndex % std::clamp(frame.interactiveSamples, 1u, 256u) + 1
                                      : 0;
    _stats.sample = noiseSample;
    const uint32_t sobolIndex = noiseSample > 0 ? noiseSample - 1 : 0;
    // The post motion blur: TAA's frames (interactive) when the camera or
    // the scene moved; under the film the time samples blur instead.
    const bool postBlur = motionBlur && !film;
    // Motion blur time samples: each motion is kept from the frame before
    // it (shutter.comp's capture; the camera here) and the film's samples
    // render the scene at a time in the shutter, linear from that frame
    // (0) to this one (1), until a change other than motion moves the
    // scene's pools. The camera moves for this frame (`frame`, before the
    // lens jitter), the scene in its pools (below).
    const bool shutterCapture = shutterOn && motionHistory && (cameraMotion || gpu.moved) && gpu.instanceCapacity > 0 &&
                                gpu.drawCount > 0;
    if (shutterCapture) {
        _shutter.valid = true;
        _shutter.version = gpu.version;
        _shutter.instances = gpu.instances;
        _shutter.jointPool = gpu.joints;
        _shutter.weightPool = gpu.blendWeights;
        _shutter.instanceCapacity = gpu.instanceCapacity;
        std::memcpy(_shutter.viewProj, motionViewProj, sizeof(_shutter.viewProj));
        std::memcpy(_shutter.cameraPos, motionCameraPos, sizeof(_shutter.cameraPos));
    }
    _shutter.valid = _shutter.valid && shutterOn && gpu.version == _shutter.version &&
                     gpu.instances == _shutter.instances && gpu.joints == _shutter.jointPool &&
                     gpu.blendWeights == _shutter.weightPool && gpu.instanceCapacity == _shutter.instanceCapacity;
    const bool timeSample = film && _shutter.valid;
    // Adaptive sampling stops a pixel on its own samples' variance, but
    // the error of time samples is structured: every sample moves the
    // whole scene, so a pixel the motion covers for 3% of the shutter
    // misses all of its first 16 samples (zero variance) and would stop
    // dark, and the copies of a moving object between samples are steps,
    // not noise. Films with time samples run to filmSamples; the
    // samples render the whole frame anyway, so only the film's early
    // end is lost, which moving pixels prevent in most frames.
    const float adaptiveThreshold = timeSample                      ? 0.0f
                                  : frame.adaptiveThreshold > 0.0f ? frame.adaptiveThreshold
                                  : _filmFinal                      ? std::max(frame.adaptiveFinalThreshold, 0.0f)
                                                                    : 0.0f;
    _filmAdaptive = film && adaptiveThreshold > 0.0f;
    // ponytail: a box shutter.
    float shutterU = 1.0f;
    ScrambledSobol(sobolIndex, kSeedShutter, &shutterU, 1);
    const float shutterT = timeSample ? 1.0f + frame.shutterOpen + frame.shutter * shutterU : 1.0f;
    // Deformation blur: point velocities (mesh.vert) move by this.
    fd.deform[0] = shutterT - 1.0f;
    fd.deform[1] = frame.deformationBlur ? 1.0f : 0.0f;
    fd.deform[2] = timeSample ? 1.0f : 0.0f;
    fd.deform[3] = timeSample ? float((_shutter.joints ? 1 : 0) | (_shutter.weights ? 2 : 0)) : 0.0f;
    Mat4 shutterInvViewProj;  // the time sample's camera, before lens jitter
    if (timeSample) {
        for (int i = 0; i < 16; ++i) {  // linear in the view while the projection stays
            lensed.viewProj[i] = _shutter.viewProj[i] + (appFrame.viewProj[i] - _shutter.viewProj[i]) * shutterT;
        }
        const Mat4 inv = Inverse(ToMat4(lensed.viewProj));
        std::memcpy(lensed.invViewProj, inv.m, sizeof(lensed.invViewProj));
        shutterInvViewProj = inv;
        for (int i = 0; i < 3; ++i) {
            lensed.cameraPos[i] = _shutter.cameraPos[i] + (appFrame.cameraPos[i] - _shutter.cameraPos[i]) * shutterT;
        }
        std::memcpy(fd.invViewProj, frame.invViewProj, sizeof(fd.invViewProj));
    }
    // Depth of field: a thin lens with a circle of confusion
    // f^2 / (N (S - f)) * |D - S| / D across on the sensor.
    const bool dofLens = frame.postProcess && frame.depthOfField && frame.fStop > 0 && frame.focalLength > 0 &&
                         frame.sensorWidth > 0 && frame.focusDistance > frame.focalLength;
    // Lens jitter (film samples): the camera moves to a point of a disc
    // whose radius gives that circle of confusion through the pinhole
    // projection (f S / (2 N (S - f))), sheared so the focus plane stays,
    // and the post DOF is skipped.
    const bool lensJitter = film && frame.dofJitter && dofLens;
    if (lensJitter) {
        float lens[2];
        ScrambledSobol(sobolIndex, kSeedLens, lens, 2);
        const float f = frame.focalLength, focus = frame.focusDistance;
        const float r = 0.5f * f * focus / (frame.fStop * (focus - f)) * std::sqrt(lens[0]);
        const float phi = 6.2831853f * lens[1];
        const Mat4 inv = ToMat4(frame.invViewProj);
        const Vec3 o = TransformPoint(inv, {0, 0, 0});
        const Vec3 right = Normalize(TransformPoint(inv, {1, 0, 0}) - o), up = Normalize(TransformPoint(inv, {0, 1, 0}) - o);
        Vec3 forward = Normalize(Cross(up, right));  // normal of the planes of equal depth
        if (Dot(forward, TransformPoint(inv, {0, 0, 1}) - TransformPoint(inv, {0, 0, -1})) < 0) {
            forward = forward * -1.0f;
        }
        const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
        // p -> p + d (dot(p - eye, forward) / S - 1): the lens point maps to
        // the eye, the focus plane to itself; its inverse is the same with -d.
        auto shear = [&](Vec3 d) {
            Mat4 m;
            const float fw[3] = {forward.x, forward.y, forward.z}, dd[3] = {d.x, d.y, d.z};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    m.m[i][j] += fw[i] * dd[j] / focus;
                }
                m.m[3][i] = -dd[i] * (1.0f + Dot(eye, forward) / focus);
            }
            return m;
        };
        const Vec3 d = (right * std::cos(phi) + up * std::sin(phi)) * r;
        const Mat4 viewProj = shear(d) * ToMat4(frame.viewProj);
        const Mat4 invViewProj = inv * shear(d * -1.0f);
        std::memcpy(lensed.viewProj, viewProj.m, sizeof(lensed.viewProj));
        std::memcpy(lensed.invViewProj, invViewProj.m, sizeof(lensed.invViewProj));
        lensed.cameraPos[0] = eye.x + d.x;
        lensed.cameraPos[1] = eye.y + d.y;
        lensed.cameraPos[2] = eye.z + d.z;
        std::memcpy(fd.invViewProj, frame.invViewProj, sizeof(fd.invViewProj));
    }
    std::memcpy(jitteredViewProj, frame.viewProj, sizeof(jitteredViewProj));
    if (taa) {
        sample = film ? _filmSamples : (_haltonIndex % taaSamples) + 1;
        ++_haltonIndex;
        filmOffset[0] = 0.5f - Halton(sample, 2);
        filmOffset[1] = 0.5f - Halton(sample, 3);
        const float jx = (Halton(sample, 2) - 0.5f) * 2.0f / float(_width);
        const float jy = (Halton(sample, 3) - 0.5f) * 2.0f / float(_height);
        for (int r = 0; r < 4; ++r) {  // clip.xy += jitter * clip.w
            jitteredViewProj[r * 4 + 0] += jx * jitteredViewProj[r * 4 + 3];
            jitteredViewProj[r * 4 + 1] += jy * jitteredViewProj[r * 4 + 3];
        }
        unjitter.m[3][0] = -jx;
        unjitter.m[3][1] = -jy;
        // The sky reconstructs rays per pixel, so it follows the jitter too.
        const Mat4 invJittered = unjitter * ToMat4(frame.invViewProj);
        std::memcpy(fd.invViewProj, invJittered.m, sizeof(fd.invViewProj));
    }
    std::memcpy(fd.viewProj, jitteredViewProj, sizeof(fd.viewProj));

    // Last frame's camera (motion vectors; without history: this one).
    // Reflections shade hits with last frame's resolved image, which exists
    // once post (TAA resolve) ran at this size.
    std::memcpy(fd.prevViewProj, _historyValid ? _prevViewProj : frame.viewProj, sizeof(fd.prevViewProj));
    // SSR's hits from the history's mip their footprint spans: built at
    // the end of each frame for the next.
    const bool ssrHitFilter = frame.ssr && frame.ssrHitFilter && frame.postProcess && frame.depthPrepass;
    _taa.SetHistoryMips(ssrHitFilter);
    if (frame.ssr && frame.postProcess && frame.depthPrepass && _historyValid) {
        fd.ssr[0] = 1.0f;
        fd.ssr[1] = float(sample);
        fd.screenTrace[2] = ssrHitFilter ? 1.0f : 0.0f;
        // Denoised: the shading pass writes its rays (traced reflections
        // replace SSR where they apply: no denoiser then).
        if (frame.ssrDenoise && !(frame.rtReflections && _rt && frame.depthPrepass)) {
            fd.ssr[2] = 1.0f;
            _ssr.Prepare();
        }
    }

    std::memcpy(fd.cameraPos, frame.cameraPos, sizeof(frame.cameraPos));
    fd.viewport[0] = float(_width);
    fd.viewport[1] = float(_height);
    // Ray tracing (raytracing.cpp): with the prepass (mesh.frag tells the
    // opaque surface the rays started from by its depth) traced shadows
    // take over the sun's cascades and the first local lights' atlas tiles.
    RayTracing::Work rt;
    const bool rtScene = _rt && gpu.drawCount > 0 && gpu.instanceCapacity > 0 &&
                         gpu.instanceCapacity <= kMaxRtInstances - kMaxRtSkinned;
    // Final quality (Frame::finalShadows) may trace them without rtShadows,
    // one ray per light and film sample, and up to kMaxRtShadowLights.
    // Auto traces when the maps would cost more: a final sample redraws every
    // shadow view (jittered light shapes) over the scene's triangles, while
    // tracing costs a ray per pixel and shadowed light. So: at least
    // kAutoTriangles triangles per pixel and shadowed light (960 x 540, 64
    // samples: City 94 per pixel, 5 lights, traced 2.2 against maps 4.5 s of
    // GPU; Kitchen_set 1.0, 6 lights, traced 0.93 against 0.64 s), and every
    // skinned instance blocking rays (the crowd's 20000 agents exceed
    // rtSkinnedInstances: its traced shadows would miss most of them).
    // ponytail: triangles before culling stand in for what each shadow view
    // draws; a measured per-frame cost would place the switch better.
    constexpr double kAutoTriangles = 2.0;
    const bool finalRun = film && _filmFinal;
    using FinalShadows = Frame::FinalShadows;
    auto autoTraced = [&] {
        const size_t local = frame.localShadows && frame.localLights ? gpu.shadowCasters.size() : 0;
        const size_t lights = std::min<size_t>(local + (gpu.shadowLight != kNone ? 1 : 0), kMaxRtShadowLights);
        const uint32_t skinCap = std::min(frame.rtSkinnedInstances, kMaxRtSkinned);
        return lights > 0 && double(gpu.triangleCount) >= kAutoTriangles * double(_width) * _height * double(lights) &&
               scene.LargestSkinned(frame.cameraPos, skinCap + 1).size() <= skinCap;
    };
    const bool tracedShadows =
        frame.rtShadows ||
        (finalRun && (frame.finalShadows == FinalShadows::Raytraced ||
                      (frame.finalShadows == FinalShadows::Auto && autoTraced())));
    rt.shadows = rtScene && frame.depthPrepass && tracedShadows;
    rt.reflections = rtScene && frame.depthPrepass && frame.rtReflections;
    // Traced refraction: in final quality (or always), for scenes with
    // refracting meshes.
    rt.refraction = rtScene && frame.depthPrepass && frame.translucency && frame.refraction && gpu.refraction &&
                    (frame.rtRefraction >= 2 || (frame.rtRefraction == 1 && film && _filmFinal));
    rt.refractionBounces = frame.rtRefractionBounces;
    rt.refractionDisplacement = frame.rtRefractionDisplacement;
    rt.shadowSamples = finalRun ? 1 : frame.rtShadowSamples;
    std::fill_n(rt.shadowLights, kMaxRtShadowLights, kNone);
    // Through glass and smoke: translucent casters colour each light's
    // shadow (a texel per light), volumes are marched (grey).
    const bool rtDraw = gpu.drawCount > 0 && gpu.instanceCapacity > 0;
    rt.shadowColor = frame.rtTranslucentShadows && rtDraw && frame.translucency && !gpu.translucent.empty();
    rt.shadowVolumes = frame.rtTranslucentShadows && rtDraw && frame.volumes && frame.volume.shadows
                           ? gpu.volumeCount : 0;
    rt.volumeStep = std::max(frame.volume.stepSize, 0.05f);  // as VolumeParams
    rt.volumeDensity = std::max(frame.volume.density, 0.0f);
    ScrambledSobol(sobolIndex, kSeedRtShadow, rt.shadowSample, 2);
    rt.reflectionSamples = frame.rtReflectionSamples;
    rt.sun = gpu.shadowLight;
    rt.gi = rtScene && frame.gi;
    rt.giProbes = frame.giProbes;
    rt.giRays = frame.giRays;
    rt.giHysteresis = frame.giHysteresis;
    rt.giBurst = frame.giPreconverge;
    rt.displacement = frame.rtDisplacement && frame.displacement && gpu.displaced ? frame.displacementScale : 0.0f;
    rt.displacementGraphs = frame.displacementGraphs;
    const uint32_t rtSun = rt.shadows && frame.shadows && gpu.shadowLight != kNone ? 1 : 0;
    rt.shadowLights[0] = rtSun ? gpu.shadowLight : kNone;
    // Jittered light shapes (film samples only): this sample's point on
    // every shape, the sun's direction within its disc.
    const bool shapeJitter = film && _filmFinal && frame.shadowJitter && frame.softShadows;
    float shapeSample[3];
    ScrambledSobol(sobolIndex, kSeedLightShape, shapeSample, 3);
    float toSun[3];
    std::copy(gpu.shadowDirection, gpu.shadowDirection + 3, toSun);
    const bool sunJitter = shapeJitter && frame.softSunShadows && gpu.shadowAngle > 0;
    if (sunJitter) {
        const Vec3 d = Normalize({toSun[0], toSun[1], toSun[2]});
        const Vec3 t1 = Normalize(Cross(d, std::abs(d.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0})), t2 = Cross(d, t1);
        const float r = std::tan(0.5f * gpu.shadowAngle * 0.0174532925f) * std::sqrt(shapeSample[0]);
        const float phi = 6.2831853f * shapeSample[1];
        const Vec3 j = Normalize(d + (t1 * std::cos(phi) + t2 * std::sin(phi)) * r);
        toSun[0] = j.x;
        toSun[1] = j.y;
        toSun[2] = j.z;
    }
    // A traced sun still gets its cascades when something reads them that
    // the traced image (opaque pixels only) cannot cover: volumes and fog in
    // the medium, translucent surfaces (RtTraced falls back to the maps off
    // the prepass depth) and subsurface's thickness. Else the walls let the
    // sun through to smoke and fog. Traced local lights keep atlas tiles for
    // them likewise (_PlanLocalShadows).
    // ponytail: the maps' resolution, not the rays'; a traced 3D sun
    // visibility grid would match the surfaces' sharpness.
    const bool cascadeUsers = (frame.volumes && gpu.volumeCount > 0) || (frame.fog && frame.fogDensity > 0.0f) ||
                              (frame.translucency && !gpu.translucent.empty()) ||
                              (frame.subsurface && !gpu.subsurface.empty());
    const bool shadows = frame.shadows && (!rtSun || cascadeUsers) && gpu.drawCount > 0 && gpu.instanceCapacity > 0 &&
                         _FitCascades(gpu, frame, toSun, &fd);
    const uint32_t cascades = shadows ? std::clamp(frame.shadowCascades, 1u, kCascades) : 0;
    const uint32_t localLights = frame.localLights ? gpu.localLightCount : 0;
    fd.counts[0] = gpu.lightCount;
    fd.localLights[0] = localLights;
    fd.localLights[1] = sample;
    // Sample dimensions: this sample's value per effect (the TAA cycle's
    // or the film's sample; 0 without TAA).
    float dims[kSampleDims] = {};
    if (frame.sampleDimensions) {
        fd.sampleInfo[0] = 1;
        for (uint32_t d = 0; d < kSampleDims; ++d) {
            dims[d] = fd.sampleDims[d / 4][d % 4] = Sobol(sobolIndex, d);
        }
        // The denoised SSR's GGX sample (kDimSsrGgx, kDimSsrGgx2) is one 2D
        // point: from the scrambled (0, 2)-sequence, as Sobol dimensions
        // 10 and 11 together leave half of an 8 x 8 grid's cells empty
        // after 64 samples.
        ScrambledSobol(sobolIndex, kSeedSsrGgx, fd.sampleDims[2], 2);
    }
    if (localLights > 0) {
        _FitClusters(gpu, frame, &fd);
    }
    const std::vector<ShadowTile> shadowTiles =
        _PlanLocalShadows(gpu, frame, fd.clusters[3], rt.shadowLights + rtSun,
                          rt.shadows ? (finalRun ? kMaxRtShadowLights : 4) - rtSun : 0, cascadeUsers,
                          shapeJitter ? shapeSample : nullptr);
    rt.shadows = rt.shadows && rt.shadowLights[0] != kNone;
    fd.features[0] |= (rt.shadows ? kFeatureRtShadows : 0) | (rt.reflections ? kFeatureRtReflections : 0) |
                      (rt.gi ? kFeatureGi : 0);
    fd.counts[1] = shadows || rtSun ? gpu.shadowLight : kNone;
    // Soft sun shadows: for a sun with an angle, from its cascades.
    // Jittered, the cascades are looked up hard (the bit stays: the same
    // pipelines as the TAA frames).
    if (frame.softShadows && frame.softSunShadows && shadows && gpu.shadowAngle > 0) {
        fd.features[0] |= kFeatureSoftSun;
        fd.features[3] = _minSampler && !sunJitter ? uint32_t(_shadowMinLevels.size()) : 0;  // shadow_min.comp's levels
    }
    fd.shadowJitter[0] = shapeJitter ? 1 : 0;
    fd.counts[2] = gpu.environmentLevels;
    const bool ibl = gpu.environment && frame.imageBasedLighting;
    if (ibl) {
        for (int i = 0; i < 9; ++i) {
            std::memcpy(fd.envSh[i], gpu.environmentSh[i], sizeof(gpu.environmentSh[i]));
        }
        std::memcpy(fd.envWorldToLocal, gpu.environmentWorldToLocal, sizeof(fd.envWorldToLocal));
        std::memcpy(fd.envRadiance, gpu.environmentRadiance, sizeof(gpu.environmentRadiance));
        fd.envRadiance[3] = 1.0f;
    }
    const bool sky = gpu.environment && frame.showEnvironment && gpu.environmentLatlong != kNone;
    fd.counts[3] = sky ? gpu.environmentLatlong : kNone;  // the dome the sky pass shows
    // Volumetric fog; it also covers the background, as the sky pass does
    // (premultiplied: needs post to composite the clear colour behind).
    _filmFroxels = film && frame.fogFilmJitter;
    const bool fog = _SetupFog(gpu, frame, &fd);
    if (_reflections && !_reflections->Probes().empty()) {
        fd.features[0] |= kFeatureProbe;
        const auto &probes = _reflections->Probes();
        fd.reflections[0] = uint32_t(probes.size());
        for (size_t i = 0; i < probes.size(); ++i) {
            std::copy(probes[i].center, probes[i].center + 3, fd.probeCenter[i]);
            fd.probeCenter[i][3] = float(probes[i].levels - 1);
            std::copy(probes[i].min, probes[i].min + 3, fd.probeMin[i]);
            fd.probeMin[i][3] = probes[i].blend;
            std::copy(probes[i].max, probes[i].max + 3, fd.probeMax[i]);
            for (int c = 0; c < 4; ++c) {
                std::copy(probes[i].sh[c], probes[i].sh[c] + 3, fd.probeSh[i * 4 + c]);
            }
        }
    }
    std::fill(fd.planarInfo, fd.planarInfo + 4, kNone);
    if (_reflections && !_reflections->Planars().empty()) {
        fd.features[0] |= kFeaturePlanar;
        for (size_t i = 0; i < _reflections->Planars().size(); ++i) {
            const Reflections::Planar &planar = _reflections->Planars()[i];
            std::copy(planar.plane, planar.plane + 4, fd.planarPlane[i]);
            fd.planarInfo[i] = planar.slot;
        }
    }
    // Culling output; host-visible so VisibleInstances() can read it back.
    // One set of commands for the camera, one per cascade, one for atlas tiles.
    _dev.Reserve(_commands, VkDeviceSize(gpu.drawCount) * (2 + kCascades) * sizeof(VkDrawIndexedIndirectCommand),
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // Last frame's joints exist when the pool is still the buffer they were
    // built in (it is rebuilt by every frame's skin.comp).
    const bool prevJoints = gpu.joints && gpu.jointsBytes > 0 && _prevJointsSource == gpu.joints &&
                            _prevJoints.size >= gpu.jointsBytes;
    if (gpu.joints && gpu.jointsBytes > _prevJoints.size) {
        // Device-local: only GPU copies fill it, and every skinned vertex
        // reads it in the main pass.
        _dev.Destroy(_prevJoints);
        _prevJoints = _dev.CreateDeviceBuffer(gpu.jointsBytes,
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    _prevJointsSource = gpu.joints;
    // The camera's level of detail per instance slot, kept across frames
    // (cull.comp); a new pool starts from whatever is there.
    if (VkDeviceSize(std::max(gpu.instanceCapacity, 1u)) * 2 * sizeof(uint32_t) > _lodState.size) {
        _dev.Destroy(_lodState);
        _lodInstances = VK_NULL_HANDLE;  // cleared before its first use
        _lodState = _dev.CreateDeviceBuffer(VkDeviceSize(std::max(gpu.instanceCapacity, 1u)) * 2 * sizeof(uint32_t),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    // Blend shape motion: last frame's weights, kept like the joints.
    const bool blendMotion = frame.objectMotion && frame.blendShapeMotion && gpu.blendWeightsBytes > 0;
    const bool prevBlend = blendMotion && _prevBlendValid && _prevBlendSource == gpu.blendWeights &&
                           _prevBlendWeights.size >= gpu.blendWeightsBytes;
    if (blendMotion && gpu.blendWeightsBytes > _prevBlendWeights.size) {
        _dev.Destroy(_prevBlendWeights);
        _prevBlendWeights = _dev.CreateDeviceBuffer(gpu.blendWeightsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    const bool rayTracing = rt.shadows || rt.reflections || rt.gi || rt.refraction;
    // Time samples move the scene without a new version; the TLAS follows
    // (and once more when they end).
    rt.moving = timeSample || _shutterMoved;
    if (rayTracing) {
        rt.skinnedCapacity = std::min(frame.rtSkinnedInstances, kMaxRtSkinned);
        rt.skinned = scene.LargestSkinned(frame.cameraPos, rt.skinnedCapacity);
        _rt->Prepare(gpu, rt, _width, _height);
    } else if (_rt) {
        _rt->Release();  // nothing traced: no acceleration structures either
    }
    const bool draw = gpu.drawCount > 0 && gpu.instanceCapacity > 0;
    // Motion blur time samples: room for the capture (shutter.comp) and for
    // this frame's pools, saved while a sample moves them.
    const bool shutterMove = draw && timeSample;
    const VkDeviceSize instanceBytes = VkDeviceSize(gpu.instanceCapacity) * sizeof(Instance);
    if (shutterCapture || shutterMove) {
        auto ensure = [&](Buffer &b, VkDeviceSize bytes) {
            if (bytes > b.size) {
                _dev.Destroy(b);
                b = _dev.CreateDeviceBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            }
        };
        if (shutterCapture) {
            _shutter.joints = frame.objectMotion && prevJoints;
            _shutter.weights = prevBlend;
        }
        ensure(_shutterRows, VkDeviceSize(gpu.instanceCapacity) * 3 * 4 * sizeof(float));
        ensure(_shutterJoints, _shutter.joints ? gpu.jointsBytes : 4);
        ensure(_shutterWeights, _shutter.weights ? gpu.blendWeightsBytes : 4);
        if (shutterMove) {
            ensure(_savedInstances, instanceBytes);
            ensure(_savedJoints, std::max<VkDeviceSize>(gpu.jointsBytes, 4));
            ensure(_savedWeights, std::max<VkDeviceSize>(gpu.blendWeightsBytes, 4));
        }
    }
    // Refraction reads a copy of the opaque colour and the prepass depth.
    const bool refraction = draw && frame.translucency && frame.refraction && frame.depthPrepass && gpu.refraction;
    // Translucent shadows: tint maps at a fraction of the shadow maps'
    // size (1 x 1 while unused); new ones invalidate the cached atlas.
    const bool translucentShadows = draw && frame.translucency && frame.translucentShadows &&
                                    !gpu.translucent.empty();
    // Volume shadows: the volumes' transmittance goes into the sun's tint
    // map as well (VolumePass::RecordShadow), read with the translucent
    // casters'.
    const bool volumeBase = draw && frame.volumes && frame.volume.shadows && gpu.volumeCount > 0;
    const bool volumeShadows = shadows && volumeBase;
    // and the local lights' (Frame::localVolumeShadows): into the atlas tint map, tile by tile.
    const bool localVolumeShadows = volumeBase && frame.localVolumeShadows && frame.localShadows &&
                                    frame.localLights && !gpu.shadowCasters.empty();
    const uint32_t divisor = std::clamp(frame.translucentShadowDivisor, 1u, 4u);
    if (_transparency.EnsureTintMaps(translucentShadows || volumeShadows ? std::max(_shadowSize / divisor, 1u) : 1u,
                                     kCascades,
                                     translucentShadows || localVolumeShadows ? kShadowAtlasSize / divisor : 1u)) {
        _atlasVersion = 0;
    }
    if (translucentShadows || volumeShadows || localVolumeShadows) {
        fd.features[0] |= kFeatureTranslucentShadows;
    }
    if (frame.holdouts && gpu.holdouts) {
        fd.features[0] |= kFeatureHoldouts;
    }
    // Subsurface scattering blurs by the prepass depth.
    if (draw && frame.subsurface && frame.depthPrepass && !gpu.subsurface.empty()) {
        fd.features[0] |= kFeatureSubsurface;
    }
    if (frame.volumes && frame.volume.resolution >= 2 && gpu.volumeCount > 0) {
        _volumePass->EnsureHalf(_width, _height);
    }
    // Volumes under transparency: split around the nearest translucent
    // surface, which needs its depth (and the prepass's, readable early).
    const bool volumesUnder = frame.volumes && frame.volume.underTransparency && gpu.volumeCount > 0 && draw &&
                              frame.translucency && frame.depthPrepass && !gpu.translucent.empty();
    if (volumesUnder) {
        _volumePass->EnsureTranslucentDepth(_width, _height);
    }
    // SSGI: in the AO texture (gtao.cpp), sampled by shading.
    const bool ssgi = frame.ssgi && frame.depthPrepass;
    _gtao.SetSsgi(ssgi, frame.ssgiResolution);
    fd.features[0] |= ssgi ? kFeatureSsgi : 0;
    // Probe normalization needs local ambient light: GI's or SSGI's.
    fd.reflections[2] = frame.probeNormalize && (fd.features[0] & (kFeatureGi | kFeatureSsgi)) ? 1u : 0u;
    fd.reflections[2] |= frame.fogGiAmbient && (fd.features[0] & kFeatureGi) ? 2u : 0u;  // kFogGiAmbient
    fd.reflections[2] |= frame.lightTextures ? 4u : 0u;  // kLightTextures
    // Light path intensity: the splits' code (compiled with the splits' key
    // bit) scales the final pixel's direct and indirect light.
    const bool lightPath = frame.directIntensity != 1.0f || frame.indirectIntensity != 1.0f;
    // Light splits (aov.h): stored by shading pipelines compiled with them,
    // for the AOVs or for refraction's copy to take the intensities out of.
    const bool splits = (frame.extraAovs && Aovs::NeedsSplits(out.aovs)) || (lightPath && refraction);
    _aovs.EnsureSplits(splits ? _width : 1, splits ? _height : 1);
    fd.aovs[0] = splits ? 1u : 0u;
    fd.aovs[1] = frame.splitDomeDirect ? 1u : 0u;
    fd.lightPath[0] = std::max(frame.directIntensity, 0.0f);
    fd.lightPath[1] = std::max(frame.indirectIntensity, 0.0f);
    fd.lightPath[2] = lightPath ? 1.0f : 0.0f;
    _UpdateDescriptors(gpu, frame.depthPrepass, refraction ? _transparency.EnsureBackground() : VK_NULL_HANDLE);
    if (_rt) {
        _rt->UpdateDescriptors(_set, gpu);
    }
    if (refraction) {
        fd.features[0] |= kFeatureRefraction;
        fd.translucency[0] = float(std::clamp(frame.refractionSamples, 1u, 16u));
        fd.translucency[1] = float(_transparency.BackgroundLevels());
        fd.rtRefraction[0] = rt.refraction ? 1u : 0u;
    }
    // The shading pipelines with the code of this frame's optional features.
    // Bit 0 (kFeatureNormalMaps, never optional) marks the light splits'
    // code (mesh.frag's kCompiledLightSplits); kFeatureLod (never optional)
    // the levels of detail crossfade's dither (mesh.frag).
    static_assert((kOptionalFeatures & (1u | kFeatureLod)) == 0, "the light splits' and crossfade's key bits");
    _shadingBase = _ShadingSet((fd.features[0] & kOptionalFeatures) | (splits || lightPath ? 1u : 0u) |
                               (fd.lodFade[0] > 0 ? kFeatureLod : 0)) *
                   kVariants;
    if (frame.pipelinePrewarm) {
        // ponytail: prewarms the sets without the light splits' bit only.
        _Prewarm(fd.features[0] & kOptionalFeatures, gpu, frame.pipelineCache);
    }
    _lastDrawCount = draw ? gpu.drawCount : 0;

    Check(vkResetCommandBuffer(_cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(_cmd, &bi), "vkBeginCommandBuffer");
    // Timestamps when everything recorded before them finished: the time
    // between two is the pass in between (`name`).
    _passes.clear();
    if (_timestamps) {
        vkCmdResetQueryPool(_cmd, _timestamps, 0, kMaxTimestamps);
        vkCmdWriteTimestamp2(_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _timestamps, 0);
    }
    auto pass = [&](const char *name) {
        if (_timestamps && _passes.size() + 1 < kMaxTimestamps) {
            _passes.push_back(name);
            vkCmdWriteTimestamp2(_cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _timestamps, uint32_t(_passes.size()));
        }
    };
    for (const Reflections::Planar &planar : _reflections ? _reflections->Planars() : std::vector<Reflections::Planar>{}) {
        // Rendered (and left for readback) by the reflections' renderers.
        ImageBarrier(_cmd, planar.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    const VkDeviceSize vertexOffset = 0;
    if (draw) {
        // GPU culling: reset the commands from the scene's template, then one
        // invocation per instance slot appends survivors and counts them,
        // for the camera and for each shadow cascade.
        const VkDeviceSize commandBytes = VkDeviceSize(gpu.drawCount) * sizeof(VkDrawIndexedIndirectCommand);
        VkBufferCopy resets[1 + kCascades];
        for (uint32_t k = 0; k <= kCascades; ++k) {
            resets[k] = {0, k * commandBytes, commandBytes};
        }
        vkCmdCopyBuffer(_cmd, gpu.commandTemplate, _commands.buffer, 1 + cascades, resets);
        // Levels of detail of another scene's (or pool's) instances mean
        // nothing here: no last level, nothing to fade from.
        // (A destroyed scene's buffer handle can come back for a new one: the scene counts too.)
        if (gpu.instances != _lodInstances || gpu.sceneId != _lodScene) {
            vkCmdFillBuffer(_cmd, _lodState.buffer, 0, VK_WHOLE_SIZE, 0);
            _lodInstances = gpu.instances;
            _lodScene = gpu.sceneId;
        }
        if (prevJoints) {
            const VkBufferCopy all{0, 0, gpu.jointsBytes};
            vkCmdCopyBuffer(_cmd, gpu.joints, _prevJoints.buffer, 1, &all);
        }
        // Motion blur time samples: the motion this frame starts from last
        // frame's joints and blend shape weights.
        if (shutterCapture && _shutter.joints) {
            const VkBufferCopy all{0, 0, gpu.jointsBytes};
            vkCmdCopyBuffer(_cmd, gpu.joints, _shutterJoints.buffer, 1, &all);
        }
        if (shutterCapture && _shutter.weights) {
            const VkBufferCopy all{0, 0, gpu.blendWeightsBytes};
            vkCmdCopyBuffer(_cmd, _prevBlendWeights.buffer, _shutterWeights.buffer, 1, &all);
        }
        VkMemoryBarrier2 toCull{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toCull.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        toCull.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toCull.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        toCull.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &toCull;
        vkCmdPipelineBarrier2(_cmd, &dep);

        // Joint matrices of all skinned instances, read by culling (bounds)
        // and every vertex stage (reflection renders reuse the last frame's).
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _skinPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        if (frame.skinning) {
            vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);
        }
        VkMemoryBarrier2 jointsBuilt{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        jointsBuilt.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        jointsBuilt.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        jointsBuilt.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        jointsBuilt.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        dep.pMemoryBarriers = &jointsBuilt;
        vkCmdPipelineBarrier2(_cmd, &dep);

        // Motion blur time samples (shutter.comp): a motion's rows are
        // captured; a sample saves this frame's pools (put back after the
        // frame) and moves them to its time, before anything else reads them.
        if (shutterCapture || shutterMove) {
            struct {
                uint32_t capture;
                float t;
                uint32_t instances, weights, joints, objects;
            } params{0, shutterT, gpu.instanceCapacity, 0, _shutter.joints ? 1u : 0u, frame.objectMotion ? 1u : 0u};
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            dep.pMemoryBarriers = &barrier;
            if (shutterMove) {
                vkCmdPipelineBarrier2(_cmd, &dep);  // skin.comp's joints, before they are saved
                const VkBufferCopy instances{0, 0, instanceBytes};
                vkCmdCopyBuffer(_cmd, gpu.instances, _savedInstances.buffer, 1, &instances);
                if (gpu.jointsBytes > 0) {
                    const VkBufferCopy joints{0, 0, gpu.jointsBytes};
                    vkCmdCopyBuffer(_cmd, gpu.joints, _savedJoints.buffer, 1, &joints);
                }
                if (gpu.blendWeightsBytes > 0) {
                    const VkBufferCopy weights{0, 0, gpu.blendWeightsBytes};
                    vkCmdCopyBuffer(_cmd, gpu.blendWeights, _savedWeights.buffer, 1, &weights);
                }
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            }
            vkCmdPipelineBarrier2(_cmd, &dep);
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _shutterPipeline);
            vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
            if (shutterCapture) {
                params.capture = 1;
                vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(params), &params);
                vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);
                barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                vkCmdPipelineBarrier2(_cmd, &dep);
            }
            if (shutterMove) {
                params.capture = 0;
                params.weights = _shutter.weights ? uint32_t(gpu.blendWeightsBytes / sizeof(float)) : 0u;
                vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(params), &params);
                vkCmdDispatch(_cmd, (std::max(gpu.instanceCapacity, params.weights) + 63) / 64, 1, 1);
            }
            // Culling, rays (TLAS, skinned BLASes) and every pass read the moved pools.
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            vkCmdPipelineBarrier2(_cmd, &dep);
            pass("shutter");
        }

        // Without frustum culling the planes stay zero (everything is inside)
        // and so does the minimum size.
        CullParams cull{};
        if (frame.frustumCulling) {
            FrustumPlanes(frame.viewProj, cull.planes);
            cull.minRadius = std::max(frame.cullPixels, 0.0f);
        }
        cull.instanceCapacity = gpu.instanceCapacity;
        cull.drawCount = gpu.drawCount;
        // Screen size of an instance: its world radius over the size of a
        // pixel at its depth (which grows linearly with depth unless ortho).
        const Mat4 inv = ToMat4(frame.invViewProj);
        const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
        const Vec3 nearCenter = TransformPoint(inv, {0, 0, -1}), mid = TransformPoint(inv, {0, 0, 0});
        const Vec3 forward = Normalize(mid - nearCenter);
        const float pixel = Length(TransformPoint(inv, {0, 2.0f / float(_height), 0}) - mid);
        cull.orthographic = std::abs(frame.viewProj[3]) + std::abs(frame.viewProj[7]) +
                            std::abs(frame.viewProj[11]) < 1e-9f ? 1.0f : 0.0f;
        cull.pixelScale = (cull.orthographic > 0 ? 1.0f : Dot(mid - eye, forward)) / std::max(pixel, 1e-30f);
        cull.nearDistance = Dot(nearCenter - eye, forward);
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _cullPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(cull), &cull);
        vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);
        // Shadows draw the levels of detail the camera picked (cull.comp).
        VkMemoryBarrier2 lodPicked{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        lodPicked.srcStageMask = lodPicked.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        lodPicked.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        lodPicked.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        dep.pMemoryBarriers = &lodPicked;
        vkCmdPipelineBarrier2(_cmd, &dep);
        // Each cascade's ortho box (which spans the scene's whole depth
        // towards the light, so every caster in it stays); instances under
        // half a shadow texel are dropped.
        for (uint32_t c = 0; c < cascades; ++c) {
            FrustumPlanes(fd.cascadeViewProj[c], cull.planes);
            cull.orthographic = 1.0f;
            cull.minRadius = 0.5f;
            cull.pixelScale = 1.0f / fd.cascadeTexel[c];
            cull.listBase = (1 + c) * gpu.instanceCapacity;
            cull.commandBase = (1 + c) * gpu.drawCount;
            vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(cull), &cull);
            vkCmdDispatch(_cmd, (gpu.instanceCapacity + 63) / 64, 1, 1);
        }

        VkMemoryBarrier2 toDraw{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toDraw.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toDraw.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toDraw.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                              VK_PIPELINE_STAGE_2_HOST_BIT;
        toDraw.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                               VK_ACCESS_2_HOST_READ_BIT;
        dep.pMemoryBarriers = &toDraw;
        vkCmdPipelineBarrier2(_cmd, &dep);
        pass("culling");
    }
    if (localLights > 0 && frame.clusteredLights) {
        // Light clusters, read by the shading pass.
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _lightsPipeline);
        vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
        vkCmdDispatch(_cmd, (16 * 9 * 24 + 63) / 64, 1, 1);
        VkMemoryBarrier2 toShading{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        toShading.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toShading.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toShading.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toShading.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &toShading;
        vkCmdPipelineBarrier2(_cmd, &dep);
        pass("lightClusters");
    }
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);

    // Shadow cascades. Without a shadow light the map is only transitioned so
    // its descriptor is valid; the shader never samples it.
    const VkImageSubresourceRange shadowRange = Range(VK_IMAGE_ASPECT_DEPTH_BIT, kCascades);
    if (shadows) {
        ImageBarrier(_cmd, _shadowMap.image, shadowRange, VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        for (uint32_t c = 0; c < cascades; ++c) {
            VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = _shadowLayers[c];
            att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.clearValue.depthStencil.depth = 1.0f;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {_shadowSize, _shadowSize}};
            ri.layerCount = 1;
            ri.pDepthAttachment = &att;
            vkCmdBeginRendering(_cmd, &ri);
            const VkViewport vp{0, 0, float(_shadowSize), float(_shadowSize), 0, 1};
            const VkRect2D scissor{{0, 0}, {_shadowSize, _shadowSize}};
            vkCmdSetViewport(_cmd, 0, 1, &vp);
            vkCmdSetScissor(_cmd, 0, 1, &scissor);
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _shadowPipeline[0]);
            vkCmdSetDepthBias(_cmd, 0.0f, 0.0f, 1.5f);
            PushDraw(_cmd, _layout, fd.cascadeViewProj[c], (1 + c) * gpu.instanceCapacity);
            vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
            vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
            DrawChunks(_cmd, gpu, _commands.buffer,
                       VkDeviceSize(1 + c) * gpu.drawCount * sizeof(VkDrawIndexedIndirectCommand),
                       _shadowPipeline, _shadowCutoutPipeline, kDepthVariants);
            vkCmdEndRendering(_cmd);
        }
        // Volumes and translucent casters into the sun's tint map, cascade
        // by cascade.
        if (translucentShadows || volumeShadows) {
            const Transparency::TintMap &map = _transparency.SunTint();
            _transparency.BeginTint(_cmd, map, kCascades);
            for (uint32_t c = 0; c < cascades; ++c) {
                VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                color.imageView = map.colorLayers[c];
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfo depth = color;
                depth.imageView = map.depthLayers[c];
                depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                VkRenderingInfo tri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                tri.renderArea = {{0, 0}, {map.size, map.size}};
                tri.layerCount = 1;
                tri.colorAttachmentCount = 1;
                tri.pColorAttachments = &color;
                tri.pDepthAttachment = &depth;
                vkCmdBeginRendering(_cmd, &tri);
                const VkViewport tvp{0, 0, float(map.size), float(map.size), 0, 1};
                vkCmdSetViewport(_cmd, 0, 1, &tvp);
                vkCmdSetScissor(_cmd, 0, 1, &tri.renderArea);
                if (volumeShadows) {
                    _volumePass->RecordShadow(_cmd, _set, gpu.volumeCount, c, map.size, frame.volume);
                    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
                }
                if (translucentShadows) {
                    PushDraw(_cmd, _layout, fd.cascadeViewProj[c], (1 + c) * gpu.instanceCapacity);
                    _DrawTintCasters(gpu,
                                     VkDeviceSize(1 + c) * gpu.drawCount * sizeof(VkDrawIndexedIndirectCommand));
                }
                vkCmdEndRendering(_cmd);
            }
            _transparency.EndTint(_cmd, map, kCascades);
        }
        ImageBarrier(_cmd, _shadowMap.image, shadowRange,
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     kShadowReaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        pass("sunShadows");
        // Soft sun shadows: the cascades' nearest depths, level by level.
        if (fd.features[3] > 0) {
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, fd.features[3], 0, kCascades};
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _shadowMinPipeline);
            vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1, &_set, 0, nullptr);
            const uint32_t minSize = (_shadowSize + 3) / 4;
            for (uint32_t level = 0; level < fd.features[3]; ++level) {
                vkCmdPushConstants(_cmd, _layout, kPushStages, 0, sizeof(level), &level);
                const uint32_t n = std::max(minSize >> level, 1u);
                vkCmdDispatch(_cmd, (n + 7) / 8, (n + 7) / 8, cascades);
                ImageBarrier(_cmd, _shadowMin.image, all, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            }
            pass("sunShadowMin");
        }
    } else {
        ImageBarrier(_cmd, _shadowMap.image, shadowRange, VK_PIPELINE_STAGE_2_NONE, 0,
                     kShadowReaders, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // Depth prepass: the main pass then shades each pixel once, and GTAO
    // reads the finished depth before it. From here on depth is read-only.
    // Without it (Frame::depthPrepass) the main pass writes depth itself.
    constexpr VkPipelineStageFlags2 kDepthTests =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                 VK_PIPELINE_STAGE_2_NONE, 0, kDepthTests,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = _depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil.depth = frame.clearDepth;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {_width, _height}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depthAtt;
    // No Y flip: NDC y=-1 lands in memory row 0, which is the bottom row
    // in GL/Hydra order, so readback needs no flip either.
    const VkViewport vp{0, 0, float(_width), float(_height), 0, 1};
    const VkRect2D scissor{{0, 0}, {_width, _height}};
    if (frame.depthPrepass) {
        vkCmdBeginRendering(_cmd, &ri);
        vkCmdSetViewport(_cmd, 0, 1, &vp);
        vkCmdSetScissor(_cmd, 0, 1, &scissor);
        if (draw) {
            vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _prepassPipeline[0]);
            PushDraw(_cmd, _layout, jitteredViewProj, 0);
            vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
            vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
            DrawChunks(_cmd, gpu, _commands.buffer, 0, _prepassPipeline, _prepassCutoutPipeline, kDepthVariants,
                       fd.lodFade[0] > 0);
        }
        vkCmdEndRendering(_cmd);
        ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                     kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     kDepthTests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        pass("depthPrepass");
    }

    // AO only darkens the dome lighting, so without a dome it is skipped.
    // SSGI replaces it (its visibility is the AO).
    if (ssgi && draw) {
        _gtao.RecordSsgi(_cmd, fd.invViewProj, fd.prevViewProj, frame.cameraPos, frame.ssgiRadius,
                         frame.sampleDimensions ? dims[kDimSsgi] : float(sample + 1),
                         std::clamp(frame.aoSlices, 1u, 8u), std::clamp(frame.ssgiSteps, 1u, 32u), _taa.History(),
                         _historyValid,
                         std::min(4.0f / float(frame.postProcess ? _post.Exposure() : 1.0),
                                  frame.indirectClamp > 0.0f ? frame.indirectClamp : FLT_MAX),
                         _historyValid && std::memcmp(appFrame.viewProj, _prevViewProj, sizeof(_prevViewProj)) == 0,
                         frame.ssgiThickness, frame.ssgiBitmask ? std::max(frame.ssgiBitmaskThickness, 1e-3f) * frame.ssgiRadius : 0.0f,
                         frame.screenBackface ? std::clamp(frame.screenBackfaceScale, 0.0f, 1.0f) : -1.0f,
                         frame.ssgiTileOrder ? sample : 0, frame.sampleDimensions ? dims[kDimSsgiJitter] : -1.0f);
        pass("ssgi");
    } else if (frame.ambientOcclusion && frame.depthPrepass && draw && ibl) {
        _gtao.Record(_cmd, fd.invViewProj, frame.cameraPos, frame.aoRadius,
                     frame.sampleDimensions ? dims[kDimAo] : float(sample + 1),
                     std::clamp(frame.aoSlices, 1u, 8u));
        pass("ambientOcclusion");
    } else {
        _gtao.Clear(_cmd);
    }

    // The atlas draws the camera's levels of detail (cull.comp): it is
    // redrawn when the camera moves while there are levels.
    // Crossfades may run for kLodFadeFrames after the camera moves or new
    // levels arrive: not converged until they are over.
    // ponytail: instances that move on their own can start fades unseen.
    _lodFadeLeft = _lodFadeLeft > 0 ? _lodFadeLeft - 1 : 0;
    if (fd.lodFade[0] > 0 && gpu.drawCount != _lodDrawCount) {
        _lodFadeLeft = kLodFadeFrames + 1;
    }
    _lodDrawCount = gpu.drawCount;
    if (frame.lod && gpu.drawCount > gpu.meshCount && std::memcmp(appFrame.viewProj, _lodViewProj, sizeof(_lodViewProj))) {
        std::memcpy(_lodViewProj, appFrame.viewProj, sizeof(_lodViewProj));
        _atlasVersion = 0;
        _lodFadeLeft = fd.lodFade[0] > 0 ? kLodFadeFrames + 1 : 0;
    }
    if (timeSample || _shutterMoved) {
        _atlasVersion = 0;  // the casters moved without a new version
    }
    _RenderLocalShadows(gpu, draw ? shadowTiles : std::vector<ShadowTile>(), translucentShadows || localVolumeShadows,
                        localVolumeShadows ? &frame.volume : nullptr);
    if (!shadowTiles.empty()) {
        pass("localShadows");
    }
    if (fog) {
        _fog.Record(_cmd, _layout, _set);
        pass("fog");
    }
    if (rayTracing) {
        _rt->Record(_cmd, _set, gpu, pass);
    }

    const bool ssrDenoise = fd.ssr[2] > 0.0f;
    if (ssrDenoise) {
        _ssr.Clear(_cmd);
    }
    if (splits) {
        _aovs.ClearSplits(_cmd);
    }
    const Image *colorTargets[3] = {&_color, &_primId, &_instanceId};
    for (const Image *img : colorTargets) {
        ImageBarrier(_cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_NONE, 0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    ImageBarrier(_cmd, _velocity.image, Range(VK_IMAGE_ASPECT_COLOR_BIT), VK_PIPELINE_STAGE_2_NONE, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo colorAtts[4] = {};
    for (int i = 0; i < 3; ++i) {
        colorAtts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAtts[i].imageView = colorTargets[i]->view;
        colorAtts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtts[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::fill(colorAtts[i].clearValue.color.int32, colorAtts[i].clearValue.color.int32 + 4, -1);
    }
    // With post on, the scene is premultiplied by coverage (cleared to
    // transparent) and the tonemap composites the app's colour behind it.
    const float transparent[4] = {0, 0, 0, 0};
    std::memcpy(colorAtts[0].clearValue.color.float32, frame.postProcess ? transparent : frame.clearColor,
                sizeof(frame.clearColor));
    colorAtts[3].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtts[3].imageView = _velocity.view;
    colorAtts[3].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtts[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtts[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // clearValue zero: no object motion where nothing is drawn.
    if (frame.depthPrepass) {
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    }
    ri.colorAttachmentCount = 4;
    ri.pColorAttachments = colorAtts;
    vkCmdBeginRendering(_cmd, &ri);
    vkCmdSetViewport(_cmd, 0, 1, &vp);
    vkCmdSetScissor(_cmd, 0, 1, &scissor);
    // GTAO bound its own set; the sky and shading need the scene's again.
    vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
    // Motion vectors: 1 with last frame's joints, 2 without, 0 none; + 4
    // with last frame's blend shape weights (mesh.vert).
    const uint32_t motion = !frame.objectMotion ? 0 : (prevJoints ? 1 : 2) | (prevBlend ? 4 : 0);
    vkCmdSetDepthWriteEnable(_cmd, frame.depthPrepass ? VK_FALSE : VK_TRUE);
    if (draw) {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipeline[_shadingBase]);
        PushDraw(_cmd, _layout, jitteredViewProj, 0, motion);
        vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
        vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
        DrawChunks(_cmd, gpu, _commands.buffer, 0, _pipeline + _shadingBase, _pipeline + _shadingBase, kVariants - 1);
    }
    vkCmdSetDepthWriteEnable(_cmd, VK_FALSE);
    if (sky || (fog && frame.postProcess)) {
        vkCmdBindPipeline(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skyPipeline);
        vkCmdDraw(_cmd, 3, 1, 0, 0);
    }
    vkCmdEndRendering(_cmd);
    pass("shading");
    if (ssrDenoise) {
        _ssr.Resolve(_cmd, _color.view, fd.invViewProj, fd.prevViewProj, frame.cameraPos, sample, _ssrHistory,
                     frame.sampleDimensions ? dims[kDimSsrDenoise] : -1.0f, _aovs.SplitsView(), splits);
        pass("ssrDenoise");
    }
    _ssrHistory = ssrDenoise;
    if (fd.features[0] & kFeatureSubsurface) {
        _RenderSubsurface(gpu, frame, jitteredViewProj, fd.invViewProj, depthAtt, splits);
        pass("subsurface");
    }
    if (volumesUnder) {
        _RenderTranslucentDepth(gpu, jitteredViewProj);
        _volumePass->Record(_cmd, _set, gpu.volumeCount, _color.view, _width, _height, frame.volume,
                            VolumePass::kBehind);
        pass("volumes");
    }
    if (draw && frame.translucency && !gpu.translucent.empty()) {
        _RenderTranslucency(gpu, frame, &fd, jitteredViewProj, motion, depthAtt);
        pass("translucency");
    }
    if (!frame.depthPrepass) {
        ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                     kDepthTests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
    }
    // Blend shape motion: this frame's weights are the next one's last.
    _prevBlendValid = blendMotion && draw;
    if (_prevBlendValid) {
        VkMemoryBarrier2 drawn{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        // After the draws read the copy; the weights' upload is done too.
        drawn.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
        drawn.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        drawn.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        drawn.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &drawn;
        vkCmdPipelineBarrier2(_cmd, &dep);
        const VkBufferCopy all{0, 0, gpu.blendWeightsBytes};
        // (This frame's: a time sample moved the pool.)
        vkCmdCopyBuffer(_cmd, shutterMove ? _savedWeights.buffer : gpu.blendWeights, _prevBlendWeights.buffer, 1, &all);
        _prevBlendSource = gpu.blendWeights;
    }
    // Volumes, clipped by the opaque depth. Under transparency (above) the
    // part behind the nearest translucent surface went in before the
    // translucency pass, which dims and refracts it; here the part in front
    // goes over the composited translucency. Otherwise all of it goes over
    // everything shaded so far, translucent surfaces included. Volumetric
    // fog, already in the image to the surface behind, applies to them
    // once: volume.frag puts each behind the fog in front of it.
    // ponytail: only the nearest translucent layer splits a ray: volumes
    // between two layers of glass go behind both.
    if (frame.volumes &&
        _volumePass->Record(_cmd, _set, gpu.volumeCount, _color.view, _width, _height, frame.volume,
                            volumesUnder ? VolumePass::kInFront : VolumePass::kAll)) {
        pass(volumesUnder ? "volumesInFront" : "volumes");
    }
    // Light shapes: the local lights seen by the camera, in front of the opaque depth.
    if (draw && frame.lightShapesVisible && gpu.localLightCount > 0) {
        _volumePass->RecordLightShapes(_cmd, _set, gpu.lightCount, gpu.localLightCount, _color.view, _width,
                                       _height);
        pass("lightShapes");
    }
    ImageBarrier(_cmd, _velocity.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    constexpr VkPipelineStageFlags2 kPostStages =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    _adapting = frame.postProcess && frame.post.autoExposure;
    // The camera's lens: depth of field when it has an fStop (motion blur
    // above); under lens jitter only the residual blur between its samples,
    // when its largest circle (at infinity) reaches 1.5 pixels: below, the
    // lens samples leave no gaps and the gather's uneven edges (pixels
    // under half a pixel of circle keep their colour, their neighbours
    // take some) would only add light to small highlights.
    const float cocPixels = dofLens ? 0.5f * frame.focalLength * frame.focalLength /
                                          (frame.fStop * (frame.focusDistance - frame.focalLength)) *
                                          float(frame.width) / frame.sensorWidth
                                    : 0.0f;
    const float residualScale = 1.0f / std::sqrt(float(std::max(frame.filmSamples, 1u)));
    const bool dof = dofLens && (!lensJitter || (frame.dofResidualBlur && cocPixels * residualScale >= 1.5f));
    // Residual motion blur: time samples render whole copies of the scene,
    // as many as the film has samples, about shutter / filmSamples apart;
    // a fast move leaves pixels between the copies (steps across the
    // blur). Each sample is spread over its stratum of the shutter (the
    // first 2^m Sobol points are one per stratum), so the strata tile the
    // shutter exactly (EEVEE's vector blur between its steps). The motion
    // vectors of a time sample run from the frame before (t = 0) to its
    // time, t of a frame's motion, so the stratum's shutter is divided by
    // t. Pixels whose stratum moves them under a pixel are left alone.
    // ponytail: filmSamples not a power of two leaves the strata uneven.
    const bool residualBlur = timeSample && frame.motionBlurResidual && shutterT > 1e-3f;
    Lens::Params lens{};
    if (dof || postBlur || residualBlur) {
        const Mat4 back = postBlur       ? ToMat4(appFrame.invViewProj) * ToMat4(motionViewProj)
                          : residualBlur ? shutterInvViewProj * ToMat4(_shutter.viewProj)
                                         : Mat4();
        std::memcpy(lens.reprojection, &back.m[0][0], sizeof(lens.reprojection));
        std::copy(frame.depthToDistance, frame.depthToDistance + 4, lens.depthToDistance);
        if (dof) {
            // Thin lens: a circle of confusion f^2 / (N (S - f)) * |D - S| / D
            // across on the sensor, as a radius in pixels.
            lens.cocScale = cocPixels * (lensJitter ? residualScale : 1.0f);
            lens.focusDistance = frame.focusDistance;
        }
        // Blurs reach at most a tile (32 pixels) beyond the 3x3 tiles' max.
        lens.maxRadius = std::min(0.04f * float(frame.width), 32.0f);
        lens.frame = float(_haltonIndex % 64);
        lens.shutter = postBlur       ? frame.shutter
                       : residualBlur ? frame.shutter / (float(std::max(frame.filmSamples, 1u)) * shutterT)
                                      : 0.0f;
        const float stratum = shutterU * float(std::max(frame.filmSamples, 1u));
        lens.window[0] = std::floor(stratum) - stratum;
        lens.window[1] = lens.window[0] + 1.0f;
        lens.maxHalfBlur = std::min(0.025f * float(frame.width), 32.0f);
        _lens.RecordTiles(_cmd, lens);
    }
    if (dof) {
        _lens.RecordDepthOfField(_cmd, lens);
        pass("depthOfField");
    }
    if (residualBlur) {
        _lens.RecordResidualMotionBlur(_cmd, lens);
        pass("motionBlur");
    }
    if (frame.postProcess) {
        // HDR color feeds TAA and post (TAA also reads the read-only depth).
        ImageBarrier(_cmd, _color.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     kPostStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // TAA weight: 1 without usable history; 1/(n+1) over n history
        // frames, capped at kTaaMoving while the camera moves (0.1) and at
        // kTaaSamples - 1 while it is still (a 16-sample average).
        float alpha = 1.0f;
        Mat4 reprojection;
        if (taa) {
            const bool moved = _taaValid && std::memcmp(appFrame.viewProj, _prevViewProj, sizeof(_prevViewProj)) != 0;
            if (_taaValid) {
                // Upscaling resolves at the frame's (unjittered) pixel centres.
                reprojection = (_upscaled ? Mat4() : unjitter) * ToMat4(appFrame.invViewProj) * ToMat4(_prevViewProj);
            }
            if (!_taaValid) {
                _taaFrames = 0;
                _taaWarmup = true;
            } else if (moved) {
                _taaFrames = std::min(_taaFrames, kTaaMoving);
                _taaWarmup = false;
            }
            alpha = 1.0f / float(_taaFrames + 1);
            // Last sample of a still camera; with the film, which takes over, never.
            _taaConverged = _taaValid && !moved && _taaFrames >= taaSamples - 1 && !(frame.filmAccumulation && !postBlur);
            _taaFrames = std::min(_taaFrames + 1, taaSamples - 1);
            _taaValid = true;
            if (film) {
                alpha = 1.0f / float(_filmSamples);
                _taaConverged = _filmSamples >= std::max(frame.filmSamples, 1u);
                _taaFrames = kTaaMoving;  // TAA goes on from the film's image
            }
        } else {
            _taaValid = false;
            _taaWarmup = false;
            _taaConverged = true;
        }

        // Adaptation runs on wall-clock time. The first frame, toggling
        // auto-exposure and a fresh TAA history snap straight to the target:
        // while TAA accumulates a new image its histogram still shifts, and
        // the settled image should be exposed for itself.
        const auto now = std::chrono::steady_clock::now();
        const float dt = _hasLastFrame
            ? std::min(std::chrono::duration<float>(now - _lastFrame).count(), 0.1f) : 0.0f;
        const bool reset = !_hasLastFrame || frame.post.autoExposure != _lastAutoExposure || _taaWarmup;
        _lastFrame = now;
        _hasLastFrame = true;
        _lastAutoExposure = frame.post.autoExposure;
        if (_taaConverged) {
            _taaWarmup = false;
        }

        if (film) {
            // From a TAA image: as much as 4 samples, a small bias that keeps
            // the switch from TAA from showing one sample's noise.
            _taa.RecordFilm(_cmd, filmOffset, std::clamp(frame.filmFilterRadius, 0.0f, 2.0f), filmMode, 4.0f,
                            adaptiveThreshold, frame.adaptiveMinSamples,
                            frame.post.autoExposure ? _post.Exposure() : std::exp2(frame.post.exposureCompensation));
            pass("film");
        } else {
            _taa.Record(_cmd, &reprojection.m[0][0], alpha, filmOffset);
            pass("taa");
        }
        if (postBlur) {
            _lens.RecordMotionBlur(_cmd, lens);
            _taaConverged = true;
            pass("motionBlur");
        }
        _post.Record(_cmd, frame.post, dt, reset, frame.clearColor, frame.linearColor);
        pass("post");
        _stats.taaSamples = uint32_t(1.0f / alpha + 0.5f);
        _stats.taaTarget = film ? std::max(frame.filmSamples, 1u) : taa ? taaSamples : 1;
        std::memcpy(_prevViewProj, appFrame.viewProj, sizeof(_prevViewProj));
        std::memcpy(_prevCameraPos, appFrame.cameraPos, sizeof(_prevCameraPos));
        _historyValid = true;
    } else {
        _taaValid = false;
        _taaConverged = true;
        _historyValid = false;
        _stats.taaSamples = _stats.taaTarget = 1;
    }

    // Everything that may be read back goes to TRANSFER_SRC (with post on,
    // color comes from the post output, already in TRANSFER_SRC).
    for (const Image *img : colorTargets) {
        if (img == &_color && frame.postProcess) {
            continue;
        }
        ImageBarrier(_cmd, img->image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    ImageBarrier(_cmd, _depth.image, Range(VK_IMAGE_ASPECT_DEPTH_BIT),
                 kDepthTests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(_cmd, _velocity.image, Range(VK_IMAGE_ASPECT_COLOR_BIT),
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // Film: ids and depth come from the sample nearest the pixel centre so
    // far, not the last: a nearer one is kept in _filmIds, a farther one gets
    // the kept ones back (the ids after the extra AOVs, which read this
    // sample's ids: Cryptomatte's coverage sums all samples', its nearest-id
    // fallback and the positions skip a farther sample; depth after them
    // too: their G-buffer and positions are this sample's).
    const bool filmNearer = film && (filmMode != 0 || std::hypot(filmOffset[0], filmOffset[1]) < _filmIdDistance);
    auto filmIds = [&](uint32_t first, uint32_t count) {
        if (!film) {
            return;
        }
        Image *frameIds[3] = {&_primId, &_instanceId, &_depth};
        for (uint32_t i = first; i < first + count; ++i) {
            const VkImageAspectFlags aspect = i == 2 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            if (!_filmIds[i].image) {
                _filmIds[i] = _dev.CreateImage(i == 2 ? kDepthFormat : kIdFormat, _width, _height, 1, 1,
                                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                   VK_IMAGE_USAGE_SAMPLED_BIT,  // the view CreateImage makes
                                               aspect, VK_IMAGE_VIEW_TYPE_2D);
                ImageBarrier(_cmd, _filmIds[i].image, Range(aspect), VK_PIPELINE_STAGE_2_NONE, 0,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_GENERAL);
            } else {  // the last frame's copy
                ImageBarrier(_cmd, _filmIds[i].image, Range(aspect), VK_PIPELINE_STAGE_2_COPY_BIT,
                             VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            }
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {aspect, 0, 0, 1};
            region.extent = {_width, _height, 1};
            if (filmNearer) {
                vkCmdCopyImage(_cmd, frameIds[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _filmIds[i].image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                continue;
            }
            ImageBarrier(_cmd, frameIds[i]->image, Range(aspect), VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyImage(_cmd, _filmIds[i].image, VK_IMAGE_LAYOUT_GENERAL, frameIds[i]->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ImageBarrier(_cmd, frameIds[i]->image, Range(aspect), VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
    };
    if (filmNearer) {
        _filmIdDistance = std::hypot(filmOffset[0], filmOffset[1]);
    }

    // Extra AOVs (aov.h), before the lens bends depth and the ids.
    if (frame.extraAovs && Aovs::Any(out.aovs)) {
        Aovs::Params params;
        std::memcpy(params.view, frame.view, sizeof(params.view));
        std::memcpy(params.invViewProj, frame.invViewProj, sizeof(params.invViewProj));
        params.cryptomatte = frame.cryptomatte;
        params.cryptomatteIds = frame.cryptomatteIds;
        std::copy(out.aovComponents, out.aovComponents + Aovs::kCount, params.components);
        std::copy(frame.viewProj, frame.viewProj + 16, params.viewProj);
        // Under the film the AOVs average its samples: they start over with it.
        params.version = film ? ~_filmRuns : gpu.version;
        params.maxFrames = film ? std::max(frame.filmSamples, 1u) : taa ? taaSamples : 1;
        params.film = film;
        params.filmNearer = filmNearer;
        params.cryptoSoft = frame.cryptomatteSoft;
        std::copy(filmOffset, filmOffset + 2, params.filmOffset);
        params.filmRadius = std::clamp(frame.filmFilterRadius, 0.0f, 2.0f);
        params.filmWeights = _taa.FilmWeights();
        const VkPipeline *pipelines = draw && Aovs::NeedsGeometry(out.aovs) ? _aovs.Pipelines(_layout) : nullptr;
        std::function<void()> geometry;
        if (pipelines) {
            geometry = [&] {
                vkCmdBindDescriptorSets(_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _layout, 0, 1, &_set, 0, nullptr);
                PushDraw(_cmd, _layout, jitteredViewProj, 0);
                vkCmdBindVertexBuffers(_cmd, 0, 1, &gpu.vertices, &vertexOffset);
                vkCmdBindIndexBuffer(_cmd, gpu.indices, 0, VK_INDEX_TYPE_UINT32);
                DrawChunks(_cmd, gpu, _commands.buffer, 0, pipelines, pipelines, Aovs::kVariants);
            };
        }
        _aovs.Record(_cmd, params, _width, _height, _depth.image, _depth.view, _primId.image, _primId.view, out.aovs,
                     geometry);
        pass("aovs");
    }

    filmIds(0, 3);

    // Lens distortion warps everything read back.
    if (frame.lensDistortion && frame.distortion.Active()) {
        const bool post = frame.postProcess;
        _distortion.Record(_cmd, frame.distortion, _width, _height, post ? _post.Output() : _color.image,
                           post ? _post.OutputView() : _color.view, _depth.image, _depth.view, _primId.image,
                           _primId.view, _instanceId.image, _instanceId.view, frame.clearColor, frame.clearDepth,
                           post && frame.linearColor ? _post.LinearOutput() : VK_NULL_HANDLE,
                           post && frame.linearColor ? _post.LinearOutputView() : VK_NULL_HANDLE);
        pass("lensDistortion");
    }

    // Upscaling: depth, ids and motion vectors at the frame's size (nearest).
    const Image *outImages[4] = {&_depth, &_primId, &_instanceId, &_velocity};
    if (_upscaled) {
        VkMemoryBarrier2 written{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        written.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        written.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        written.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        written.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &written;
        vkCmdPipelineBarrier2(_cmd, &dep);
        for (int i = 0; i < 4; ++i) {
            const VkImageAspectFlags aspect = i == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            ImageBarrier(_cmd, _up[i].image, Range(aspect), VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_BLIT_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {aspect, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(_width), int32_t(_height), 1};
            blit.dstOffsets[1] = {int32_t(_outWidth), int32_t(_outHeight), 1};
            vkCmdBlitImage(_cmd, outImages[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _up[i].image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            ImageBarrier(_cmd, _up[i].image, Range(aspect), VK_PIPELINE_STAGE_2_BLIT_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            outImages[i] = &_up[i];
        }
    }
    _frameColor = frame.postProcess ? _post.Output() : _color.image;
    _linearColor = frame.postProcess && frame.linearColor ? _post.LinearOutput() : VK_NULL_HANDLE;

    // Copy only what the caller asked for, packed one after another.
    struct Output {
        VkImage image;
        VkImageAspectFlags aspect;
        VkDeviceSize bytesPerPixel;
        void *dst;
        VkDeviceSize offset;
    };
    Output outputs[5] = {
        {frame.postProcess ? _post.Output() : _color.image, VK_IMAGE_ASPECT_COLOR_BIT, 8, out.color, 0},
        {outImages[0]->image, VK_IMAGE_ASPECT_DEPTH_BIT, 4, out.depth, 0},
        {outImages[1]->image, VK_IMAGE_ASPECT_COLOR_BIT, 4, out.primId, 0},
        {outImages[2]->image, VK_IMAGE_ASPECT_COLOR_BIT, 4, out.instanceId, 0},
        {outImages[3]->image, VK_IMAGE_ASPECT_COLOR_BIT, 4, out.velocity, 0},
    };
    VkDeviceSize offset = 0;
    for (Output &o : outputs) {
        if (!o.dst) {
            continue;
        }
        o.offset = offset;
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {o.aspect, 0, 0, 1};
        copy.imageExtent = {_outWidth, _outHeight, 1};
        vkCmdCopyImageToBuffer(_cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               _readback.buffer, 1, &copy);
        offset += o.bytesPerPixel * _outWidth * _outHeight;
    }

    VkBufferMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = _readback.buffer;
    toHost.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(_cmd, &dep);
    pass("readback");

    // Motion blur time samples: this frame's pools back, for the next
    // commit's uploads and frames.
    if (shutterMove) {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo restore{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        restore.memoryBarrierCount = 1;
        restore.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(_cmd, &restore);
        const VkBufferCopy instances{0, 0, instanceBytes};
        vkCmdCopyBuffer(_cmd, _savedInstances.buffer, gpu.instances, 1, &instances);
        if (gpu.jointsBytes > 0) {
            const VkBufferCopy joints{0, 0, gpu.jointsBytes};
            vkCmdCopyBuffer(_cmd, _savedJoints.buffer, gpu.joints, 1, &joints);
        }
        if (gpu.blendWeightsBytes > 0) {
            const VkBufferCopy weights{0, 0, gpu.blendWeightsBytes};
            vkCmdCopyBuffer(_cmd, _savedWeights.buffer, gpu.blendWeights, 1, &weights);
        }
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier2(_cmd, &restore);
    }
    _shutterMoved = shutterMove;

    Check(vkEndCommandBuffer(_cmd), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = _cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    Check(vkQueueSubmit2(_dev.queue, 1, &si, _fence), "vkQueueSubmit2");
    Check(vkWaitForFences(_dev.device, 1, &_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    Check(vkResetFences(_dev.device, 1, &_fence), "vkResetFences");

    const auto *src = static_cast<const uint8_t *>(_readback.mapped);
    for (const Output &o : outputs) {
        if (o.dst) {
            std::memcpy(o.dst, src + o.offset, o.bytesPerPixel * _outWidth * _outHeight);
        }
    }
    if (frame.extraAovs) {
        _aovs.Read(out.aovs);
    }

    // What the camera drew: the culled commands' instances and triangles.
    const auto *commands = static_cast<const VkDrawIndexedIndirectCommand *>(_commands.mapped);
    _stats.instances = 0;
    _stats.triangles = 0;
    for (uint32_t i = 0; i < _lastDrawCount; ++i) {
        _stats.instances += commands[i].instanceCount;
        _stats.triangles += uint64_t(commands[i].instanceCount) * (commands[i].indexCount / 3);
    }
    _stats.drawCalls = 0;
    for (size_t i = 0; draw && i + 1 < gpu.drawChunks.size(); ++i) {
        _stats.drawCalls += (gpu.chunkKind[i] & (Scene::Gpu::kWide - 1)) != Scene::Gpu::kTranslucent;
    }
    _stats.drawCalls += draw ? uint32_t(gpu.translucent.size()) : 0;
    _stats.exposure = frame.postProcess ? _post.Exposure() : 1.0;
    _stats.accelerationStructureBytes = _rt ? _rt->AccelerationStructureBytes() : 0;
    _stats.passMs.clear();
    _stats.gpuMs = 0;
    if (_timestamps && !_passes.empty()) {
        std::vector<uint64_t> ticks(_passes.size() + 1);
        if (vkGetQueryPoolResults(_dev.device, _timestamps, 0, uint32_t(ticks.size()), ticks.size() * sizeof(uint64_t),
                                  ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (size_t i = 0; i < _passes.size(); ++i) {
                _stats.passMs.push_back({_passes[i], double(ticks[i + 1] - ticks[i]) * _timestampMs});
            }
            _stats.gpuMs = double(ticks.back() - ticks.front()) * _timestampMs;
        }
    }
    // The reflections' renders ran before this frame's passes.
    for (const auto &[name, ms] : {std::pair{"planarReflection", _reflections ? _reflections->planarMs : 0.0},
                                   std::pair{"probeCapture", _reflections ? _reflections->probeMs : 0.0}}) {
        if (ms > 0) {
            _stats.passMs.insert(_stats.passMs.begin(), {name, ms});
            _stats.gpuMs += ms;
        }
    }
    // Adaptive film: converged once no pixel took a sample (every pixel
    // takes the minimum first).
    _stats.filmActivePixels = _filmAdaptive ? _taa.ActivePixels() : 0;
    if (_filmAdaptive && _stats.filmActivePixels <= (1.0f - std::clamp(frame.adaptivePixelFraction, 0.0f, 1.0f)) *
                                                        float(_width) * float(_height)) {
        _taaConverged = true;
    }
    // Time to converge: counts from the last change, stops at convergence.
    if (!_accumDone) {
        _accumSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - _accumStart).count();
        _accumDone = IsConverged();
    }
    _stats.accumSeconds = _accumSeconds;
    _stats.film = film;
    _stats.filmFinal = film && _filmFinal;
    _stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
}

// As Render's readback, from the images it left in TRANSFER_SRC_OPTIMAL.
void Renderer::Read(const Targets &out)
{
    std::lock_guard<std::mutex> lock(_frameMutex);
    if (!_frameColor) {
        return;
    }
    const Image *images = _upscaled ? _up : nullptr;  // else the scene's, at the frame's size
    const std::pair<VkImage, void *> outputs[6] = {
        {_frameColor, out.color}, {(images ? images[0] : _depth).image, out.depth},
        {(images ? images[1] : _primId).image, out.primId}, {(images ? images[2] : _instanceId).image, out.instanceId},
        {(images ? images[3] : _velocity).image, out.velocity},
        {_linearColor ? _linearColor : _frameColor, out.linearColor}};
    const VkDeviceSize bytesPerPixel[6] = {8, 4, 4, 4, 4, 8};
    Check(vkResetCommandBuffer(_cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(_cmd, &bi), "vkBeginCommandBuffer");
    VkDeviceSize offsets[6] = {}, offset = 0;
    for (int i = 0; i < 6; ++i) {
        if (outputs[i].second) {
            offsets[i] = offset;
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = {i == 1 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                            : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
            copy.imageExtent = {_outWidth, _outHeight, 1};
            vkCmdCopyImageToBuffer(_cmd, outputs[i].first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _readback.buffer, 1,
                                   &copy);
            offset += bytesPerPixel[i] * _outWidth * _outHeight;
        }
    }
    VkMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(_cmd, &dep);
    Check(vkEndCommandBuffer(_cmd), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = _cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    Check(vkQueueSubmit2(_dev.queue, 1, &si, _fence), "vkQueueSubmit2");
    Check(vkWaitForFences(_dev.device, 1, &_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    Check(vkResetFences(_dev.device, 1, &_fence), "vkResetFences");
    const auto *src = static_cast<const uint8_t *>(_readback.mapped);
    for (int i = 0; i < 6; ++i) {
        if (outputs[i].second) {
            std::memcpy(outputs[i].second, src + offsets[i], bytesPerPixel[i] * _outWidth * _outHeight);
        }
    }
}

} // namespace vkr
