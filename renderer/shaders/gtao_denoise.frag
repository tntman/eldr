#version 450

// 5x5 box over the raw GTAO, skipping taps whose distance to the camera
// differs from the center's by more than ~1.5% per pixel of offset, so
// occlusion doesn't bleed across depth edges.
// ponytail: distance-only weights reject most taps on surfaces seen at
// grazing angles, leaving some speckle there for TAA; a plane-distance
// weight (needs the normal) would keep them.

layout(set = 0, binding = 1) uniform sampler2D rawAo;  // visibility, distance (0 = background)

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
