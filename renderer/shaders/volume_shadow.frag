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
// Alpha holds the optical depth's spread around that depth, the half width
// w of a uniform medium's range with the same variance (sqrt(3) sigma), as
// 1 - w^(1/4) in window depth (translucent casters multiply it by 1; cleared
// 1: none): the fog ramps through it (fog_inject.comp).
// ponytail: one depth per texel, so surfaces inside a volume are either
// shadowed by all of it or by none; nearest reads; the grid's tree isn't
// used to skip empty tiles (VolumeOpticalDepth).

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
    float weighted, weighted2;  // the optical depth times s, s^2
    float tau = VolumeOpticalDepth(origin, dir, params.count, params.stepSize, params.density, weighted, weighted2);
    if (tau < 1e-3) {
        discard;
    }
    float mean = weighted / tau;
    float spread = sqrt(3.0 * max(weighted2 / tau - mean * mean, 0.0));
    if (tile) {
        vec4 n = viewProj * vec4(origin + dir * max(mean - spread, 0.0), 1.0);
        vec4 f = viewProj * vec4(origin + dir * min(mean + spread, 1.0), 1.0);
        vec4 m = viewProj * vec4(origin + dir * mean, 1.0);
        spread = 0.25 * (f.z / f.w - n.z / n.w);  // window depth is half of NDC
        mean = m.z / m.w * 0.5 + 0.5;
    }
    outTint = vec4(vec3(exp(-tau)), 1.0 - sqrt(sqrt(max(spread, 0.0))));
    gl_FragDepth = mean;
}
