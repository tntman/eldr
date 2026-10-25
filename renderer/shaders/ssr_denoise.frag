#version 450

// Denoised glossy SSR (renderer/ssr.h): the spatial ratio estimator, the
// temporal pass reprojecting through the reflected point, and the
// variance-guided bilateral filter adding the result to the colour: built
// once per pass (PASS 0, 1, 2), so each uses only the images it reads.

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;  // window depth [0, 1]
layout(set = 0, binding = 1) uniform sampler2D rays;        // radiance along the ray, its pdf
layout(set = 0, binding = 2) uniform sampler2D directions;  // ray direction (octahedral), hit distance (< 0: none), GGX alpha
layout(set = 0, binding = 3) uniform sampler2D normals;     // normal (octahedral)
layout(set = 0, binding = 4) uniform sampler2D factor;      // the reflection's weight (rgb); a: 1 = has a ray
layout(set = 0, binding = 5) uniform sampler2D resolved;    // the spatial pass's radiance, mean hit distance
layout(set = 0, binding = 6) uniform sampler2D history;     // last frame's temporal result (rgb, luma^2), linear
layout(set = 0, binding = 7) uniform sampler2D current;     // this frame's
layout(set = 0, binding = 8) uniform sampler2D blueNoise;   // 64 x 64 RGBA (Renderer's sample dimensions)

layout(push_constant) uniform DenoiseParams {
    mat4 invViewProj;   // NDC -> world (row-vector order)
    mat4 prevViewProj;  // world -> last frame's clip
    vec4 eye;           // camera position; w: noise index
    vec4 params;        // x: kDimSsrDenoise's value (< 0: gradient noise), history valid, 1/width, 1/height
} pc;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

