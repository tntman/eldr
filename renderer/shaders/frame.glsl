// Per-frame data shared by all passes. std140 mirror of FrameData in renderer.cpp.
const uint kNone = 0xFFFFFFFFu;
const float PI = 3.14159265359;

layout(std140, set = 0, binding = 3) uniform FrameData {
    mat4 viewProj;       // as rasterized: includes the TAA jitter
    mat4 invViewProj;
    vec4 cameraPos;      // xyz
    vec4 cameraForward;  // xyz
    vec4 viewport;       // width, height
    uvec4 counts;        // lightCount, shadowLight, envLevels, envLatlong (kNone = none)
    mat4 cascadeViewProj[4];
    vec4 cascadeSplits;  // far view depth of each cascade
    vec4 cascadeTexel;   // world size of one shadow texel per cascade
    vec4 envSh[9];       // irradiance SH, dome-local space
    mat4 envWorldToLocal;
    vec4 envRadiance;    // rgb multiplier, w = 1 if a dome is active
    mat4 prevViewProj;   // unjittered camera of the last frame (previousColor, motion vectors)
    vec4 ssr;            // x = 1: trace reflections, y = noise index, z = 1: denoised (renderer/ssr.h)
    vec4 clusters;       // near, far (along cameraForward), log2(far / near), light cutoff radiance
    uvec4 localLights;   // x: sphere/rect lights, stored after the counts.x distant ones;
                         // y: TAA sample index (noise for soft shadows)
    uvec4 features;      // x: kFeature* flags, y: SSR steps, z: soft shadow samples
    // Shading styles (brdf.glsl): x: kStyle* flags, y: toon bands for every
    // material (0: only materials with their own).
    uvec4 styles;
    // Translucency (transparency.glsl): x: refraction blur taps, y:
    // refraction background mip levels, z: 1 / distance of the nearest
    // translucent mesh, w: OIT depth weight exponent.
    vec4 translucency;
    // Volumetric fog (fog.glsl).
    vec4 fogMedium;      // extinction per unit at the base height, height falloff, base height, phase g
    vec4 fogAlbedo;      // rgb: scattering colour; w: view depth the froxels reach
    uvec4 fogGrid;       // froxels across, up, slices; w: 1 = heights along Z (else Y)
    vec4 fogTemporal;    // x: slice jitter, y: history weight (0: none), z: last frame's fogAlbedo.w,
                         // w: 1 = orthographic camera
    // Reflection probe and planar reflections (probes.glsl).
    uvec4 reflections;   // x: probes, z: flags (kProbeNormalize)
    vec4 probeCenter[8]; // capture position; w: prefiltered levels - 1
    vec4 probeMin[8];    // influence and parallax box; w: blend distance (0: everywhere)
    vec4 probeMax[8];
    vec4 probeSh[32];    // per probe its irradiance SH L0, L1 (rgb)
    vec4 planarPlane[4]; // the mirrors' planes (unit normal towards the camera, offset)
    uvec4 planarInfo;    // the mirrors' mesh slots (DrawInfo), kNone: unused
    vec4 geometry;       // x: displacement scale, w: 1 = displacement graphs (kFeatureDisplacement);
                         // y: LOD bias, z: frame (kFeatureLod)
    // Sample dimensions (SampleNoise): this sample's value per kDim*;
    // sampleInfo.x 1 with them on; y 1: froxels jittered per film sample.
    vec4 sampleDims[3];
    uvec4 sampleInfo;
    uvec4 aovs;          // light splits (aov.h): x 1 = mesh.frag stores them, y 1 = the dome's visible light is direct
    vec4 lightClamp;     // x: direct, y: indirect light's brightness limit (brightest channel; 0: off)
    vec4 screenTrace;    // x: 1 = thickness estimator in SSR (Frame::ssgiThickness), y: back-face hits'
                         // share of light (Frame::screenBackface; 1: off), z: 1 = SSR hits
                         // footprint-filtered (Frame::ssrHitFilter, the history's mips)
    uvec4 shadowJitter;  // x 1: light shapes jittered this film sample (shadow maps seen from a point
                         // of each light, ShadowTile.origin): hard lookups, no PCSS
    uvec4 lodFade;       // x: frames a level of detail change crossfades over (0: it pops; LodKeep)
    vec4 lightPath;      // x: direct, y: indirect light's multiplier in the final pixel; z 1: either isn't 1
    vec4 deform;         // x: the time sample's offset from this frame (frames), y: 1 = point velocities on,
                         // z: 1 = a time sample (motion vectors from the shutter's capture),
                         // w: its capture holds joints (+1) and blend shape weights (+2)
    uvec4 rtRefraction;  // x 1: refracting front faces read rtRefractionImage (transparency.glsl)
} frame;

// Sample dimensions (Renderer's, after EEVEE's Sampling): every stochastic
// effect turns a 64 x 64 blue noise by a low-discrepancy value of its own
// per sample (Cranley-Patterson rotation), each dimension reading the noise
// elsewhere, so effects neither share a pattern nor leave one in the film.
const uint kDimAo = 0u, kDimSsgi = 1u, kDimSsr = 2u, kDimShadow = 3u, kDimSunShadow = 4u, kDimRefraction = 5u,
           kDimVolume = 6u, kDimFog = 7u;
