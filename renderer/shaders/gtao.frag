#version 450

// Ground-truth ambient occlusion (Jimenez et al. 2016), following Intel's
// XeGTAO: per pixel, a few screen-space slices around the view vector; each
// finds the highest horizon on both sides within `radius` and integrates the
// cosine-weighted visible arc against the normal projected into the slice.
// Works in world space from the depth buffer; the normal is reconstructed
// from depth (the side with the smaller step, so silhouettes don't bend it).
// ponytail: no depth mip chain, so very large screen radii sample sparsely
// and thrash the cache; add XeGTAO's prefiltered depth if that shows up.
//
// Built with SSGI: screen-space global illumination on the same horizon
// scan (in the spirit of EEVEE Next's): each step that raises a horizon
// hides the arc of the hemisphere between the old and the new horizon, and
// that arc's cosine-weighted share of light comes from what was hit: its
// colour in last frame's resolved image, reprojected. What stays visible
// is the visibility (no 2.2 curve: visible and hidden shares add up to 1)
// that the probes' or the dome's light fills in mesh.frag.
// ponytail: hit surfaces count as facing the pixel (no normal test), and
// their light is what the camera saw of them (view-dependent highlights too).
//
// Built with BITMASK too (Frame::ssgiBitmask): a visibility bitmask
// instead of the highest horizon (Therrien et al. 2023, EEVEE's fast GI).
// Each slice's hemisphere is 32 sectors; a sample hides the sectors
// between the angles of its front and of its back (thickness behind it),
// and only sectors no nearer sample hid yet take its light, weighted by
// how much its surface faces the pixel. Light passes behind thin objects
// and what one hides is lit by it alone. The sector grid is jittered per
// slice within a sector (no banding, for TAA to average).

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;  // window depth [0, 1]

layout(push_constant) uniform GtaoParams {
    mat4 invViewProj;  // NDC -> world (row-vector order)
    vec4 eye;          // camera position; w: slices (directions per pixel)
    vec4 params;       // world radius, noise index, 1/width, 1/height (of the depth)
#ifdef SSGI
    mat4 prevViewProj;  // the camera of the history (last frame's resolved image)
    vec4 ssgi;          // x: pixels of depth per output pixel (1, 2), y: steps per side, z: 1 = history valid,
                        // w: brightest radiance taken (fireflies: highlights a few pixels wide)
    vec4 temporal;      // (gtao_denoise.frag's); y: 1 = thickness estimator, z: object thickness (BITMASK)
    vec4 screen;        // x: back-face hits' share of light (Frame::screenBackface; < 0: off), y: the
                        // sector jitter's sample dimension (kDimSsgiJitter; < 0: off, from stepNoise),
                        // zw: the depth pixel of each ssgi.x-wide tile that traces (Frame::ssgiTileOrder)
#endif
} pc;

#ifdef SSGI
layout(set = 0, binding = 2) uniform sampler2D history;  // last frame's resolved HDR colour, premultiplied
layout(location = 0) out vec4 outSsgi;  // gathered irradiance / pi (rgb), visibility
#else
layout(location = 0) out vec2 outAo;  // visibility, distance to the camera (0 = background)
#endif

const float PI = 3.14159265359;
const float HALF_PI = 1.57079632679;
const int kSteps = 3;  // per side

