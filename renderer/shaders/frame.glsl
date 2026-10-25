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
    vec4 ssr;            // x = 1: trace reflections, y = noise index
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
    vec4 probe;          // xyz: capture position; w: prefiltered levels - 1
    vec4 probeMin;       // the scene's bounds when captured (parallax correction)
    vec4 probeMax;
    vec4 planarPlane;    // the mirror's plane (unit normal towards the camera, offset)
    uvec4 planarInfo;    // x: the mirror's mesh slot (DrawInfo)
    vec4 geometry;       // x: displacement scale, w: 1 = displacement graphs (kFeatureDisplacement);
                         // y: LOD bias, z: frame (kFeatureLod)
    uvec4 lodFade;       // x: frames a level of detail change crossfades over (0: it pops; LodKeep)
} frame;

// frame.features.x: what the renderer's settings switched on.
const uint kFeatureNormalMaps = 1u, kFeatureMaterialGraphs = 2u, kFeatureClusters = 4u;
// 256: kFeatureProcedurals (program.glsl)
const uint kFeatureDisplacement = 1024u;  // a drawn material displaces (displace.glsl)
const uint kFeatureLod = 2048u;           // levels of detail (cull.comp)
// Soft sun shadows (PCSS from the distant light's angle), volumetric fog,
// the reflection probe, planar reflections.
const uint kFeatureSoftSun = 65536u, kFeatureFog = 131072u, kFeatureProbe = 262144u, kFeaturePlanar = 524288u;

// The optional frame.features.x bits whose code a pipeline has: the
// shading pipelines are specialized with the ones their frame uses
// (Renderer::_ShadingSet), as skipped code still costs every pixel
// registers and occupancy. Other shaders have them all.
layout(constant_id = 13) const uint kCompiledFeatures = 0xFFFFFFFFu;

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
