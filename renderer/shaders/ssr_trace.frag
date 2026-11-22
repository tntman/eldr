#version 450
#extension GL_GOOGLE_include_directive : require

// Screen-space reflections at a lower resolution (renderer/ssr.h,
// Frame::movingSsrResolution): one ray per 2 x 2 block of pixels, from the
// block's first pixel, as mesh.frag traces it at full resolution. Drawn
// over the blocks after the shading pass, which stored per pixel what this
// needs (frame.ssr.z 2: a GGX-sampled ray for the denoiser, 3: the mirror
// ray). Writes the ray's radiance and pdf, and its direction, hit distance
// and GGX alpha per block.
// Interleaved (frame.ssr.z 4, Frame::ssrInterleave): the block's pixel
// changes every frame (pc.blocks.z) and its ray goes into the pixel's own
// texels of the full-resolution images, unless mesh.frag traced it.

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
    vec4 blocks;  // z: interleaved, 1 + the block's pixel (x + 2 y)
} pc;

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
    bool interleaved = pc.blocks.z > 0.0;
    int which = int(pc.blocks.z) - 1;
    ivec2 q = ivec2(gl_FragCoord.xy), px = 2 * q + (interleaved ? ivec2(which & 1, which >> 1) : ivec2(0));
    outRay = vec4(0.0);
    if (any(greaterThanEqual(px, imageSize(ssrFactor))) || imageLoad(ssrFactor, px).a <= 0.0) {
        return;  // nothing reflects here: the passes after skip the block
    }
    vec2 ndc = (vec2(px) + 0.5) / frame.viewport.xy * 2.0 - 1.0;
    vec3 p = Unproject(vec3(ndc, texelFetch(sceneDepth, px, 0).r * 2.0 - 1.0));
    vec3 v = ViewDirection(p);
    vec4 surface = imageLoad(ssrSurface, px), normals = imageLoad(ssrNormals, px);
    if (interleaved && surface.a > 0.0) {
        return;  // a smooth surface: mesh.frag traced it
    }
    vec3 n = OctDecode(normals.xy);
    float a = interleaved ? imageLoad(ssrDirections, px).w : surface.a, pdf = 1.0, cone = 2.0 * a;
    // Interleaved, each pixel has its own sequence of samples (every fourth frame's).
    vec2 noisePixel = interleaved ? vec2(px) : gl_FragCoord.xy;
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
    if (interleaved) {
        imageStore(ssrSurface, px, vec4(ray.rgb, max(pdf, 1e-4)));  // a pdf above 0: it has a ray
        imageStore(ssrDirections, px, direction);
    } else {
        outRay = ray;
        imageStore(ssrDirections, q, direction);
    }
}
