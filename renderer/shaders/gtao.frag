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

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;  // window depth [0, 1]

layout(push_constant) uniform GtaoParams {
    mat4 invViewProj;  // NDC -> world (row-vector order)
    vec4 eye;          // camera position; w: slices (directions per pixel)
    vec4 params;       // world radius, noise index, 1/width, 1/height (of the depth)
#ifdef SSGI
    mat4 prevViewProj;  // the camera of the history (last frame's resolved image)
    vec4 ssgi;          // x: pixels of depth per output pixel (1, 2), y: steps per side, z: 1 = history valid,
                        // w: brightest radiance taken (fireflies: highlights a few pixels wide)
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

// Interleaved gradient noise (Jimenez 2014), shifted per frame for TAA.
float Noise(vec2 p)
{
    p += 5.588238 * pc.params.y;
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

#ifdef SSGI
// What the camera saw at world point p last frame (0 off its image).
vec3 HistoryAt(vec3 p)
{
    vec4 clip = pc.prevViewProj * vec4(p, 1.0);
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (pc.ssgi.z <= 0.0 || clip.w <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        return vec3(0.0);
    }
    vec3 c = textureLod(history, uv, 0.0).rgb;
    float peak = max(max(c.r, c.g), c.b);
    return !(peak >= 0.0) ? vec3(0.0) : c * min(1.0, pc.ssgi.w / max(peak, 1e-20));
}

#define OUTPUT(visibility, distance, light) outSsgi = vec4(light, visibility)
#else
#define OUTPUT(visibility, distance, light) outAo = vec2(visibility, distance)
#endif

void main()
{
#ifdef SSGI
    ivec2 q = ivec2(gl_FragCoord.xy) * int(pc.ssgi.x);  // the depth pixel this output pixel stands for
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

    float sliceNoise = Noise(vec2(q) + 0.5);
    float stepNoise = Noise(vec2(q.yx) + 0.5 + vec2(17.0, 43.0));
    float visibility = 0.0;
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
        float sliceWeight = mix(projectedLength, 1.0, 0.05);
        for (int s = 0; s < steps; ++s) {
            float t = (float(s) + stepNoise) / float(steps);
            t = t * t + minS;  // denser near the center
            ivec2 offset = ivec2(round(t * radiusPixels * omega));
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
                float len = length(delta);
                float weight = clamp(len * falloffMul + falloffAdd, 0.0, 1.0);
                float c = dot(delta / max(len, 1e-20), v);
#ifdef SSGI
                // The arc this step hides, lit by what it hit.
                float before = side == 0 ? horizonCos0 : horizonCos1;
                float after = max(before, mix(side == 0 ? lowCos0 : lowCos1, c, weight));
                if (after > before) {
                    float sgn = side == 0 ? 1.0 : -1.0;
                    float h0 = angleN + clamp(sgn * acos(clamp(before, -1.0, 1.0)) - angleN, -HALF_PI, HALF_PI);
                    float h1 = angleN + clamp(sgn * acos(clamp(after, -1.0, 1.0)) - angleN, -HALF_PI, HALF_PI);
                    light += HistoryAt(hitPos) * (sliceWeight * abs(Arc(h0, angleN, cosN) - Arc(h1, angleN, cosN)));
                }
#endif
                if (side == 0) {
                    horizonCos0 = max(horizonCos0, mix(lowCos0, c, weight));
                } else {
                    horizonCos1 = max(horizonCos1, mix(lowCos1, c, weight));
                }
            }
        }

        float h0 = -acos(clamp(horizonCos1, -1.0, 1.0));
        float h1 = acos(clamp(horizonCos0, -1.0, 1.0));
        h0 = angleN + clamp(h0 - angleN, -HALF_PI, HALF_PI);
        h1 = angleN + clamp(h1 - angleN, -HALF_PI, HALF_PI);
        visibility += sliceWeight * (Arc(h0, angleN, cosN) + Arc(h1, angleN, cosN));
    }
#ifdef SSGI
    OUTPUT(clamp(visibility / float(slices), 0.0, 1.0), distanceToEye, light / float(slices));
#else
    visibility = max(pow(clamp(visibility / float(slices), 0.0, 1.0), 2.2), 0.03);
    OUTPUT(visibility, distanceToEye, light);
#endif
}