vec3 WorldPos(ivec2 q)
{
    vec2 uv = (vec2(q) + 0.5) * pc.params.zw;
    vec4 p = pc.invViewProj * vec4(uv * 2.0 - 1.0, texelFetch(sceneDepth, q, 0).r * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

float Luma(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 ToYCoCg(vec3 c)
{
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 FromYCoCg(vec3 c)
{
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// GGX D times the height-correlated visibility times cos: this pixel's
// specular lobe for light from l (Fresnel is in the weight, factor).
float Lobe(vec3 n, vec3 v, vec3 l, float a)
{
    float NoL = dot(n, l), NoV = max(dot(n, v), 1e-4);
    if (NoL <= 0.0) {
        return 0.0;
    }
    vec3 h = normalize(v + l);
    float NoH = max(dot(n, h), 0.0), a2 = max(a * a, 1e-6);
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    float D = a2 / (PI * f * f);
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2), gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return D * 0.5 / (gv + gl) * NoL;
}

// The spatial kernel's rotation (the one noise lookup here): frame.glsl's
// SampleNoise for kDimSsrDenoise (10: channel b, shifted by 10 x (23, 41));
// without the sample dimensions interleaved gradient noise (Jimenez 2014)
// shifted per frame.
float Noise(vec2 p)
{
    if (pc.params.x >= 0.0) {
        return fract(texelFetch(blueNoise, (ivec2(p) + ivec2(230, 410)) & 63, 0).b + pc.params.x);
    }
    p += 5.588238 * pc.eye.w;
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

#if PASS == 0
// 1. Each pixel's reflection from its own and its neighbours' rays, each
// weighted by this pixel's lobe towards where it went over the pdf it was
// drawn with (the ratio estimator: noise falls, the lobe stays this
// pixel's). Rays from other surfaces don't count.
vec4 Spatial(ivec2 q)
{
    vec3 p = WorldPos(q), v = normalize(pc.eye.xyz - p);
    vec3 n = OctDecode(texelFetch(normals, q, 0).xy);
    float a = texelFetch(directions, q, 0).w;
    ivec2 maxQ = textureSize(sceneDepth, 0) - 1;
    float radius = mix(1.0, 6.0, clamp(sqrt(a) * 1.5, 0.0, 1.0));  // pixels, wider for rougher lobes
    float angle = 2.0 * PI * Noise(vec2(q));
    vec3 sum = vec3(0.0);
    float weights = 0.0, hitDistance = 0.0, hitWeights = 0.0;
    for (int k = 0; k < 9; ++k) {
        // The pixel itself, then 8 taps on a rotated spiral.
        float r = k == 0 ? 0.0 : radius * sqrt(float(k) / 8.0);
        float phi = angle + 2.39996323 * float(k);
        ivec2 s = clamp(q + ivec2(round(r * vec2(cos(phi), sin(phi)))), ivec2(0), maxQ);
        if (texelFetch(factor, s, 0).a <= 0.0) {
            continue;
        }
        vec3 ns = OctDecode(texelFetch(normals, s, 0).xy);
        vec3 ps = WorldPos(s);
        if (dot(ns, n) < 0.9 || abs(dot(ps - p, n)) > 0.02 * length(p - pc.eye.xyz)) {
            continue;
        }
        vec4 ray = texelFetch(rays, s, 0);
        vec4 dir = texelFetch(directions, s, 0);
        vec3 l = OctDecode(dir.xy);
        if (dir.z > 0.0) {
            l = normalize(ps + l * dir.z - p);  // towards its hit, from here
        }
        float w = Lobe(n, v, l, a) / max(ray.w, 1e-4);
        sum += ray.rgb * w;
        weights += w;
        if (dir.z > 0.0) {
            hitDistance += dir.z * w;
            hitWeights += w;
        }
    }
    vec3 c = weights > 0.0 ? sum / weights : texelFetch(rays, q, 0).rgb;
    return vec4(c, hitWeights > 0.0 ? hitDistance / hitWeights : -1.0);
}

#elif PASS == 1
// 2. Last frame's result where the reflected point was (the reflection's
// own parallax), clamped to this neighbourhood's spread in YCoCg; the
// luma's second moment rides along in alpha (the variance, pass 3).
vec4 Temporal(ivec2 q)
{
    vec4 c = texelFetch(resolved, q, 0);
    float luma = Luma(c.rgb);
    if (pc.params.y <= 0.0) {
        return vec4(c.rgb, luma * luma);
    }
    ivec2 maxQ = textureSize(sceneDepth, 0) - 1;
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 s = ToYCoCg(texelFetch(resolved, clamp(q + ivec2(x, y), ivec2(0), maxQ), 0).rgb);
            m1 += s;
            m2 += s * s;
        }
    }
    vec3 mean = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
    vec3 p = WorldPos(q), v = normalize(pc.eye.xyz - p);
    vec3 n = OctDecode(texelFetch(normals, q, 0).xy);
    // Where the reflection appears to be: behind the mirror by the hit
    // distance (the surface itself without a hit).
    vec3 reflected = c.a > 0.0 ? p - v * c.a : p;
    vec4 clip = pc.prevViewProj * vec4(reflected, 1.0);
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (clip.w <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec4(c.rgb, luma * luma);
    }
    vec4 h = textureLod(history, uv, 0.0);
    if (any(isnan(h)) || any(isinf(h))) {
        return vec4(c.rgb, luma * luma);
    }
    vec3 clamped = FromYCoCg(clamp(ToYCoCg(h.rgb), mean - sigma, mean + sigma));
    // The second moment follows the clamped history.
    float moment = h.a + Luma(clamped) * Luma(clamped) - Luma(h.rgb) * Luma(h.rgb);
    return vec4(mix(clamped, c.rgb, 0.1), mix(max(moment, 0.0), luma * luma, 0.1));
}

#else
// 3. A bilateral filter over the same surface, its luma weights scaled by
// the temporal variance (wide where still noisy, tight where converged),
// times the reflection's weight, added to the colour.
vec4 Composite(ivec2 q)
{
    vec4 f = texelFetch(factor, q, 0);
    if (f.a <= 0.0) {
        return vec4(0.0);
    }
    vec4 center = texelFetch(current, q, 0);
    float luma = Luma(center.rgb);
    float sigma = sqrt(max(center.a - luma * luma, 0.0)) + 1e-3;
    vec3 p = WorldPos(q);
    vec3 n = OctDecode(texelFetch(normals, q, 0).xy);
    ivec2 maxQ = textureSize(sceneDepth, 0) - 1;
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            ivec2 s = clamp(q + ivec2(x, y), ivec2(0), maxQ);
            if (texelFetch(factor, s, 0).a <= 0.0) {
                continue;
            }
            vec3 ns = OctDecode(texelFetch(normals, s, 0).xy);
            if (dot(ns, n) < 0.9 || abs(dot(WorldPos(s) - p, n)) > 0.02 * length(p - pc.eye.xyz)) {
                continue;
            }
            vec3 c = texelFetch(current, s, 0).rgb;
            float w = exp(-abs(Luma(c) - luma) / (2.0 * sigma)) * exp(-0.1 * float(x * x + y * y));
            sum += c * w;
            weights += w;
        }
    }
    vec3 c = weights > 0.0 ? sum / weights : center.rgb;
    return vec4(f.rgb * c, 0.0);
}

#endif

void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
#if PASS == 0
    vec4 result = texelFetch(factor, q, 0).a > 0.0 ? Spatial(q) : vec4(0.0, 0.0, 0.0, -1.0);
#elif PASS == 1
    vec4 result = Temporal(q);
#else
    vec4 result = Composite(q);
#endif
    outColor = any(isnan(result)) || any(isinf(result)) ? vec4(0.0) : result;
}
