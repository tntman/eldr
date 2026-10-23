#version 450

// Ground-truth ambient occlusion (Jimenez et al. 2016), following Intel's
// XeGTAO: per pixel, a few screen-space slices around the view vector; each
// finds the highest horizon on both sides within `radius` and integrates the
// cosine-weighted visible arc against the normal projected into the slice.
// Works in world space from the depth buffer; the normal is reconstructed
// from depth (the side with the smaller step, so silhouettes don't bend it).
// ponytail: no depth mip chain, so very large screen radii sample sparsely
// and thrash the cache; add XeGTAO's prefiltered depth if that shows up.

layout(set = 0, binding = 0) uniform sampler2D sceneDepth;  // window depth [0, 1]

layout(push_constant) uniform GtaoParams {
    mat4 invViewProj;  // NDC -> world (row-vector order)
    vec4 eye;          // camera position
    vec4 params;       // world radius, noise index, 1/width, 1/height
} pc;

layout(location = 0) out vec2 outAo;  // visibility, distance to the camera (0 = background)

const float PI = 3.14159265359;
const float HALF_PI = 1.57079632679;
const int kSlices = 3;
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

void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(sceneDepth, q, 0).r;
    if (depth >= 1.0) {
        outAo = vec2(1.0, 0.0);
        return;
    }
    ivec2 size = textureSize(sceneDepth, 0);
    vec2 uv = gl_FragCoord.xy * pc.params.zw;
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
        outAo = vec2(1.0, distanceToEye);
        return;
    }
    // Weight falls off over the outer 61.5% of the radius (XeGTAO defaults).
    float falloffRange = 0.615 * radius;
    float falloffMul = -1.0 / falloffRange;
    float falloffAdd = radius * (1.0 - 0.615) / falloffRange + 1.0;
    float minS = 1.3 / radiusPixels;  // never sample the center pixel

    float sliceNoise = Noise(gl_FragCoord.xy);
    float stepNoise = Noise(gl_FragCoord.yx + vec2(17.0, 43.0));
    float visibility = 0.0;
    for (int slice = 0; slice < kSlices; ++slice) {
        float phi = (float(slice) + sliceNoise) * PI / float(kSlices);
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
        for (int s = 0; s < kSteps; ++s) {
            float t = (float(s) + stepNoise) / float(kSteps);
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
                vec3 delta = WorldPos((vec2(sq) + 0.5) * pc.params.zw, sd) - p;
                float len = length(delta);
                float weight = clamp(len * falloffMul + falloffAdd, 0.0, 1.0);
                float c = dot(delta / max(len, 1e-20), v);
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
        float arc0 = (cosN + 2.0 * h0 * sin(angleN) - cos(2.0 * h0 - angleN)) / 4.0;
        float arc1 = (cosN + 2.0 * h1 * sin(angleN) - cos(2.0 * h1 - angleN)) / 4.0;
        visibility += mix(projectedLength, 1.0, 0.05) * (arc0 + arc1);
    }
    visibility = max(pow(clamp(visibility / float(kSlices), 0.0, 1.0), 2.2), 0.03);
    outAo = vec2(visibility, distanceToEye);
}
