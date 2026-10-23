#version 450
#extension GL_GOOGLE_include_directive : require
#include "post.glsl"

// Bloom composite, exposure and ACES tonemapping. The output is display
// referred but still linear: the app applies the sRGB transfer (usdview's
// default colour correction does).
// a = (bloom intensity, bloom mip count, count background, 0), b = (1/width, 1/height)
layout(location = 0) out vec4 outColor;

// Stephen Hill's fit of the ACES RRT + sRGB ODT (sRGB linear in and out).
// Matrices are column-major, i.e. the transposes of the published rows.
const mat3 kAcesInput = mat3(0.59719, 0.07600, 0.02840,
                             0.35458, 0.90834, 0.13383,
                             0.04823, 0.01566, 0.83777);
const mat3 kAcesOutput = mat3(1.60475, -0.10208, -0.00327,
                              -0.53108, 1.10813, -0.07276,
                              -0.07367, -0.00605, 1.07602);

vec3 AcesFitted(vec3 c)
{
    c = kAcesInput * c;
    vec3 a = c * (c + 0.0245786) - 0.000090537;
    vec3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
    return clamp(kAcesOutput * (a / b), 0.0, 1.0);
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 hdr = texelFetch(src, p, 0);
    // The app's background colour passes through untouched.
    if (IsBackground(p, pc.a.z)) {
        outColor = hdr;
        return;
    }
    vec3 c = hdr.rgb;
    if (pc.a.x > 0.0) {
        vec3 b = textureLod(bloom, gl_FragCoord.xy * pc.b.xy, 0.0).rgb / pc.a.y;
        c = mix(c, b, pc.a.x);
    }
    outColor = vec4(AcesFitted(c * exp2(exposure.ev)), hdr.a);
}
