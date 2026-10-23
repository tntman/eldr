#version 450

// Weighted blended OIT resolve (McGuire & Bavoil 2013): the translucent
// layers' weighted average colour, covering 1 - revealage of the pixel,
// premultiplied over the opaque colour by the pipeline's blend.

layout(set = 0, binding = 0) uniform sampler2D accum;      // sum of premultiplied colour, coverage (times weights)
layout(set = 0, binding = 1) uniform sampler2D revealage;  // product of 1 - coverage

layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
    float r = texelFetch(revealage, q, 0).r;
    vec4 a = texelFetch(accum, q, 0);
    if (r >= 1.0 || a.a <= 0.0) {
        discard;  // no translucent surface here
    }
    float coverage = 1.0 - clamp(r, 0.0, 1.0);
    outColor = vec4(a.rgb / a.a, 1.0) * coverage;
}
