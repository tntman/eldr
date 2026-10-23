#version 450
#extension GL_GOOGLE_include_directive : require

// Depth of field of a thin lens (the camera's fStop and focus distance),
// before TAA: every pixel gathers on a golden-angle spiral turned per pixel
// and frame, which TAA averages into a smooth blur, as far as its own
// circle of confusion or any near-field circle around it reaches (tiles).
// A tap counts when its own circle reaches back to the pixel; a tap behind
// the pixel reaches no further than the pixel's circle, so the background
// never bleeds over sharper foreground (Gustafsson's single-pass bokeh).
// Colour is premultiplied by coverage.

layout(set = 0, binding = 0) uniform sampler2D color;  // copy of this frame's HDR colour
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D tiles;

#include "lens.glsl"

layout(location = 0) out vec4 outColor;

const int kTaps = 24;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 center = texelFetch(color, p, 0);
    float d0 = Distance(texelFetch(sceneDepth, p, 0).r);
    float c0 = Circle(d0);
    float radius = max(c0, NeighborMax(tiles, p).z);
    if (radius < 0.5) {
        outColor = center;
        return;
    }
    float turn = 6.2831853 * Noise(gl_FragCoord.xy);
    vec4 sum = center;
    float total = 1.0;
    for (int i = 0; i < kTaps; ++i) {
        float r = radius * sqrt((float(i) + 0.5) / float(kTaps));
        float a = float(i) * 2.3999632 + turn;
        vec2 uv = (gl_FragCoord.xy + r * vec2(cos(a), sin(a))) * pc.motion.zw;
        float d = Distance(textureLod(sceneDepth, uv, 0.0).r);
        float reach = d > d0 ? min(Circle(d), c0) : Circle(d);
        float m = smoothstep(r - 0.5, r + 0.5, reach);
        sum += mix(sum / total, textureLod(color, uv, 0.0), m);
        total += 1.0;
    }
    outColor = sum / total;
}
