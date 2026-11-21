#version 450

// Subsurface blur (renderer/subsurface.h), with one of two profiles.
// Burley (profile.x 1, one pass): Christensen and Burley's normalized
// diffusion R(r) = (e^-r/d + e^-r/3d) / (8 pi d r) per channel, over a disc
// of taps on the surface, their distances drawn from the largest
// channel's profile (exactly: a mix of two exponentials) and their angles
// on a golden-angle spiral turned per pixel and frame. Each channel weighs
// a tap by its profile over the draw's density, at the distance in the
// world, so light doesn't cross depth jumps.
// Gaussian (two passes): pass 0 blurs horizontally, pass 1 that
// vertically; taps spread over three times the largest radius (at most
// 128 pixels), each weighted per channel by a Gaussian of that channel's
// radius over its distance in the world.
// Both skip taps of other materials (ids) and add blurred minus unblurred
// light to the colour.
// ponytail: no normal-aware weights (no normal buffer here); the world
// distance stops the light at depth jumps.

layout(set = 0, binding = 0) uniform sampler2D src;      // pass 0: the light, 1: after pass 0
layout(set = 0, binding = 1) uniform sampler2D light;    // rgb: subsurface light, a: largest radius
layout(set = 0, binding = 2) uniform isampler2D ids;     // radii's proportions (10 bits each), bit 30 set
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;

layout(push_constant) uniform BlurParams {
    mat4 invViewProj;
    vec4 params;  // direction (pixels), taps, pass
    vec4 splits;  // x: 1 moves the light in the light splits too, yz: direct, indirect intensity
    vec4 profile;  // x: 1 Burley's, y: noise index
} pc;

layout(set = 0, binding = 4, rgba16f) uniform image2DArray lightSplits;  // 0: direct, 1: indirect diffuse, 5, 6: of them the subsurface's

layout(location = 0) out vec4 outColor;

vec3 World(ivec2 q, ivec2 size)
{
    float depth = texelFetch(sceneDepth, q, 0).r;
    vec4 p = pc.invViewProj * vec4((vec2(q) + 0.5) / vec2(size) * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

// Interleaved gradient noise (Jimenez 2014), shifted per frame.
float Noise(vec2 p)
{
    p += 5.588238 * pc.profile.y;
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The world size of a pixel step from q along `step`: the shorter of the
// two sides', so a silhouette's neighbour beyond doesn't count.
float PixelSize(ivec2 q, ivec2 step, ivec2 size, vec3 p)
{
    float a = length(World(clamp(q + step, ivec2(0), size - 1), size) - p);
    float b = length(World(clamp(q - step, ivec2(0), size - 1), size) - p);
    return max(min(a > 0.0 ? a : b, b > 0.0 ? b : a), 1e-12);
}

// Burley's profile over a disc of `taps` taps; d: each channel's
// scattering distance (world units).
vec3 Burley(ivec2 q, ivec2 size, int id, vec4 center, vec3 d, vec3 p, int taps)
{
    float dm = max(d.r, max(d.g, d.b));
    vec2 pixel = vec2(PixelSize(q, ivec2(1, 0), size, p), PixelSize(q, ivec2(0, 1), size, p));
    if (dm < 0.25 * min(pixel.x, pixel.y)) {
        return center.rgb;  // all of it within the pixel
    }
    const float kMaxPixels = 256.0;
    float jitter = Noise(vec2(q)), turn = 6.2831853 * Noise(vec2(q.y, q.x) + 17.0);
    vec3 sum = vec3(0.0), weights = vec3(0.0);
    for (int i = 0; i < taps; ++i) {
        // The largest channel's radial density, 0.25 e^-r/d / d +
        // 0.75 e^-r/3d / 3d, by its two lobes' inverse CDFs.
        float u = (float(i) + jitter) / float(taps);
        float r = u < 0.25 ? -dm * log(1.0 - u / 0.25) : -3.0 * dm * log(1.0 - (u - 0.25) / 0.75);
        float angle = float(i) * 2.3999632 + turn;
        vec2 o = r * vec2(cos(angle), sin(angle)) / pixel;
        if (dot(o, o) > kMaxPixels * kMaxPixels) {
            continue;
        }
        ivec2 t = q + ivec2(round(o));
        if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, size)) || texelFetch(ids, t, 0).r != id) {
            continue;
        }
        float rho = max(length(World(t, size) - p), r);
        // R_c(rho) 2 pi r / density(r); the 1/4 cancels.
        vec3 w = (r / rho) * (exp(-rho / d) + exp(-rho / (3.0 * d))) / d /
                 max((exp(-r / dm) + exp(-r / (3.0 * dm))) / dm, 1e-30);
        sum += w * texelFetch(src, t, 0).rgb;
        weights += w;
    }
    const float kCenter = 1e-4;  // the centre's, where every tap missed
    return (sum + kCenter * center.rgb) / (weights + kCenter);
}