vec3 WorldPos(vec2 uv, float depth)
{
    vec4 p = pc.invViewProj * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

vec3 PixelPos(ivec2 q)
{
    return WorldPos((vec2(q) + 0.5) * pc.params.zw, texelFetch(sceneDepth, q, 0).r);
}

layout(set = 0, binding = 4) uniform sampler2D blueNoise;  // 64 x 64 (Renderer's sample dimensions)

// The blue noise turned by this sample's value (params.y < 1, Renderer's
// sample dimensions), or interleaved gradient noise (Jimenez 2014) shifted
// per frame by index params.y - 1.
float Noise(vec2 p)
{
    if (pc.params.y < 1.0) {
        return fract(texelFetch(blueNoise, ivec2(p) & 63, 0).r + pc.params.y);
    }
    p += 5.588238 * (pc.params.y - 1.0);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Of the two one-pixel steps, the shorter one stays on this surface.
vec3 Tangent(vec3 p, ivec2 q, ivec2 step, ivec2 maxQ)
{
    vec3 a = PixelPos(clamp(q + step, ivec2(0), maxQ)) - p;
    vec3 b = p - PixelPos(clamp(q - step, ivec2(0), maxQ));
    float la = dot(a, a), lb = dot(b, b);
    return (la > 0.0 && la < lb) || lb == 0.0 ? a : b;
}

// The cosine-weighted share of a slice's hemisphere between the normal's
// side and horizon angle h (XeGTAO's integral).
float Arc(float h, float angleN, float cosN)
{
    return (cosN + 2.0 * h * sin(angleN) - cos(2.0 * h - angleN)) / 4.0;
}

#ifdef BITMASK
// The cosine-weighted share between angles a <= b (Arc measures from the
// view vector, 0, either way).
float Share(float a, float b, float angleN, float cosN)
{
    float fa = Arc(a, angleN, cosN), fb = Arc(b, angleN, cosN);
    return a >= 0.0 || b <= 0.0 ? abs(fb - fa) : fa + fb;
}
#endif

#ifdef SSGI
// What the camera saw at world point p last frame (0 off its image).
vec3 HistoryAt(vec3 p)
{
    vec4 clip = pc.prevViewProj * vec4(p, 1.0);
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (pc.ssgi.z <= 0.0 || clip.w <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec3(0.0);
    }
    // NaN/Inf in the history (max() may skip a NaN channel) is no light.
    vec3 c = textureLod(history, uv, 0.0).rgb;
    if (any(isnan(c)) || any(isinf(c))) {
        return vec3(0.0);
    }
    float peak = max(max(c.r, c.g), c.b);
    return peak <= 0.0 ? vec3(0.0) : c * min(1.0, pc.ssgi.w / peak);
}

// How much of the light of hit hitPos (depth pixel sq) leaves towards
// the pixel along `direction` (from it): 1 from its front, from its back
// (the ray went behind it; its normal from depth faces the camera) the
// back-face share (Frame::screenBackface), else `back`.
float Facing(vec3 hitPos, ivec2 sq, ivec2 size, vec3 direction, float back)
{
    vec3 hn = normalize(cross(Tangent(hitPos, sq, ivec2(1, 0), size - 1), Tangent(hitPos, sq, ivec2(0, 1), size - 1)));
    hn = dot(hn, pc.eye.xyz - hitPos) < 0.0 ? -hn : hn;
    if (any(isnan(hn))) {
        return 1.0;
    }
    return mix(pc.screen.x >= 0.0 ? pc.screen.x : back, 1.0, smoothstep(-0.1, 0.1, -dot(hn, direction)));
}

#define OUTPUT(visibility, distance, light) outSsgi = vec4(light, visibility)
#else
#define OUTPUT(visibility, distance, light) outAo = vec2(visibility, distance)
#endif

void main()
{
#ifdef SSGI
    // The depth pixel this output pixel stands for: at half resolution
    // one of its tile's, a different one each frame (Frame::ssgiTileOrder).
    ivec2 q = min(ivec2(gl_FragCoord.xy) * int(pc.ssgi.x) + ivec2(pc.screen.zw), textureSize(sceneDepth, 0) - 1);
    int steps = max(int(pc.ssgi.y), 1);
#else
    ivec2 q = ivec2(gl_FragCoord.xy);
    int steps = kSteps;
#endif
    vec3 light = vec3(0.0);
    float depth = texelFetch(sceneDepth, q, 0).r;
    if (depth >= 1.0) {
        OUTPUT(1.0, 0.0, light);
        return;
    }
    ivec2 size = textureSize(sceneDepth, 0);
    vec2 uv = (vec2(q) + 0.5) * pc.params.zw;
    vec3 surface = WorldPos(uv, depth);
    vec3 nearPos = WorldPos(uv, 0.0);       // same pixel ray on the near plane
    vec3 v = normalize(nearPos - surface);  // towards the camera, also for ortho
    vec3 n = normalize(cross(Tangent(surface, q, ivec2(1, 0), size - 1),
                             Tangent(surface, q, ivec2(0, 1), size - 1)));
    if (dot(n, v) < 0.0) {
        n = -n;
    }
    float distanceToEye = length(surface - pc.eye.xyz);
    // Nudged towards the camera against self-occlusion from depth precision.
    vec3 p = surface + (nearPos - surface) * 0.0008;

    float radius = pc.params.x;
    float pixelSize = length(WorldPos(uv + vec2(pc.params.z, 0.0), depth) - surface);
    float radiusPixels = radius / max(pixelSize, 1e-20);
    if (radiusPixels < 1.0 || any(isnan(n))) {
        OUTPUT(1.0, distanceToEye, light);
        return;
    }
    // Weight falls off over the outer 61.5% of the radius (XeGTAO defaults).
    float falloffRange = 0.615 * radius;
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = radius * (1.0 - 0.615) / falloffRange + 1.0;
    float minS = 1.3 / radiusPixels;  // never sample the center pixel

#ifdef SSGI
    // The thickness estimator (Frame::ssgiThickness): every step at least
    // a pixel on, and nothing within a pixel's footprint of this surface's
    // plane occludes it (depth precision at grazing angles).
    bool estimator = pc.temporal.y > 0.0;
#else
    const bool estimator = false;
#endif
    float sliceNoise = Noise(vec2(q) + 0.5);
    float stepNoise = Noise(vec2(q.yx) + 0.5 + vec2(17.0, 43.0));
    float visibility = 0.0, full = 0.0;
    int slices = max(int(pc.eye.w), 1);
    for (int slice = 0; slice < slices; ++slice) {
        float phi = (float(slice) + sliceNoise) * PI / float(slices);
        vec2 omega = vec2(cos(phi), sin(phi));  // pixels; +y is up like NDC
        // The slice plane holds the view vector and this screen direction.
        vec3 direction = normalize(WorldPos(uv + omega * pc.params.zw, depth) - surface);
        vec3 orthoDirection = direction - dot(direction, v) * v;
        vec3 axis = normalize(cross(direction, v));
        vec3 projectedNormal = n - axis * dot(n, axis);
        float projectedLength = length(projectedNormal);
        float cosN = clamp(dot(projectedNormal, v) / max(projectedLength, 1e-6), 0.0, 1.0);
        float angleN = sign(dot(orthoDirection, projectedNormal)) * acos(cosN);

        // Side 0 searches along +omega, side 1 along -omega.
        float lowCos0 = cos(angleN + HALF_PI), lowCos1 = cos(angleN - HALF_PI);
        float horizonCos0 = lowCos0, horizonCos1 = lowCos1;
#ifdef SSGI
        // SSGI's energy audit (after EEVEE 5.2's): the projected normal's
        // length, the cosine weight itself (a 5% floor made surfaces at
        // grazing angles up to 2.3% too bright), and the slices' sum
        // normalized by what they'd see unoccluded (`full`), so visibility
        // and hidden shares add up to exactly 1 at every pixel and sample:
        // a white furnace stays the dome's brightness, and no visibility
        // over 1 is clamped away (EEVEE renormalizes its SH alike).
        float sliceWeight = projectedLength;
        full += sliceWeight * (Arc(angleN - HALF_PI, angleN, cosN) + Arc(angleN + HALF_PI, angleN, cosN));
#else
        float sliceWeight = mix(projectedLength, 1.0, 0.05);
#endif
        float lastPixels = 0.0;
#ifdef BITMASK
        uint occluded = 0u;
        float hidden = 0.0;  // the cosine-weighted share of the occluded sectors
        // frame.glsl's SampleNoise for kDimSsgiJitter (11: channel a,
        // shifted by 11 x (23, 41)); without the sample dimensions the
        // steps' noise.
        float jitterNoise = pc.screen.y >= 0.0
                          ? fract(texelFetch(blueNoise, (q + ivec2(253, 451)) & 63, 0).a + pc.screen.y)
                          : stepNoise;
        float jitter = fract(jitterNoise + 0.618034 * float(slice));
#endif
        for (int s = 0; s < steps; ++s) {
            float t = (float(s) + stepNoise) / float(steps);
            t = t * t + minS;  // denser near the center
            float stepPixels = t * radiusPixels;
            if (estimator) {
                stepPixels = max(stepPixels, lastPixels + 1.0);
                lastPixels = stepPixels;
            }
            ivec2 offset = ivec2(round(stepPixels * omega));
            for (int side = 0; side < 2; ++side) {
                ivec2 sq = side == 0 ? q + offset : q - offset;
                if (any(lessThan(sq, ivec2(0))) || any(greaterThanEqual(sq, size))) {
                    continue;  // off screen: no occluder known
                }
                float sd = texelFetch(sceneDepth, sq, 0).r;
                if (sd >= 1.0) {
                    continue;
                }
                vec3 hitPos = WorldPos((vec2(sq) + 0.5) * pc.params.zw, sd);
                vec3 delta = hitPos - p;
                if (estimator && dot(delta, n) <= pixelSize * length(hitPos - pc.eye.xyz) / distanceToEye) {
                    continue;
                }
                float len = length(delta);
                float weight = clamp(len * falloffMul + falloffAdd, 0.0, 1.0);
                float c = dot(delta / max(len, 1e-20), v);
#ifdef BITMASK
                if (weight <= 0.0) {
                    continue;
                }
                // The angles of the sample's front and of its back, at
                // least a pixel's footprint and the object thickness
                // behind it, in the hemisphere around the normal.
                float sgn = side == 0 ? 1.0 : -1.0;
                vec3 toEye = normalize(pc.eye.xyz - hitPos);
                float thickness = max(pc.temporal.z, pixelSize * length(hitPos - pc.eye.xyz) / distanceToEye);
                vec3 back = hitPos - toEye * thickness;
                float hf = clamp(sgn * acos(clamp(c, -1.0, 1.0)) - angleN, -HALF_PI, HALF_PI);
                float hb = clamp(sgn * acos(clamp(dot(normalize(back - p), v), -1.0, 1.0)) - angleN, -HALF_PI, HALF_PI);
                float a0 = min(hf, hb), a1 = max(hf, hb);
                if (a1 <= a0) {
                    continue;  // wholly below the surface (both clamped): hides no sector
                }
                uint first = uint(clamp(floor((a0 / PI + 0.5) * 32.0 + jitter), 0.0, 32.0));
                uint last = uint(clamp(ceil((a1 / PI + 0.5) * 32.0 + jitter), 0.0, 32.0));
                uint count = last > first ? last - first : 0u;
                uint mask = count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u) << first;
                uint fresh = mask & ~occluded;
                if (fresh != 0u) {
                    float share = Share(angleN + a0, angleN + a1, angleN, cosN) * weight *
                                  float(bitCount(fresh)) / float(bitCount(mask));
                    hidden += share;
                    // Light leaves the hit towards the pixel from its
                    // front: its normal (from depth) must face it.
                    float facing = Facing(hitPos, sq, size, delta / max(len, 1e-20), 0.0);
                    light += HistoryAt(hitPos) * (sliceWeight * share * facing);
                    occluded |= mask;
                }
                continue;
#endif
#ifdef SSGI
                // The arc this step hides, lit by what it hit.
                float before = side == 0 ? horizonCos0 : horizonCos1;
                float after = max(before, mix(side == 0 ? lowCos0 : lowCos1, c, weight));
                if (after > before) {
                    float sgn = side == 0 ? 1.0 : -1.0;
                    float h0 = angleN + clamp(sgn * acos(clamp(before, -1.0, 1.0)) - angleN, -HALF_PI, HALF_PI);
                    float h1 = angleN + clamp(sgn * acos(clamp(after, -1.0, 1.0)) - angleN, -HALF_PI, HALF_PI);
                    float facing = pc.screen.x >= 0.0 ? Facing(hitPos, sq, size, delta / max(len, 1e-20), 1.0) : 1.0;
                    light += HistoryAt(hitPos) * (sliceWeight * facing * abs(Arc(h0, angleN, cosN) - Arc(h1, angleN, cosN)));
                }
#endif
                if (side == 0) {
                    horizonCos0 = max(horizonCos0, mix(lowCos0, c, weight));
                } else {
                    horizonCos1 = max(horizonCos1, mix(lowCos1, c, weight));
                }
            }
        }

#ifdef BITMASK
        visibility += sliceWeight * max(Arc(angleN - HALF_PI, angleN, cosN) + Arc(angleN + HALF_PI, angleN, cosN) - hidden, 0.0);
#else
        float h0 = -acos(clamp(horizonCos1, -1.0, 1.0));
        float h1 = acos(clamp(horizonCos0, -1.0, 1.0));
        h0 = angleN + clamp(h0 - angleN, -HALF_PI, HALF_PI);
        h1 = angleN + clamp(h1 - angleN, -HALF_PI, HALF_PI);
        visibility += sliceWeight * (Arc(h0, angleN, cosN) + Arc(h1, angleN, cosN));
#endif
    }
#ifdef SSGI
    full = max(full, 1e-6);
    light /= full;
    OUTPUT(clamp(visibility / full, 0.0, 1.0), distanceToEye,
           any(isnan(light)) || any(isinf(light)) ? vec3(0.0) : light);
#else
    visibility = max(pow(clamp(visibility / float(slices), 0.0, 1.0), 2.2), 0.03);
    OUTPUT(visibility, distanceToEye, light);
#endif
}
