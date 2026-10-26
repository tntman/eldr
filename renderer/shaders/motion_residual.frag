#version 450
#extension GL_GOOGLE_include_directive : require

// Residual motion blur between time samples (Renderer, Frame::
// motionBlurResidual): a film sample renders the scene at one time; this
// spreads it over the sample's stratum of the shutter, pc.window.xy (in
// units of the stratum, around the sample's time at 0), so the strata of
// all samples tile the shutter and the copies of a moving object blend
// into one smooth blur. Each tap asks what pixel x showed at a time d of
// the stratum: the content the neighbourhood's longest motion brings
// there (y = x - vmax d), if y's own motion does bring it to x; else x's
// own content, if x stands still; else (x's content moved off) y's, the
// nearest guess for what x uncovers.

layout(set = 0, binding = 0) uniform sampler2D color;  // this sample's HDR frame (a copy)
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D velocity;
layout(set = 0, binding = 3) uniform sampler2D tiles;

#include "lens.glsl"

layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    // Motion() is half the blur over pc.motion.x: here the motion over one
    // stratum, so the taps' displacement is twice it.
    vec2 vmax = 2.0 * NeighborMax(tiles, p).xy;
    float lmax = length(vmax);
    if (lmax < 1.0) {  // the strata leave no gaps
        outColor = texelFetch(color, p, 0);
        return;
    }
    ivec2 last = textureSize(sceneDepth, 0) - 1;
    vec2 own = 2.0 * Motion(p, texelFetch(sceneDepth, p, 0).r, texelFetch(velocity, p, 0).xy);
    int taps = clamp(int(ceil(1.5 * lmax)), 2, 48);
    vec4 sum = vec4(0.0);
    for (int i = 0; i < taps; ++i) {
        float d = mix(pc.window.x, pc.window.y, (float(i) + 0.5) / float(taps));
        vec2 y = gl_FragCoord.xy - vmax * d;
        ivec2 q = clamp(ivec2(y), ivec2(0), last);
        vec2 m = 2.0 * Motion(q, texelFetch(sceneDepth, q, 0).r, texelFetch(velocity, q, 0).xy);
        bool brought = length(y + m * d - gl_FragCoord.xy) < 1.0;
        bool still = length(own * d) < 0.5;
        sum += !brought && still ? texelFetch(color, p, 0) : textureLod(color, y * pc.motion.zw, 0.0);
    }
    outColor = sum / float(taps);
}
