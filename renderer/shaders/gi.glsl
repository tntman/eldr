// Ray tracing results and global illumination probes (raytracing.cpp),
// shared by mesh.frag, the ray tracing stages and the probe update. Include
// after frame.glsl.

// frame.features.x bits of these features.
const uint kFeatureRtShadows = 1048576u, kFeatureRtReflections = 2097152u, kFeatureGi = 4194304u;

// std140 mirror of RayTracingData in raytracing.cpp.
layout(std140, set = 0, binding = 59) uniform RayTracingData {
    uvec4 shadowLights;  // the light each channel of the traced shadows is for, kNone: unused
    uvec4 params;        // x: frame (noise), y: shadow rays per light, z: reflection rays, w: light shadowed at hits
    vec4 giOrigin;       // xyz: the first probe, w: probe spacing
    uvec4 giCounts;      // probes along x, y, z; w: probe tiles per atlas row
    vec4 giParams;       // x: hysteresis this frame, y: rays per probe, z: visibility distance range
    mat4 giRotation;     // this frame's rotation of the probes' ray directions
} rt;

// DDGI probe atlases (Majercik et al. 2019): per probe an octahedral tile
// of irradiance / pi (rgb; a: 1 while the probe sees mostly front faces)
// and of visibility (mean hit distance and its square, over
// giParams.z), each with a one-texel border for bilinear filtering.
layout(set = 0, binding = 58) uniform sampler2D giAtlas[2];
const int kGiIrradianceTexels = 6, kGiVisibilityTexels = 14;  // tile interiors

float SignNotZero(float x)
{
    return x >= 0.0 ? 1.0 : -1.0;
}

// Unit vector to [-1, 1]^2, lower hemisphere folded into the corners.
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

// Atlas texel of a probe tile's corner (border included).
ivec2 GiTile(uint probe, int texels)
{
    return ivec2(probe % rt.giCounts.w, probe / rt.giCounts.w) * (texels + 2);
}

// The ray directions of every probe: a spherical Fibonacci set, rotated
// randomly each frame.
vec3 GiRayDirection(uint ray, uint rays)
{
    float golden = 2.39996323;  // pi (3 - sqrt 5)
    float z = 1.0 - (2.0 * float(ray) + 1.0) / float(rays);
    float r = sqrt(max(1.0 - z * z, 0.0));
    float phi = golden * float(ray);
    return mat3(rt.giRotation) * vec3(r * cos(phi), r * sin(phi), z);
}

// Irradiance / pi at p with normal n, seen from direction v, from the eight
// probes around it: trilinear weights, a soft backface weight and the
// Chebyshev test against each probe's visibility (no light through walls).
// w: 0 where no probe applies (keep the caller's fallback).
vec4 GiIrradiance(vec3 p, vec3 n, vec3 v)
{
    float spacing = rt.giOrigin.w;
    ivec3 counts = ivec3(rt.giCounts.xyz);
    // Biased off the surface so it isn't shadowed by itself.
    vec3 biased = p + (n * 0.2 + v * 0.8) * (0.3 * spacing);
    vec3 grid = (biased - rt.giOrigin.xyz) / spacing;
    ivec3 base = clamp(ivec3(floor(grid)), ivec3(0), counts - 2);
    vec3 alpha = clamp(grid - vec3(base), 0.0, 1.0);
    vec2 irradianceSize = vec2(textureSize(giAtlas[0], 0)), visibilitySize = vec2(textureSize(giAtlas[1], 0));
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 offset = ivec3(i, i >> 1, i >> 2) & 1;
        ivec3 c = base + offset;
        uint probe = uint(c.x + counts.x * (c.y + counts.y * c.z));
        vec3 probePos = rt.giOrigin.xyz + vec3(c) * spacing;
        vec3 toProbe = normalize(probePos - p);
        float wrap = (dot(toProbe, n) + 1.0) * 0.5;
        float w = wrap * wrap + 0.2;
        vec3 fromProbe = biased - probePos;
        float d = length(fromProbe);
        vec2 uv = (vec2(GiTile(probe, kGiVisibilityTexels)) + 1.0 +
                   (OctEncode(fromProbe / max(d, 1e-6)) * 0.5 + 0.5) * float(kGiVisibilityTexels)) / visibilitySize;
        vec2 moments = textureLod(giAtlas[1], uv, 0.0).rg;
        d /= rt.giParams.z;
        if (d > moments.x) {
            float variance = abs(moments.x * moments.x - moments.y);
            float chebyshev = variance / (variance + (d - moments.x) * (d - moments.x));
            w *= max(chebyshev * chebyshev * chebyshev, 0.05);
        }
        w = max(w, 1e-6);
        if (w < 0.2) {
            w *= w * w / 0.04;  // crush faint light leaks
        }
        vec3 trilinear = mix(1.0 - alpha, alpha, vec3(offset));
        uv = (vec2(GiTile(probe, kGiIrradianceTexels)) + 1.0 +
              (OctEncode(n) * 0.5 + 0.5) * float(kGiIrradianceTexels)) / irradianceSize;
        vec4 irradiance = textureLod(giAtlas[0], uv, 0.0);
        w *= trilinear.x * trilinear.y * trilinear.z * irradiance.a;
        sum += irradiance.rgb * w;
        weights += w;
    }
    return weights > 1e-6 ? vec4(sum / weights, 1.0) : vec4(0.0);
}