// Denoised SSR's GGX sample (two: a 2D point, Renderer's kSeedSsrGgx), its spatial kernel's rotation
// (ssr_denoise.frag's copy), the SSGI bitmask's sector jitter (gtao.frag's).
const uint kDimSsrGgx = 8u, kDimSsrGgx2 = 9u, kDimSsrDenoise = 10u, kDimSsgiJitter = 11u;
layout(set = 0, binding = 90) uniform sampler2D blueNoise;

// In [0, 1): dimension `dim` of this sample at `pixel`. Off: interleaved
// gradient noise (Jimenez 2014) shifted per frame by `index`.
float SampleNoise(uint dim, vec2 pixel, float index)
{
    if (frame.sampleInfo.x == 0u) {
        return fract(52.9829189 * fract(dot(pixel + 5.588238 * index, vec2(0.06711056, 0.00583715))));
    }
    ivec2 p = (ivec2(pixel) + ivec2(dim * 23u, dim * 41u)) & 63;
    return fract(texelFetch(blueNoise, p, 0)[dim & 3u] + frame.sampleDims[dim >> 2u][dim & 3u]);
}

// frame.features.x: what the renderer's settings switched on.
const uint kFeatureNormalMaps = 1u, kFeatureMaterialGraphs = 2u, kFeatureClusters = 4u;
// 256: kFeatureProcedurals (program.glsl)
const uint kFeatureDisplacement = 1024u;  // a drawn material displaces (displace.glsl)
const uint kFeatureLod = 2048u;           // levels of detail (cull.comp)
// Soft sun shadows (PCSS from the distant light's angle), volumetric fog,
// the reflection probe, planar reflections.
const uint kFeatureSoftSun = 65536u, kFeatureFog = 131072u, kFeatureProbe = 262144u, kFeaturePlanar = 524288u;
// The sun disc's region in mesh.frag (a wide sun or a glossy material).
const uint kFeatureSunRegion = 16777216u;
// A motion blur time sample (mesh.vert's shutter reads, frame.deform.z).
const uint kFeatureTimeSample = 33554432u;

// The optional frame.features.x bits whose code a pipeline has: the
// shading pipelines are specialized with the ones their frame uses
// (Renderer::_ShadingSet), as skipped code still costs every pixel
// registers and occupancy. Other shaders have them all.
layout(constant_id = 13) const uint kCompiledFeatures = 0xFFFFFFFFu;

// c scaled down so its brightest channel is at most `limit` (0: no limit),
// keeping its hue: kills fireflies (Frame::directClamp, indirectClamp).
vec3 BrightnessClamp(vec3 c, float limit)
{
    float peak = max(max(c.r, c.g), c.b);
    return limit > 0.0 && peak > limit ? c * (limit / peak) : c;
}

// GGX visible normal (Heitz 2018), in the frame where n is +z.
vec3 SampleGgxVndf(vec3 ve, float a, vec2 u)
{
    vec3 vh = normalize(vec3(a * ve.x, a * ve.y, ve.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float r = sqrt(u.x), phi = 2.0 * PI * u.y;
    float p1 = r * cos(phi), p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
    vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * vh;
    return normalize(vec3(a * nh.x, a * nh.y, max(nh.z, 0.0)));
}

// Whether this frame has `feature` (an optional frame.features.x bit) and
// this pipeline its code.
bool Feature(uint feature)
{
    return (kCompiledFeatures & feature) != 0u && (frame.features.x & feature) != 0u;
}

// Level of detail crossfades (cull.comp): while an instance changes level
// it is drawn in both, as visible-list entries with kLodFading set (its slot
// in the low 24 bits, how far the fade got in bits 24..29, kLodFadingOut on
// the old level's), and each pixel keeps one of the two by a dither that
// moves every frame, so the history (TAA, accumulation) blends them.
const uint kLodFading = 0x80000000u, kLodFadingOut = 0x40000000u;

// The instance slot of a visible-list entry.
uint EntrySlot(uint entry)
{
    return entry >= kLodFading ? entry & 0x00FFFFFFu : entry;
}

// Whether this pixel draws the copy `entry` (always, unless it is fading).
// The depth prepass and the shading pass agree, so each pixel has one
// level's depth and shading.
bool LodKeep(uint entry, vec2 pixel)
{
    if (entry < kLodFading) {
        return true;
    }
    float t = float((entry >> 24) & 63u) / 64.0;
    float shift = float((uint(frame.geometry.z) * 2654435769u) >> 8) / 16777216.0;  // golden ratio steps
    float noise = fract(52.9829189 * fract(dot(floor(pixel), vec2(0.06711056, 0.00583715))) + shift);
    return (entry & kLodFadingOut) != 0u ? noise >= t : noise < t;
}

layout(set = 0, binding = 0) uniform sampler2D textures[];

// UsdLux/Storm latlong mapping; texture rows are stored bottom-up.
vec2 LatlongUv(vec3 local)
{
    return vec2(atan(local.z, local.x) / (2.0 * PI) + 0.25,
                1.0 - acos(clamp(local.y, -1.0, 1.0)) / PI);
}
