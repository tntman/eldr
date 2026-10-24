#version 450

// 5x5 box over the raw GTAO, skipping taps whose distance to the camera
// differs from the center's by more than ~1.5% per pixel of offset, so
// occlusion doesn't bleed across depth edges.
// ponytail: distance-only weights reject most taps on surfaces seen at
// grazing angles, leaving some speckle there for TAA; a plane-distance
// weight (needs the normal) would keep them.

layout(set = 0, binding = 1) uniform sampler2D rawAo;  // visibility, distance (0 = background)

#ifdef SSGI
// SSGI (gtao.frag built with SSGI): the same filter over gathered light
// (rgb) and visibility (a), at full or half resolution, upsampling it:
// taps count where their surface (from the full-resolution depth) lies in
// this pixel's plane, so surfaces seen at grazing angles keep their taps
// and nothing bleeds across depth edges or creases. Without any, the
// nearest raw texel.
layout(set = 0, binding = 0) uniform sampler2D sceneDepth;
layout(push_constant) uniform GtaoParams {
    mat4 invViewProj;
    vec4 eye;
    vec4 params;  // world radius, noise index, 1/width, 1/height (of the depth)
    mat4 prevViewProj;
    vec4 ssgi;      // x: depth pixels per raw pixel
    vec4 temporal;  // x: this frame's weight in the running average (1: none)
} pc;
layout(set = 0, binding = 3) uniform sampler2D previousSsgi;  // last frame's result, same pixels (a still camera)
layout(location = 0) out vec4 outSsgi;  // visibility, gathered irradiance / pi (gba): mesh.frag's AO texture

// World position at a depth pixel; w = 0 for background.
vec4 Position(ivec2 q)
{
    float depth = texelFetch(sceneDepth, q, 0).r;
    vec4 p = pc.invViewProj * vec4((vec2(q) + 0.5) * pc.params.zw * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return vec4(p.xyz / p.w, depth < 1.0 ? 1.0 : 0.0);
}

// Of the two one-pixel steps, the shorter one stays on this surface.
vec3 Tangent(vec3 p, ivec2 q, ivec2 step, ivec2 maxQ)
{
    vec3 a = Position(clamp(q + step, ivec2(0), maxQ)).xyz - p;
    vec3 b = p - Position(clamp(q - step, ivec2(0), maxQ)).xyz;
    return dot(a, a) < dot(b, b) ? a : b;
}

void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
    int scale = int(pc.ssgi.x);
    ivec2 rawQ = q / scale, maxRaw = textureSize(rawAo, 0) - 1, maxQ = textureSize(sceneDepth, 0) - 1;
    vec4 center = Position(q);
    if (center.w <= 0.0) {
        outSsgi = vec4(1.0, 0.0, 0.0, 0.0);
        return;
    }
    vec3 n = normalize(cross(Tangent(center.xyz, q, ivec2(1, 0), maxQ), Tangent(center.xyz, q, ivec2(0, 1), maxQ)));
    float tolerance = 0.01 * distance(center.xyz, pc.eye.xyz);
    vec4 sum = vec4(0.0);
    float weights = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            ivec2 r = clamp(rawQ + ivec2(x, y), ivec2(0), maxRaw);
            vec4 tap = Position(min(r * scale, maxQ));
            float w = tap.w > 0.0 && abs(dot(tap.xyz - center.xyz, n)) <= tolerance ? 1.0 : 0.0;
            sum += texelFetch(rawAo, r, 0) * w;
            weights += w;
        }
    }
    vec4 s = weights > 0.0 && !any(isnan(n)) ? sum / weights : texelFetch(rawAo, rawQ, 0);
    outSsgi = vec4(s.a, s.rgb);
    if (pc.temporal.x < 1.0) {
        outSsgi = mix(texelFetch(previousSsgi, q, 0), outSsgi, pc.temporal.x);
    }
}
#else
layout(location = 0) out float outAo;

void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
    ivec2 maxQ = textureSize(rawAo, 0) - 1;
    vec2 center = texelFetch(rawAo, q, 0).xy;
    if (center.y <= 0.0) {
        outAo = 1.0;
        return;
    }
    float sum = 0.0, weights = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 s = texelFetch(rawAo, clamp(q + ivec2(x, y), ivec2(0), maxQ), 0).xy;
            float tolerance = 0.015 * center.y * float(max(max(abs(x), abs(y)), 1));
            float w = s.y > 0.0 && abs(s.y - center.y) <= tolerance ? 1.0 : 0.0;
            sum += s.x * w;
            weights += w;
        }
    }
    outAo = sum / max(weights, 1.0);
}
#endif
