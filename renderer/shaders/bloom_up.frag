#version 450
#extension GL_GOOGLE_include_directive : require
#include "post.glsl"

// 3x3 tent upsample of the next smaller mip, added (blend ONE, ONE) onto
// this mip's downsampled content.
// a = (src texel size xy, radius in src texels, 0), b = (dst texel size xy)
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = gl_FragCoord.xy * pc.b.xy;
    vec2 t = pc.a.xy * pc.a.z;
    vec3 c = textureLod(src, uv + vec2(-t.x, t.y), 0.0).rgb
           + textureLod(src, uv + vec2(0.0, t.y), 0.0).rgb * 2.0
           + textureLod(src, uv + vec2(t.x, t.y), 0.0).rgb
           + textureLod(src, uv + vec2(-t.x, 0.0), 0.0).rgb * 2.0
           + textureLod(src, uv, 0.0).rgb * 4.0
           + textureLod(src, uv + vec2(t.x, 0.0), 0.0).rgb * 2.0
           + textureLod(src, uv + vec2(-t.x, -t.y), 0.0).rgb
           + textureLod(src, uv + vec2(0.0, -t.y), 0.0).rgb * 2.0
           + textureLod(src, uv + vec2(t.x, -t.y), 0.0).rgb;
    outColor = vec4(c / 16.0, 1.0);
}