#ifdef RT_RESULTS
// mesh.frag: what the ray tracing pass traced for the opaque surface at
// this pixel. Translucent surfaces lie in front of it (closer than the
// prepass depth) and have no rays of their own.
layout(set = 0, binding = 53, rgba8) uniform readonly image2D rtShadowImage;
layout(set = 0, binding = 54, rgba16f) uniform readonly image2D rtReflectionImage;

bool RtOpaque()
{
    return gl_FragCoord.z >= texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
}

// Traced reflections around this pixel: up to 8 taps (kPoisson within
// `radius` pixels, rotated per pixel and frame) whose surfaces lie in this
// pixel's plane (p, n), averaged with it, before TAA averages the frames:
// a ray or a few per pixel are noisy on rough surfaces. Taps without a
// reflection (a = 0) don't count.
vec4 RtAround(vec3 p, vec3 n, float radius)
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 sum = imageLoad(rtReflectionImage, pixel);
    if (radius < 1.0) {
        return sum;
    }
    float weights = 1.0;
    float angle = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * float(frame.localLights.y),
                                                           vec2(0.06711056, 0.00583715))));
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    float tolerance = 0.02 * distance(p, frame.cameraPos.xyz);
    ivec2 size = imageSize(rtReflectionImage);
    for (int i = 0; i < 8; ++i) {
        ivec2 tap = clamp(pixel + ivec2(rotation * kPoisson[i] * radius), ivec2(0), size - 1);
        vec4 ndc = vec4((vec2(tap) + 0.5) / vec2(size) * 2.0 - 1.0, texelFetch(sceneDepth, tap, 0).r * 2.0 - 1.0, 1.0);
        vec4 q = frame.invViewProj * ndc;
        vec4 traced = imageLoad(rtReflectionImage, tap);
        if (traced.a > 0.0 && abs(dot(q.xyz / q.w - p, n)) < tolerance) {
            sum += traced;
            weights += 1.0;
        }
    }
    return sum / weights;
}

// The traced shadow of `light`, if it has one (then it has no shadow map):
// unshadowed on translucent surfaces.
// ponytail: unfiltered (a fixed blur would soften hard and small shadows
// alike); a filter sized by the penumbra needs the blockers' distances.
bool RtShadow(uint light, out float visibility)
{
    visibility = 1.0;
    if (!Feature(kFeatureRtShadows)) {
        return false;
    }
    for (int k = 0; k < 4; ++k) {
        if (rt.shadowLights[k] == light) {
            if (RtOpaque()) {
                visibility = imageLoad(rtShadowImage, ivec2(gl_FragCoord.xy))[k];
            }
            return true;
        }
    }
    return false;
}

// Traced reflections replace the dome's (and SSR) by their weight (a),
// which fades out over rough surfaces; rough ones are averaged over up to
// 12 pixels around.
vec3 RtReflection(vec3 specular, vec3 p, vec3 n, float perceptual)
{
    if (!Feature(kFeatureRtReflections) || !RtOpaque()) {
        return specular;
    }
    float weight = imageLoad(rtReflectionImage, ivec2(gl_FragCoord.xy)).a;
    return mix(specular, RtAround(p, n, 12.0 * smoothstep(0.05, 0.5, perceptual)).rgb, weight);
}

// Diffuse light from the probes where they cover p, else `fallback` (the
// dome's).
vec3 GiDiffuse(vec3 p, vec3 n, vec3 v, vec3 fallback)
{
    if (!Feature(kFeatureGi)) {
        return fallback;
    }
    vec4 gi = GiIrradiance(p, n, v);
    return gi.a > 0.0 ? gi.rgb : fallback;
}
#endif
