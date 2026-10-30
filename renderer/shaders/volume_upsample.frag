#version 450
#extension GL_GOOGLE_include_directive : require

// Half resolution volumes (volume.frag at scale 2) over the image. Each
// pixel takes the four nearest half resolution samples by their bilinear
// weights times how well the distance each one marched to (the farthest of
// its 2 x 2 block) matches the pixel's own, so volumes stop where nearer
// surfaces begin rather than bleeding over their edges.
// ponytail: a surface thinner than a block next to the volume can still
// show a little of it; TAA's jitter moves the blocks every frame.

#include "frame.glsl"

layout(set = 0, binding = 63) uniform sampler2D sceneDepth;   // window [0, 1]
// Six half floats (volume.frag's HALF): radiance, coloured transmittance.
layout(set = 0, binding = 66) uniform usampler2D halfVolumes;

// As volume.frag's: premultiplied radiance, 1 - the mean transmittance; the
// image is multiplied by the coloured transmittance (dual-source blend).
layout(location = 0, index = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outTransmittance;

// The light splits seen through the volumes, as volume.frag's VolumeSplits.
layout(set = 0, binding = 67, rgba16f) uniform image2DArray lightSplits;
void VolumeSplits(vec3 radiance, vec3 transmittance)
{
    int layers = imageSize(lightSplits).z;
    if (frame.aovs.x == 0u || layers <= 4) {
        return;
    }
    ivec2 q = ivec2(gl_FragCoord.xy);
    for (int i = 0; i < layers; ++i) {
        vec4 s = imageLoad(lightSplits, ivec3(q, i));
        imageStore(lightSplits, ivec3(q, i), vec4(s.rgb * transmittance + (i == 9 ? radiance : vec3(0.0)), s.a));
    }
}

// Distance from the camera to the depth at full resolution pixel p.
float Distance(vec2 p, float depth)
{
    if (depth >= 1.0) {
        return 1e30;
    }
    vec2 ndc = p / frame.viewport.xy * 2.0 - 1.0;
    vec4 near = frame.invViewProj * vec4(ndc, -1.0, 1.0), at = frame.invViewProj * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    return length(at.xyz / at.w - near.xyz / near.w);
}

void main()
{
    ivec2 last = textureSize(sceneDepth, 0) - 1, halfLast = textureSize(halfVolumes, 0) - 1;
    float own = Distance(gl_FragCoord.xy, texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r);
    vec2 h = gl_FragCoord.xy * 0.5 - 0.5;  // in half resolution texels (centres at integers)
    ivec2 base = ivec2(floor(h));
    vec2 f = h - vec2(base);
    vec3 radiance = vec3(0.0), transmittance = vec3(0.0);
    float weights = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1), t = clamp(base + o, ivec2(0), halfLast);
        ivec2 block = t * 2;
        float depth = max(max(texelFetch(sceneDepth, min(block, last), 0).r,
                              texelFetch(sceneDepth, min(block + ivec2(1, 0), last), 0).r),
                          max(texelFetch(sceneDepth, min(block + ivec2(0, 1), last), 0).r,
                              texelFetch(sceneDepth, min(block + ivec2(1, 1), last), 0).r));
        float d = Distance(vec2(block) + 1.0, depth);
        float relative = abs(d - own) / max(min(d, own), 1e-6);
        float bilinear = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float w = (bilinear + 1e-3) / (1e-3 + relative * relative);
        uvec4 v = texelFetch(halfVolumes, t, 0);
        vec2 a = unpackHalf2x16(v.x), b = unpackHalf2x16(v.y), c = unpackHalf2x16(v.z);
        radiance += vec3(a, b.x) * w;
        transmittance += vec3(b.y, c) * w;
        weights += w;
    }
    radiance /= weights;
    transmittance /= weights;
    float mean = dot(transmittance, vec3(1.0 / 3.0));
    outColor = vec4(radiance, 1.0 - mean);
    outTransmittance = vec4(transmittance, mean);
    VolumeSplits(radiance, transmittance);
}
