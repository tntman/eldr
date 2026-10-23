#version 450
#extension GL_GOOGLE_include_directive : require

// Heterogeneous volumes (NanoVDB grids) raymarched per pixel over the shaded
// image, nearest volume first, up to the opaque depth: single, isotropic
// scattering of the distant lights through a shadow march across the grid.
// Each pixel starts at a different fraction of a step every frame, which
// TAA averages. Steps skip tiles (and the background) of zero density.
// ponytail: volumes are composited one after another along the ray (in
// entry order, not interleaved where they overlap) and don't shadow each
// other or the surfaces.

#include "frame.glsl"
#include "lights.glsl"

layout(set = 0, binding = 63) uniform sampler2D sceneDepth;  // window [0, 1], prepass or not
layout(std430, set = 0, binding = 64) readonly buffer Grids { uint pnanovdb_buf_data[]; };
#define PNANOVDB_GLSL
#include "PNanoVDB.h"

// std430 mirror of GpuVolume in volume.h.
struct Volume {
    vec4 worldToIndex[3];  // density index = rows . (world, 1)
    vec4 indexMin;         // density index-space box around the active voxels
    vec4 indexMax;
    uvec4 grids;           // byte offsets: density, temperature, color, emission (kNone: none)
    uvec4 types;           // PNANOVDB_GRID_TYPE_*
    vec4 toGrid[9];        // density index -> temperature, color, emission index (3 rows each)
    vec4 params;           // x: temperature to Kelvin
};
layout(std430, set = 0, binding = 65) readonly buffer Volumes { Volume volumes[]; };

layout(push_constant) uniform Params {
    float stepSize;     // density voxels per step
    uint maxSteps;      // per pixel
    uint shadowSteps;
    float density;      // extinction multiplier
    uint count;         // volumes[]
} params;

layout(location = 0) out vec4 outColor;  // premultiplied: radiance, 1 - transmittance

const uint kMaxVolumes = 8u;     // per pixel, the nearest
const float kOpaque = 0.01;      // transmittance at which a ray stops
const float kAlbedo = 0.8;       // Volume::kDefaultAlbedo

pnanovdb_buf_t buf;

vec3 Unproject(vec3 ndc)
{
    vec4 p = frame.invViewProj * vec4(ndc, 1.0);
    return p.xyz / p.w;
}

vec3 Row3(vec4 r0, vec4 r1, vec4 r2, vec3 p, float w)
{
    vec4 q = vec4(p, w);
    return vec3(dot(r0, q), dot(r1, q), dot(r2, q));
}

// Avoids 0 * inf in the slab tests.
vec3 SafeDir(vec3 d)
{
    return mix(d, vec3(1e-12), lessThan(abs(d), vec3(1e-12)));
}

// Ray parameters [t0, t1] inside box [lo, hi] (t0 > t1: missed).
vec2 Slabs(vec3 o, vec3 d, vec3 lo, vec3 hi)
{
    vec3 inv = 1.0 / SafeDir(d);
    vec3 a = (lo - o) * inv, b = (hi - o) * inv;
    vec3 near = min(a, b), far = max(a, b);
    return vec2(max(max(near.x, near.y), near.z), min(min(far.x, far.y), far.z));
}

float ReadFloat(uint type, inout pnanovdb_readaccessor_t acc, ivec3 ijk)
{
    return pnanovdb_read_float(buf, pnanovdb_readaccessor_get_value_address(type, buf, acc, ijk));
}

// A float or Vec3f grid's value (floats splat to grey).
vec3 ReadRgb(uint type, inout pnanovdb_readaccessor_t acc, ivec3 ijk)
{
    pnanovdb_address_t a = pnanovdb_readaccessor_get_value_address(type, buf, acc, ijk);
    if (type == PNANOVDB_GRID_TYPE_VEC3F) {
        return vec3(pnanovdb_read_float(buf, a), pnanovdb_read_float(buf, pnanovdb_address_offset(a, 4u)),
                    pnanovdb_read_float(buf, pnanovdb_address_offset(a, 8u)));
    }
    return vec3(pnanovdb_read_float(buf, a));
}

// Density at index-space p (voxel values sit on integer coordinates),
// trilinear on average: one of the eight voxels around p, each as likely as
// its trilinear weight, picked by xi in [0, 1)^3 (TAA averages the rest).
float Density(inout pnanovdb_readaccessor_t acc, vec3 p, vec3 xi)
{
    vec3 base = floor(p);
    ivec3 ijk = ivec3(base) + ivec3(lessThan(xi, p - base));
    return max(ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, acc, ijk), 0.0);
}

pnanovdb_readaccessor_t Accessor(uint offset)
{
    pnanovdb_grid_handle_t grid;
    grid.address.byte_offset = offset;
    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(acc, pnanovdb_tree_get_root(buf, pnanovdb_grid_get_tree(buf, grid)));
    return acc;
}

// Transmittance from density index p towards a distant light (index-space
// direction dl per world unit) to the edge of the box.
float ShadowMarch(inout pnanovdb_readaccessor_t acc, vec3 p, vec3 dl, vec3 lo, vec3 hi, float noise)
{
    float far = Slabs(p, dl, lo, hi).y;
    if (!(far > 0.0) || params.shadowSteps == 0u) {
        return 1.0;
    }
    float dt = far / float(params.shadowSteps), tau = 0.0;
    for (uint k = 0u; k < params.shadowSteps; ++k) {
        ivec3 ijk = ivec3(floor(p + dl * ((float(k) + noise) * dt) + 0.5));
        tau += max(ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, acc, ijk), 0.0);
    }
    return exp(-tau * dt * params.density);
}

