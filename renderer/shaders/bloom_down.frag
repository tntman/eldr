#version 450
#extension GL_GOOGLE_include_directive : require
#include "post.glsl"

// 13-tap downsample (Jimenez, "Next Generation Post Processing in Call of
// Duty: Advanced Warfare"). The first pass uses a Karis average per group
// so single bright pixels can't flicker into huge blooms.
// a = (src texel size xy, karis, 0), b = (dst texel size xy)
layout(location = 0) out vec4 outColor;

vec3 Tap(vec2 uv, vec2 offset)
{
    return textureLod(src, uv + offset * pc.a.xy, 0.0).rgb;
}

float KarisWeight(vec3 c)
{
    return 1.0 / (1.0 + Luminance(c));
}

void main()
{
    vec2 uv = gl_FragCoord.xy * pc.b.xy;
    vec3 a = Tap(uv, vec2(-2, 2)), b = Tap(uv, vec2(0, 2)), c = Tap(uv, vec2(2, 2));
    vec3 d = Tap(uv, vec2(-2, 0)), e = Tap(uv, vec2(0, 0)), f = Tap(uv, vec2(2, 0));
    vec3 g = Tap(uv, vec2(-2, -2)), h = Tap(uv, vec2(0, -2)), i = Tap(uv, vec2(2, -2));
    vec3 j = Tap(uv, vec2(-1, 1)), k = Tap(uv, vec2(1, 1));
    vec3 l = Tap(uv, vec2(-1, -1)), m = Tap(uv, vec2(1, -1));

    vec3 color;
    if (pc.a.z > 0.5) {
        vec3 g0 = (a + b + d + e) * 0.25, g1 = (b + c + e + f) * 0.25;
        vec3 g2 = (d + e + g + h) * 0.25, g3 = (e + f + h + i) * 0.25;
        vec3 g4 = (j + k + l + m) * 0.25;
        float w0 = KarisWeight(g0) * 0.125, w1 = KarisWeight(g1) * 0.125;
        float w2 = KarisWeight(g2) * 0.125, w3 = KarisWeight(g3) * 0.125;
        float w4 = KarisWeight(g4) * 0.5;
        color = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    } else {
        color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 +
                (j + k + l + m) * 0.125;
    }
    outColor = vec4(max(color, vec3(0.0)), 1.0);
}
