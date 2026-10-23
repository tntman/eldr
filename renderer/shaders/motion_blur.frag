#version 450
#extension GL_GOOGLE_include_directive : require

// Motion blur after TAA, McGuire 2012's reconstruction: every pixel
// samples along the longest motion around it (tiles), centred on itself,
// in taps at most 1.5 pixels apart (jittered taps showed as dither after
// TAA). A tap counts where its own blur
// covers the pixel and it is in front, where the pixel's blur covers it
// and it is behind, or where both blur alike, so moving objects smear over
// what is behind them and stay sharp behind what is in front.

layout(set = 0, binding = 0) uniform sampler2D color;  // TAA's history: this frame's resolve
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D velocity;   // object motion in last frame's uv
layout(set = 0, binding = 3) uniform sampler2D tiles;

#include "lens.glsl"

layout(location = 0) out vec4 outColor;

const int kTaps = 32;

float Cone(float d, float r)
{
    return clamp(1.0 - d / r, 0.0, 1.0);
}

float Cylinder(float d, float r)
{
    return 1.0 - smoothstep(0.95 * r, 1.05 * r, d);
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 cx = texelFetch(color, p, 0);
    vec2 vmax = NeighborMax(tiles, p).xy;
    float lmax = length(vmax);
    if (lmax < 0.5) {
        outColor = cx;
        return;
    }
    float depthX = texelFetch(sceneDepth, p, 0).r;
    float zx = Distance(depthX);
    float rx = max(length(Motion(p, depthX, texelFetch(velocity, p, 0).xy)), 0.5);
    float softZ = max(0.01 * zx, 1e-4);
    float total = 1.0 / rx;
    vec4 sum = cx * total;
    ivec2 last = textureSize(sceneDepth, 0) - 1;
    for (int i = 0; i < kTaps; ++i) {
        float t = mix(-1.0, 1.0, (float(i) + 0.5) / float(kTaps));
        vec2 y = gl_FragCoord.xy + vmax * t;
        ivec2 q = clamp(ivec2(y), ivec2(0), last);
        float depthY = texelFetch(sceneDepth, q, 0).r;
        float zy = Distance(depthY);
        float ry = max(length(Motion(q, depthY, texelFetch(velocity, q, 0).xy)), 0.5);
        float d = abs(t) * lmax;
        float front = clamp(1.0 - (zy - zx) / softZ, 0.0, 1.0);
        float back = clamp(1.0 - (zx - zy) / softZ, 0.0, 1.0);
        float w = front * Cone(d, ry) + back * Cone(d, rx) + 2.0 * Cylinder(d, ry) * Cylinder(d, rx);
        sum += w * textureLod(color, y * pc.motion.zw, 0.0);
        total += w;
    }
    outColor = sum / total;
}