void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(light, 0);
    int id = texelFetch(ids, q, 0).r;
    if (id < (1 << 30)) {
        discard;  // no subsurface material here
    }
    bool vertical = pc.params.w > 0.5;
    vec4 center = texelFetch(light, q, 0);
    vec3 radius = max(center.a * vec3(id & 1023, (id >> 10) & 1023, (id >> 20) & 1023) / 1023.0, vec3(1e-6));
    ivec2 dir = ivec2(pc.params.xy);
    vec3 p = World(q, size);
    float pixel = length(World(clamp(q + dir, ivec2(0), size - 1), size) - p);
    int taps = int(pc.params.z);
    float reach = min(3.0 * center.a / max(pixel, 1e-12), 128.0);  // pixels
    vec3 sum = texelFetch(src, q, 0).rgb;
    vec3 weights = vec3(1.0);
    vec3 result;
    if (pc.profile.x > 0.5) {
        result = Burley(q, size, id, center, radius, p, taps);
    } else if (reach >= 1.0 && taps > 1) {
        float spacing = reach / (0.5 * float(taps - 1));
        for (int i = 0; i < taps; ++i) {
            float o = (float(i) - 0.5 * float(taps - 1)) * spacing;
            ivec2 t = q + dir * int(round(o));
            if (t == q || any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, size)) ||
                texelFetch(ids, t, 0).r != id) {
                continue;
            }
            float d = length(World(t, size) - p);
            vec3 w = exp(-0.5 * d * d / (radius * radius));
            sum += w * texelFetch(src, t, 0).rgb;
            weights += w;
        }
    }
    if (pc.profile.x <= 0.5) {
        result = sum / weights;
    }
    if (vertical && pc.splits.x != 0.0) {
        // The same difference in the splits' diffuse light, by the shares
        // each held before the blur (all indirect where there was none).
        // ponytail: a shadow edge's direct light spreads as the sum does.
        vec4 direct = imageLoad(lightSplits, ivec3(q, 0)), indirect = imageLoad(lightSplits, ivec3(q, 1));
        float total = dot(direct.rgb + indirect.rgb, vec3(1.0));
        float share = total > 1e-6 ? clamp(dot(direct.rgb, vec3(1.0)) / total, 0.0, 1.0) : 0.0;
        // The colour carries the light path intensities, the splits don't.
        float scale = share * pc.splits.y + (1.0 - share) * pc.splits.z;
        vec3 moved = scale > 0.0 ? (result - center.rgb) / scale : vec3(0.0);
        imageStore(lightSplits, ivec3(q, 0), vec4(direct.rgb + share * moved, direct.a));
        imageStore(lightSplits, ivec3(q, 1), vec4(indirect.rgb + (1.0 - share) * moved, indirect.a));
        if (imageSize(lightSplits).z > 4) {  // all of it is the subsurface's (layers 5, 6)
            vec4 sssDirect = imageLoad(lightSplits, ivec3(q, 5)), sssIndirect = imageLoad(lightSplits, ivec3(q, 6));
            imageStore(lightSplits, ivec3(q, 5), vec4(sssDirect.rgb + share * moved, 1.0));
            imageStore(lightSplits, ivec3(q, 6), vec4(sssIndirect.rgb + (1.0 - share) * moved, 1.0));
        }
    }
    outColor = vertical ? vec4(result - center.rgb, 0.0) : vec4(result, 1.0);
}
