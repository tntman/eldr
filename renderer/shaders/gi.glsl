// Ray tracing results and global illumination probes (raytracing.cpp),
// shared by mesh.frag, the ray tracing stages and the probe update. Include
// after frame.glsl.

// frame.features.x bits of these features.
const uint kFeatureRtShadows = 1048576u, kFeatureRtReflections = 2097152u, kFeatureGi = 4194304u;
const uint kFeatureSsgi = 8388608u;  // screen-space GI (gtao.frag's SSGI), no ray tracing needed

// std140 mirror of RayTracingData in raytracing.cpp.
layout(std140, set = 0, binding = 59) uniform RayTracingData {
    uvec4 shadowLights[4];  // the light each channel of the traced shadows is for, kNone after the last
    uvec4 params;        // x: frame (noise), y: shadow rays per light, z: reflection rays, w: light shadowed at hits
    vec4 giOrigin;       // xyz: the first probe, w: probe spacing
    uvec4 giCounts;      // probes along x, y, z; w: probe tiles per atlas row
    vec4 giParams;       // x: hysteresis this frame, y: rays per probe, z: visibility distance range
    mat4 giRotation;     // this frame's rotation of the probes' ray directions
    uvec4 skinned;       // x: the TLAS's first skinned instance (rt.glsl), y: how many
    vec4 shadowSample;   // xy: this sample's point on the light shapes (sample dimensions), w: layer stride (rows)
    uvec4 shadowInfo;    // x: shadow rays' cull mask (kRtMaskTranslucent: through translucent casters),
                         // y: volumes they march (0: none), z: lights per texel of the shadow image
                         // (4: one channel each; 1: colour, rgb)
    vec4 shadowVolume;   // x: step size (density voxels, VolumeParams), y: density multiplier
} rt;

// TLAS instance masks (tlas.comp): opaque and cutout meshes are seen by
// every ray, translucent ones only by shadow rays that tint.
const uint kRtMaskSurfaces = 1u, kRtMaskTranslucent = 2u;

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
// w: how far the probes apply, from their summed weight (0: none of them
// sees p, e.g. the outside of a thin wall: the caller's fallback instead).
vec4 GiIrradiance(vec3 p, vec3 n, vec3 v)
{
    float spacing = rt.giOrigin.w;
    ivec3 counts = ivec3(rt.giCounts.xyz);
    // Biased off the surface so it isn't shadowed by itself.
    vec3 biased = p + (n * 0.2 + v * 0.8) * (0.3 * spacing);
    // At grazing angles the view bias slides along the surface instead of
    // off it: keep it a fixed distance off, or a wall seen edge-on from the
    // probes' far side sits right at their visibility depth, and the
    // Chebyshev test there turns the visibility texels into stripes.
    biased += n * max(0.15 * spacing - dot(biased - p, n), 0.0);
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
    // Probes the visibility test rejects keep under ~0.005 each after the
    // crush; a probe that sees p keeps over ~0.2 (times its trilinear share).
    return weights > 1e-6 ? vec4(sum / weights, smoothstep(0.05, 0.15, weights)) : vec4(0.0);
}

#ifdef RT_RESULTS
// Screen-space GI: then the AO texture holds SSGI's visibility (r, used as
// AO: it hides the probes' or the dome's light, which fills the rest) and
// the light gathered from what screen-space rays hit (gba, irradiance /
// pi), which adds to it.
vec3 SsgiDiffuse()
{
    return Feature(kFeatureSsgi) ? texelFetch(ambientOcclusion, ivec2(gl_FragCoord.xy), 0).gba : vec3(0.0);
}

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
    float angle = 6.2831853 * SampleNoise(kDimSsr, gl_FragCoord.xy, float(frame.localLights.y));
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

// The traced shadow of `light` (its colour: what reaches p through
// translucent casters and volumes), if it has one (then it has no shadow
// map): unshadowed on translucent surfaces.
// ponytail: unfiltered (a fixed blur would soften hard and small shadows
// alike); a filter sized by the penumbra needs the blockers' distances.
bool RtTraced(uint light, out vec3 visibility)
{
    visibility = vec3(1.0);
    if (!Feature(kFeatureRtShadows)) {
        return false;
    }
    uint perTexel = rt.shadowInfo.z;
    for (uint k = 0u; k < 16u; ++k) {
        uint traced = rt.shadowLights[k >> 2u][k & 3u];
        if (traced == kNone) {
            break;
        }
        if (traced == light) {
            if (RtOpaque()) {
                vec4 v = imageLoad(rtShadowImage, ivec2(gl_FragCoord.xy) + ivec2(0, int(k / perTexel) * int(rt.shadowSample.w)));
                visibility = perTexel == 1u ? v.rgb : vec3(v[k & 3u]);
            }
            return true;
        }
    }
    return false;
}

// Split for mesh.frag's Shadow() and ShadowTint(): the brightest channel,
// and the colour over it.
bool RtShadow(uint light, out float visibility)
{
    vec3 v;
    bool traced = RtTraced(light, v);
    visibility = max(max(v.r, v.g), v.b);
    return traced;
}

bool RtShadowTint(uint light, out vec3 tint)
{
    vec3 v;
    bool traced = RtTraced(light, v);
    float m = max(max(v.r, v.g), v.b);
    tint = m > 0.0 ? v / m : vec3(1.0);
    return traced;
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
    return mix(fallback, gi.rgb, gi.a);
}
#endif
