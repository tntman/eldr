#version 450
#extension GL_GOOGLE_include_directive : require
#include "post.glsl"

// Bloom, exposure and tonemapping (ACES, AgX, AgX punchy or none), then
// compositing over the app's background colour by coverage. The output is
// display referred but still linear: the app applies the sRGB transfer
// (usdview's default colour correction does).
// a = (bloom intensity, bloom mip count, tonemapper (PostProcess::Settings::
// Tonemapper), 0), b = (1/width, 1/height), c = background colour
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

// AgX (Troy Sobotka), Benjamin Wrensch's fit: Rec.709 into AgX's inset
// primaries, log2 encoded over [-12.47, 4.03] EV around middle gray, a
// polynomial sigmoid, then the look (an ASC CDL: punchy = power 1.35,
// saturation 1.4, as Blender's) and back out. The sigmoid's output is
// display encoded; the 2.2 power returns it to linear for the app's sRGB.
const mat3 kAgxInset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
const mat3 kAgxOutset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);

vec3 Agx(vec3 c, bool punchy)
{
    const float minEv = -12.47393, maxEv = 4.026069;
    c = clamp(log2(max(kAgxInset * c, vec3(1e-10))), minEv, maxEv);
    vec3 x = (c - minEv) / (maxEv - minEv);
    vec3 x2 = x * x, x4 = x2 * x2;
    c = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    if (punchy) {
        float luma = Luminance(c);
        c = luma + 1.4 * (pow(max(c, vec3(0.0)), vec3(1.35)) - luma);
    }
    return pow(clamp(kAgxOutset * c, 0.0, 1.0), vec3(2.2));
}

vec3 Tonemap(vec3 c)
{
    int tonemapper = int(pc.a.z);
    return tonemapper == 1 ? Agx(c, false) : tonemapper == 2 ? Agx(c, true)
         : tonemapper == 3 ? max(c, vec3(0.0)) : AcesFitted(c);
}

void main()
{
    vec4 hdr = texelFetch(src, ivec2(gl_FragCoord.xy), 0);
    float coverage = clamp(hdr.a, 0.0, 1.0);
    // Light without coverage (a volume that emits but absorbs nothing)
    // goes over the background too: as it is without tonemapping and bloom
    // (the premultiplied over), else as if it covered the pixel as far as
    // it is bright.
    vec3 glow = max(hdr.rgb, 0.0);
    float bright = min(max(glow.r, max(glow.g, glow.b)), 1.0);
    if (coverage <= 0.0 && bright <= 0.0) {
        outColor = pc.c;  // background colour passes through exactly
        return;
    }
    if (int(pc.a.z) == 3 && pc.a.x <= 0.0) {
        outColor = vec4(glow * exp2(exposure.ev), coverage) + pc.c * (1.0 - coverage);
        return;
    }
    coverage = max(coverage, bright);
    vec3 c = hdr.rgb / coverage;
    if (pc.a.x > 0.0) {
        vec3 b = textureLod(bloom, gl_FragCoord.xy * pc.b.xy, 0.0).rgb / pc.a.y;
        c = mix(c, b, pc.a.x);
    }
    // ponytail: bloom doesn't spill onto the app's background colour.
    outColor = mix(pc.c, vec4(Tonemap(c * exp2(exposure.ev)), 1.0), coverage);
}
