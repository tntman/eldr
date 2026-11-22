#version 450
#extension GL_GOOGLE_include_directive : require

// SsrDenoise's rays (renderer/ssr.h), traced after the shading pass, which
// stored per opaque pixel what this needs: here a ray costs a fraction of
// one in mesh.frag (no helper pixels, a small shader). frame.ssr.z 3: the
// mirror ray, else a GGX-sampled ray for the denoiser.
// Over the pixels (pc.blocks.w 2): every pixel's ray, into its texels.
// Interleaved (Frame::ssrInterleave): first over the pixels the smooth
// surfaces' rays (pc.blocks.w 1), then over 2 x 2 blocks of pixels the
// rough ones' (pc.blocks.z), from one pixel of each block, another each
// frame: the denoiser's spatial pass, wider than a block there, gives the
// other pixels their neighbours' rays.
// Per block (Frame::movingSsrResolution): the ray of each block's first
// pixel, into the block's texel (radiance and pdf: the colour output).

#include "frame.glsl"

layout(set = 0, binding = 11) uniform sampler2D ambientOcclusion;
layout(set = 0, binding = 12) uniform sampler2D previousColor;
layout(set = 0, binding = 76) uniform sampler2DArray previousSplits;
layout(set = 0, binding = 13) uniform sampler2D sceneDepth;
layout(set = 0, binding = 72, rgba16f) uniform image2D ssrSurface;     // the reflection without a hit, GGX alpha
layout(set = 0, binding = 73, rgba16f) uniform image2D ssrDirections;  // per block
layout(set = 0, binding = 74, rgba16f) uniform readonly image2D ssrNormals;       // normal, geometric normal (octahedral)
layout(set = 0, binding = 75, rgba16f) uniform readonly image2D ssrFactor;        // a > 0: reflects

#include "ssr_trace.glsl"

layout(location = 0) out vec4 outRay;

layout(push_constant) uniform DenoiseParams {  // ssr_denoise.frag's
    mat4 invViewProj;
    mat4 prevViewProj;
    vec4 eye;
    vec4 params;
    vec4 splits;
    vec4 blocks;  // z: interleaved, 1 + the block's pixel (x + 2 y); w: over the pixels, 1 smooth surfaces only
} pc;

// Interleaved: from this GGX alpha (perceptual roughness 0.2) a surface's
// rays trace per block (the spatial kernel is then at least 2.5 pixels wide).
const float kInterleavedAlpha = 0.04;

float SignNotZero(float x)
{
    return x >= 0.0 ? 1.0 : -1.0;
}

vec2 OctEncode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(SignNotZero(n.x), SignNotZero(n.y));
}

vec3 OctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * vec2(SignNotZero(n.x), SignNotZero(n.y));
    }
    return normalize(n);
}

void main()
{
    // The rays go into their pixels' own texels, or their blocks'.
    bool everyPixel = pc.blocks.w > 0.0, pixels = everyPixel || pc.blocks.z > 0.0;
    int which = max(int(pc.blocks.z) - 1, 0);
    ivec2 q = ivec2(gl_FragCoord.xy), px = everyPixel ? q : 2 * q + ivec2(which & 1, which >> 1);
    outRay = vec4(0.0);
    if (any(greaterThanEqual(px, imageSize(ssrFactor))) || imageLoad(ssrFactor, px).a <= 0.0) {
        return;  // nothing reflects here: the passes after skip the block
    }
    vec2 ndc = (vec2(px) + 0.5) / frame.viewport.xy * 2.0 - 1.0;
    vec3 p = Unproject(vec3(ndc, texelFetch(sceneDepth, px, 0).r * 2.0 - 1.0));
    vec3 v = ViewDirection(p);
    vec4 surface = imageLoad(ssrSurface, px), normals = imageLoad(ssrNormals, px);
    if (pixels && surface.a > 0.0) {
        return;  // it has its ray (a smooth surface's)
    }
    vec3 n = OctDecode(normals.xy);
    float a = pixels ? imageLoad(ssrDirections, px).w : surface.a, pdf = 1.0, cone = 2.0 * a;
    if (pc.blocks.w == 1.0 && a >= kInterleavedAlpha) {
        return;  // a rough surface: the blocks' turn
    }
    // Interleaved, each pixel has its own sequence of samples (every fourth frame's).
    vec2 noisePixel = pixels ? vec2(px) : gl_FragCoord.xy;
    vec3 l = reflect(-v, n);
    if (frame.ssr.z < 3.0) {
        // As mesh.frag's denoised ray.
        float NoV = max(dot(n, v), 1e-4);
        vec3 tx = normalize(cross(abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), n)), ty = cross(n, tx);
        vec2 u;
        if (frame.sampleInfo.x != 0u) {
            u = vec2(SampleNoise(kDimSsrGgx, noisePixel, 0.0), SampleNoise(kDimSsrGgx2, noisePixel, 0.0));
        } else {
            vec2 s = noisePixel + 5.588238 * frame.ssr.y;
            u = fract(52.9829189 * fract(vec2(dot(s, vec2(0.06711056, 0.00583715)),
                                              dot(s.yx + vec2(19.0, 47.0), vec2(0.06711056, 0.00583715)))));
        }
        vec3 h = SampleGgxVndf(vec3(dot(v, tx), dot(v, ty), NoV), a, u);
        h = tx * h.x + ty * h.y + n * h.z;
        l = reflect(-v, h);
        if (dot(l, OctDecode(normals.zw)) <= 0.0) {
            h = n;  // below the surface: the mirror direction
            l = reflect(-v, n);
        }
        float NoH = max(dot(n, h), 0.0), a2 = a * a, f = (NoH * a2 - NoH) * NoH + 1.0;
        pdf = a2 / (PI * f * f) * 2.0 / (NoV + sqrt(a2 + (1.0 - a2) * NoV * NoV)) / 4.0;
        cone = sqrt(1.0 / (9.0 * PI * max(pdf, 1e-6)));
    }
    vec4 hit;
    float w = TraceReflection(p, l, cone, hit);
    vec4 ray = vec4(mix(surface.rgb, hit.rgb + (1.0 - hit.a) * surface.rgb, w), pdf);
    vec4 direction = vec4(OctEncode(l), w > 0.0 ? ssrHitDistance : -1.0, a);
    if (pixels) {
        imageStore(ssrSurface, px, vec4(ray.rgb, max(pdf, 1e-4)));  // a pdf above 0: it has a ray
        imageStore(ssrDirections, px, direction);
    } else {
        outRay = ray;
        imageStore(ssrDirections, q, direction);
    }
}
