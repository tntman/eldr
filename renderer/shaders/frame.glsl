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
    // Translucency (transparency.glsl): x: refraction blur taps, y:
    // refraction background mip levels, z: 1 / distance of the nearest
    // translucent mesh, w: OIT depth weight exponent.
    vec4 translucency;
} frame;

// frame.features.x: what the renderer's settings switched on.
const uint kFeatureNormalMaps = 1u, kFeatureMaterialGraphs = 2u, kFeatureClusters = 4u;
// 256: kFeatureProcedurals (program.glsl)

layout(set = 0, binding = 0) uniform sampler2D textures[];

// UsdLux/Storm latlong mapping; texture rows are stored bottom-up.
vec2 LatlongUv(vec3 local)
{
    return vec2(atan(local.z, local.x) / (2.0 * PI) + 0.25,
                1.0 - acos(clamp(local.y, -1.0, 1.0)) / PI);
}