// Marches volume i over [t0, t1] of the world ray, adding to radiance and
// attenuating transmittance; steps come out of stepsLeft.
void March(uint i, vec3 origin, vec3 dir, float t0, float t1, float noise, inout uint stepsLeft,
           inout vec3 radiance, inout float transmittance)
{
    vec4 w0 = volumes[i].worldToIndex[0], w1 = volumes[i].worldToIndex[1], w2 = volumes[i].worldToIndex[2];
    vec3 lo = volumes[i].indexMin.xyz, hi = volumes[i].indexMax.xyz;
    uvec4 grids = volumes[i].grids, types = volumes[i].types;
    vec3 oi = Row3(w0, w1, w2, origin, 1.0), di = Row3(w0, w1, w2, dir, 0.0);
    vec3 safe = SafeDir(di);

    // Steps of stepSize voxels; a box that would need more than maxSteps
    // gets longer ones.
    float dt = max(params.stepSize / length(di), (t1 - t0) / float(max(params.maxSteps, 1u)));
    pnanovdb_readaccessor_t density = Accessor(grids.x), shadow = Accessor(grids.x);
    // An absent color grid (never read) gets the density's.
    pnanovdb_readaccessor_t color = Accessor(grids.z != kNone ? grids.z : grids.x);
    float t = t0 + noise * dt;
    while (t < t1 && stepsLeft > 0u && transmittance > kOpaque) {
        --stepsLeft;
        vec3 p = oi + t * di;
        ivec3 ijk = ivec3(floor(p));
        uint level;
        pnanovdb_address_t a = pnanovdb_readaccessor_get_value_address_and_level(PNANOVDB_GRID_TYPE_FLOAT, buf, density,
                                                                                 ijk, level);
        if (level > 0u && pnanovdb_read_float(buf, a) <= 0.0) {
            // An empty tile (or the background): on to the first step past
            // its far side, keeping this pixel's step offset.
            float dim = level == 1u ? 8.0 : level == 2u ? 128.0 : 4096.0;
            vec3 cell = floor(p / dim) * dim + step(0.0, di) * dim;
            vec3 exits = (cell - oi) / safe;
            float exit = min(min(exits.x, exits.y), exits.z);
            t = max(t + dt, t0 + (ceil((exit - t0) / dt - noise) + noise) * dt);
            continue;
        }
        float sigma = Density(density, p, fract(noise + vec3(0.8191725, 0.6710436, 0.5497005) * float(stepsLeft)))
                      * params.density;
        if (sigma > 0.0) {
            vec3 albedo = vec3(kAlbedo);
            if (grids.z != kNone) {
                vec3 q = Row3(volumes[i].toGrid[3], volumes[i].toGrid[4], volumes[i].toGrid[5], p, 1.0);
                albedo = clamp(ReadRgb(types.z, color, ivec3(floor(q + 0.5))), 0.0, 1.0);
            }
            vec3 light = vec3(0.0);
            for (uint k = 0u; k < frame.counts.x; ++k) {
                Light sun = lights[k];
                float visible = ShadowMarch(shadow, p, Row3(w0, w1, w2, sun.position.xyz, 0.0), lo, hi, noise);
                light += sun.color.rgb * sun.halfHeight.w * visible / (4.0 * PI);
            }
            // In-scattering integrated over the step (Hillaire 2015).
            float stepT = exp(-sigma * dt);
            radiance += transmittance * albedo * light * (1.0 - stepT);
            transmittance *= stepT;
        }
        t += dt;
    }
}

void main()
{
    // Row 0 is NDC y = -1, as in sky.frag; the rays follow the TAA jitter.
    vec2 ndc = gl_FragCoord.xy / frame.viewport.xy * 2.0 - 1.0;
    vec3 origin = Unproject(vec3(ndc, -1.0));
    vec3 dir = normalize(Unproject(vec3(ndc, 1.0)) - origin);
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float tEnd = depth < 1.0 ? length(Unproject(vec3(ndc, depth * 2.0 - 1.0)) - origin) : 3.0e38;

    // The volumes this ray crosses before the opaque surface, nearest first.
    uint ids[kMaxVolumes];
    vec2 spans[kMaxVolumes];
    uint n = 0u;
    for (uint i = 0u; i < params.count; ++i) {
        vec4 w0 = volumes[i].worldToIndex[0], w1 = volumes[i].worldToIndex[1], w2 = volumes[i].worldToIndex[2];
        vec2 span = Slabs(Row3(w0, w1, w2, origin, 1.0), Row3(w0, w1, w2, dir, 0.0), volumes[i].indexMin.xyz,
                          volumes[i].indexMax.xyz);
        span.x = max(span.x, 0.0);
        span.y = min(span.y, tEnd);
        if (!(span.x < span.y) || (n == kMaxVolumes && span.x >= spans[n - 1u].x)) {
            continue;
        }
        uint k = min(n, kMaxVolumes - 1u);
        for (; k > 0u && spans[k - 1u].x > span.x; --k) {
            ids[k] = ids[k - 1u];
            spans[k] = spans[k - 1u];
        }
        ids[k] = i;
        spans[k] = span;
        n = min(n + 1u, kMaxVolumes);
    }
    if (n == 0u) {
        discard;
    }

    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * float(frame.localLights.y),
                                               vec2(0.06711056, 0.00583715))));
    vec3 radiance = vec3(0.0);
    float transmittance = 1.0;
    uint stepsLeft = params.maxSteps;
    for (uint k = 0u; k < n && transmittance > kOpaque; ++k) {
        March(ids[k], origin, dir, spans[k].x, spans[k].y, noise, stepsLeft, radiance, transmittance);
    }
    outColor = vec4(radiance, 1.0 - transmittance);
}
