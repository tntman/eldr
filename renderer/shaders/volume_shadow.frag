#version 450
#extension GL_GOOGLE_include_directive : require

// Volume shadows: the sun's light through the volumes, one texel of a
// cascade per invocation, into Transparency's sun tint map (multiplied with
// the translucent casters', which follow). The colour is the transmittance
// along the texel's light ray across the cascade's depth range, the depth
// where the optical depth's mean lies (receivers behind it get all of it).
// part 1: the same for a local light's atlas tile (cascade = its index in
// ShadowTiles, originX/Y and mapSize = the tile's rectangle in the tint
// atlas): perspective rays from the near to the far plane, the depth projected.
// ponytail: one depth per texel, so surfaces inside a volume are either
// shadowed by all of it or by none; nearest reads; segments outside a
// volume's box are skipped and the march stops at 1e-3 transmittance, but
// empty tiles inside it are still stepped (the grid's tree isn't used).

#include "frame.glsl"
#include "volume.glsl"

layout(push_constant) uniform Params {  // as volume.frag's (VolumeParams)
    float stepSize;  // density voxels per step (twice that here)
    uint maxSteps;
    uint shadowSteps;
    float density;   // extinction multiplier
    float emission;
    float anisotropy;
    uint count;      // volumes[]
    uint scale;
    uint part;
    uint cascade;    // frame.cascadeViewProj's
    uint mapSize;    // texels per side
    uint originX;    // part 1: the tile's first texel
    uint originY;
} params;

struct ShadowTile {  // as mesh.frag's
    mat4 viewProj;
    vec4 rect, shadowParams, origin;
};
layout(std430, set = 0, binding = 19) readonly buffer ShadowTiles { ShadowTile shadowTiles[]; };

layout(location = 0) out vec4 outTint;

const uint kShadowSteps = 64u;  // per volume and texel, at most
const float kOpaque = 6.9;      // optical depth of 1e-3 transmittance: nothing is left to dim

void main()
{
    // The texel's light ray, parametrized by window depth s in [0, 1] (the
    // cascades are orthographic: depth is linear along it).
    const bool tile = params.part == 1u;
    mat4 viewProj = tile ? shadowTiles[params.cascade].viewProj : frame.cascadeViewProj[params.cascade];
    mat4 inv = inverse(viewProj);
    vec2 ndc = (gl_FragCoord.xy - (tile ? vec2(params.originX, params.originY) : vec2(0.0))) /
               float(params.mapSize) * 2.0 - 1.0;
    vec4 a = inv * vec4(ndc, -1.0, 1.0), b = inv * vec4(ndc, 1.0, 1.0);
    vec3 origin = a.xyz / a.w, dir = b.xyz / b.w - origin;
    float len = length(dir);
    float tau = 0.0, weighted = 0.0;  // optical depth, and times s
    for (uint i = 0u; i < params.count && tau < kOpaque; ++i) {
        vec4 w0 = volumes[i].worldToIndex[0], w1 = volumes[i].worldToIndex[1], w2 = volumes[i].worldToIndex[2];
        vec3 oi = Row3(w0, w1, w2, origin, 1.0), di = Row3(w0, w1, w2, dir, 0.0);
        vec2 span = Slabs(oi, di, volumes[i].indexMin.xyz, volumes[i].indexMax.xyz);
        span = vec2(max(span.x, 0.0), min(span.y, 1.0));
        if (!(span.x < span.y)) {
            continue;
        }
        uint steps = uint(clamp(ceil((span.y - span.x) * length(di) / (2.0 * params.stepSize)), 1.0,
                                float(kShadowSteps)));
        float ds = (span.y - span.x) / float(steps);
        float sigma = volumes[i].medium.x * params.density * ds * len;  // per unit of density
        pnanovdb_readaccessor_t acc = Accessor(volumes[i].grids.x);
        for (uint k = 0u; k < steps && tau < kOpaque; ++k) {
            float s = span.x + (float(k) + 0.5) * ds;
            float d = max(ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, acc, ivec3(floor(oi + di * s + 0.5))), 0.0) * sigma;
            tau += d;
            weighted += d * s;
        }
    }
    if (tau < 1e-3) {
        discard;
    }
    outTint = vec4(vec3(exp(-tau)), 1.0);
    float mean = weighted / tau;
    if (tile) {
        vec4 clip = viewProj * vec4(origin + dir * mean, 1.0);
        mean = clip.z / clip.w * 0.5 + 0.5;
    }
    gl_FragDepth = mean;
}
