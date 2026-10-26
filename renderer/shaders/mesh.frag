#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "fog.glsl"
#include "lights.glsl"
#include "probes.glsl"
#include "material.glsl"
#include "scene.glsl"
#define DISPLACE_FRAGMENT
#include "displace.glsl"

// Depth is read-only here (the prepass wrote it), so the depth test can run
// before the shader even though cutouts discard.
layout(early_fragment_tests) in;
layout(set = 0, binding = 4) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 5) uniform samplerCube envCube;
layout(set = 0, binding = 6) uniform sampler2D brdfLut;
layout(set = 0, binding = 11) uniform sampler2D ambientOcclusion;  // GTAO, 1 = unoccluded
layout(set = 0, binding = 12) uniform sampler2D previousColor;     // last frame's HDR result, premultiplied
layout(set = 0, binding = 76) uniform sampler2DArray previousSplits;  // last frame's light splits (frame.ssr.w)
layout(set = 0, binding = 13) uniform sampler2D sceneDepth;        // prepass depth, window [0, 1]

// Local lights' shadows (Renderer::_PlanLocalShadows): per light its first
// atlas tile; spots up to kSpotShadowCone have one, other lights six cube
// faces along +X, -X, +Y, -Y, +Z, -Z.
const float kSpotShadowCone = 1.3962634;
struct ShadowTile {
    mat4 viewProj;
    vec4 rect;    // atlas uv offset, uv scale
    vec4 params;  // x: shadow texel size per unit of distance from the light, y/z: near/far, w: light size
    vec4 origin;  // xyz: where the tile sees from (the light's centre, or its jittered point)
};
layout(set = 0, binding = 17) uniform sampler2DShadow shadowAtlas;
layout(std430, set = 0, binding = 18) readonly buffer LightShadows { uint lightShadow[]; };
layout(std430, set = 0, binding = 19) readonly buffer ShadowTiles { ShadowTile shadowTiles[]; };
layout(set = 0, binding = 20) uniform sampler2D shadowAtlasDepth;  // same atlas, raw depths
layout(set = 0, binding = 49) uniform sampler2DArray shadowMapDepth;  // the sun's cascades, raw depths
// Their nearest depths (shadow_min.comp: level 0 per 4 x 4 texels), read
// with a minimum-reduction sampler: the minimum of the bilinear footprint.
layout(set = 0, binding = 61) uniform sampler2DArray shadowMin;

const vec2 kPoisson[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2(0.34495938, 0.29387760), vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379), vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790));

#define RT_RESULTS
#include "gi.glsl"  // ray-traced shadows and reflections, GI probes

// mesh.vert's packed outputs.
layout(location = 0) in vec4 inWorldPosPrevX;
layout(location = 1) in vec4 inNormalPrevY;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inColorPrevZ;
// The visible-list entry: the instance slot (draw: material, prim id; and
// instance index), and while its level of detail changes the crossfade.
// Pipelines without the crossfade (kCompiledFeatures) see only plain slots.
layout(location = 4) flat in uint inEntry;
#define inSlot ((kCompiledFeatures & kFeatureLod) != 0u ? EntrySlot(inEntry) : inEntry)
#define inWorldPos (inWorldPosPrevX.xyz)
#define inNormal (inNormalPrevY.xyz)
#define inColor (inColorPrevZ.rgb)
#define inPrevWorldPos vec3(inWorldPosPrevX.w, inNormalPrevY.w, inColorPrevZ.w)
#define inMaterial (draws[instances[inSlot].draw].material)
#define inPrimId (draws[instances[inSlot].draw].primId)
#define inInstanceId (instances[inSlot].index)
#ifdef GRAPH
// Material programs: vertex-rate primvars (mesh.vert) and the interpreter.
layout(location = 5) in vec4 inPrimvars;
#include "program.glsl"
#endif

layout(location = 0) out vec4 outColor;
layout(location = 1) out int outPrimId;      // Hydra picking / selection
layout(location = 2) out int outInstanceId;
// The surface point's own motion since last frame, in uv as last frame's
// camera saw it (camera motion comes from depth in TAA). Static geometry
// writes exactly zero: both positions are the same bits.
layout(location = 3) out vec2 outVelocity;

// Light splits (aov.h): the visible opaque surface's direct and indirect
// diffuse and glossy light, layers in Aovs::kSplit* order. Only pipelines
// compiled with them (kCompiledLightSplits, a bit of the shading set's key
// that is no optional feature, Renderer::_ShadingSet) have this code.
layout(set = 0, binding = 67, rgba16f) uniform writeonly image2DArray lightSplits;
const uint kCompiledLightSplits = 1u;
// Light path intensity (Frame::directIntensity, indirectIntensity) scales
// the splits' direct and indirect light in the final pixel; it needs them
// too, so it shares their code and key bit.
bool LightPathScaled()
{
    return (kCompiledFeatures & kCompiledLightSplits) != 0u && frame.lightPath.z != 0.0;
}
bool LightSplits()
{
    return (kCompiledFeatures & kCompiledLightSplits) != 0u && (frame.aovs.x != 0u || frame.lightPath.z != 0.0);
}
vec3 splitDirectDiffuse = vec3(0.0);  // LocalLight's diffuse share
float fogTransmittance = 1.0;  // of the fog in front of the surface (main, below)

#include "brdf.glsl"
#include "transparency.glsl"
#include "subsurface.glsl"

vec4 Fetch(TexRef r, vec4 constant)
{
    return Fetch(r, constant, inUv);
}

// Tangent frame from screen-space derivatives (Mikkelsen 2010), so meshes
// need no tangent attribute.
mat3 CotangentFrame(vec3 n, vec3 p, vec2 uv)
{
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    return mat3(t * invmax, b * invmax, n);
}

float D_GGX(float NoH, float a)
{
    float a2 = a * a;
    float f = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (PI * f * f);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (gv + gl);
}

vec3 F_Schlick(vec3 f0, float VoH)
{
    return f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0);
}

// GGX specular times cos for light from direction l.
vec3 Specular(vec3 n, vec3 v, vec3 l, float a, vec3 f0, float NoV)
{
#ifdef LAYERS
    if (layers.anisotropy > 0.0) {
        return TangentSpecular(n, v, l, a, f0, NoV);  // brdf.glsl: anisotropic or hair
    }
#endif
    float NoL = clamp(dot(n, l), 0.0, 1.0);
    vec3 h = normalize(v + l);
    float NoH = max(dot(n, h), 0.0);
    float LoH = max(dot(l, h), 0.0);
    return D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, max(NoL, 1e-4), a) * F_Schlick(f0, LoH) * NoL;
}

// Illuminance of a sphere of unit radiance with sin^2 of its angular radius
// sinSigmaSqr, centered at cosTheta from the normal, clipped to the horizon
// (Lagarde & de Rousiers 2014, Frostbite).
float SphereIlluminance(float cosTheta, float sinSigmaSqr)
{
    if (cosTheta * cosTheta > sinSigmaSqr) {
        return PI * sinSigmaSqr * clamp(cosTheta, 0.0, 1.0);
    }
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 1e-8));
    float x = sqrt(max(1.0 / sinSigmaSqr - 1.0, 0.0));
    float y = -x * (cosTheta / sinTheta);
    float sinThetaSqrtY = sinTheta * sqrt(max(1.0 - y * y, 0.0));
    return max((cosTheta * acos(clamp(y, -1.0, 1.0)) - x * sinThetaSqrtY) * sinSigmaSqr +
               atan(sinThetaSqrtY / max(x, 1e-8)), 0.0);
}

// Distance along the tile's axis from window depth (GL perspective).
float ShadowDistance(float depth, float n, float f)
{
    return 2.0 * f * n / ((f + n) - (depth * 2.0 - 1.0) * (f - n));
}

// Shadow of a sphere/rect/disk light at p, normal offset as for the
// cascades. Points get 3x3 PCF; lights with a size get contact-hardening
// soft shadows (PCSS, Fernando 2005): the average depth of the blockers
// around the receiver sets the penumbra width by similar triangles, then
// Poisson PCF over it, rotated per pixel and frame for TAA to average.
// ponytail: blockers are searched assuming none closer than a quarter of
// the way to the receiver, and kernels are clamped to 24 texels and to the
// tile (cube face seams can show under very soft shadows).
float LocalShadow(uint index, Light light, vec3 p, vec3 ng)
{
    float traced;
    if (RtShadow(index, traced)) {
        return traced;
    }
    uint tile = lightShadow[index];
    if (tile == kNone) {
        return 1.0;
    }
    vec3 fromLight = p - shadowTiles[tile].origin.xyz;
    if (light.shaping.y > kSpotShadowCone) {
        vec3 a = abs(fromLight);
        tile += a.x >= a.y && a.x >= a.z ? (fromLight.x > 0.0 ? 0u : 1u)
              : a.y >= a.z               ? (fromLight.y > 0.0 ? 2u : 3u)
                                         : (fromLight.z > 0.0 ? 4u : 5u);
    }
    ShadowTile t = shadowTiles[tile];
    float dist = length(fromLight);
    float texel = t.params.x * dist;
    vec3 l = -fromLight / max(dist, 1e-8);
    float NoL = clamp(dot(ng, l), 0.0, 1.0);
    vec4 clip = t.viewProj * vec4(p + ng * texel * 1.5 * (1.0 - NoL) + l * texel * 0.5, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = t.rect.xy + (ndc.xy * 0.5 + 0.5) * t.rect.zw;
    float ref = ndc.z * 0.5 + 0.5;  // same z remap as the vertex shader
    float atlasSize = float(textureSize(shadowAtlas, 0).x);
    vec2 lo = t.rect.xy + 0.5 / atlasSize, hi = t.rect.xy + t.rect.zw - 0.5 / atlasSize;
    float size = t.params.w;
    if (size <= 0.0) {
        float sum = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                sum += texture(shadowAtlas, vec3(clamp(uv + vec2(x, y) / atlasSize, lo, hi), ref));
            }
        }
        return sum / 9.0;
    }

    float uvPerUnit = 1.0 / (t.params.x * atlasSize);  // atlas uv per unit of the plane at distance 1
    float receiver = clip.w;                           // distance along the tile's axis
    float maxUv = 24.0 / atlasSize;
    float angle = 6.2831853 * SampleNoise(kDimShadow, gl_FragCoord.xy, float(frame.localLights.y));
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));

    float searchUv = min(size * 3.0 / receiver * uvPerUnit, maxUv);
    int samples = int(frame.features.z);
    float blockers = 0.0, count = 0.0;
    for (int i = 0; i < samples; ++i) {
        vec2 s = clamp(uv + rotation * kPoisson[i] * searchUv, lo, hi);
        float d = texelFetch(shadowAtlasDepth, ivec2(s * atlasSize), 0).r;
        if (d < ref) {
            blockers += d;
            count += 1.0;
        }
    }
    if (count == 0.0) {
        return 1.0;
    }
    float blocker = ShadowDistance(blockers / count, t.params.y, t.params.z);
    float filterUv = clamp(size * (1.0 / blocker - 1.0 / receiver) * uvPerUnit, 1.0 / atlasSize, maxUv);
    float sum = 0.0;
    for (int i = 0; i < samples; ++i) {
        sum += texture(shadowAtlas, vec3(clamp(uv + rotation * kPoisson[i] * filterUv, lo, hi), ref));
    }
    return sum / float(samples);
}

// One edge's term of a polygon's form factor vector (corners relative to
// the shading point); the sum over a closed loop, / 2 pi, is the vector.
vec3 EdgeFormFactor(vec3 a, vec3 b)
{
    vec3 v0 = normalize(a), v1 = normalize(b);
    vec3 c = cross(v0, v1);
    float len = length(c);
    return len > 1e-8 ? acos(clamp(dot(v0, v1), -1.0, 1.0)) * c / len : vec3(0.0);
}

// Form factor vector of the rect toLight +- hw +- hh (seen from the
// shading point), pointing at it whatever the winding.
vec3 RectFormFactor(vec3 toLight, vec3 hw, vec3 hh)
{
    vec3 corners[4] = vec3[4](toLight + hw + hh, toLight - hw + hh, toLight - hw - hh, toLight + hw - hh);
    vec3 formFactor = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        formFactor += EdgeFormFactor(corners[i], corners[(i + 1) % 4]);
    }
    formFactor /= 2.0 * PI;
    return dot(formFactor, toLight) < 0.0 ? -formFactor : formFactor;
}

// Form factor vector of the side of a cylinder (axis toLight +- axis,
// radius r) facing the shading point: the three facets of the half hexagon
// prism inscribed in it there, whose sum is the form factor of their
// outline (within a few percent of the cylinder while r is under a tenth
// of the distance).
vec3 CylinderFormFactor(vec3 toLight, vec3 axis, float r)
{
    vec3 side = cross(axis, toLight);
    float len = length(side);
    if (len <= 1e-8) {
        return vec3(0.0);  // seen end-on: only the caps, which don't emit
    }
    side /= len;
    vec3 face = normalize(cross(side, axis));
    face = dot(face, toLight) > 0.0 ? -face : face;  // towards the shading point
    vec3 q[4] = vec3[4](toLight - r * side, toLight + r * (0.8660254 * face - 0.5 * side),
                        toLight + r * (0.8660254 * face + 0.5 * side), toLight + r * side);
    vec3 loop[8] = vec3[8](q[0] - axis, q[1] - axis, q[2] - axis, q[3] - axis,
                           q[3] + axis, q[2] + axis, q[1] + axis, q[0] + axis);
    vec3 formFactor = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        formFactor += EdgeFormFactor(loop[i], loop[(i + 1) % 8]);
    }
    formFactor /= 2.0 * PI;
    return dot(formFactor, toLight) < 0.0 ? -formFactor : formFactor;
}

// p into the axis-aligned ellipse of semi-axes ab (ab.x >= ab.y >= 0):
// radially for a round one; for an elongated one along the major axis, then
// within the minor's chord there (near the nearest point, also for a
// segment); blended by elongation, so continuous.
vec2 ClampEllipse(vec2 p, vec2 ab)
{
    vec2 radial = p / max(length(p / max(ab, 1e-6)), 1.0);
    float x = clamp(p.x, -ab.x, ab.x), c = ab.y * sqrt(max(1.0 - x * x / max(ab.x * ab.x, 1e-12), 0.0));
    return mix(radial, vec2(x, clamp(p.y, -c, c)), 1.0 - ab.y / max(ab.x, 1e-6));
}

// How much LightRegion widens the lobe along a light's major and minor axes
// (half-vector semi-axes A >= B of its region or equal-area ellipse, units
// of a). The representative point alone gives the highlight the light's
// shape with the lobe's edge; the true one is the light convolved with the
// lobe, wider by up to ~20% (most across a thin axis, which the inset
// collapses to a line). Fitted to that convolution (ellipses and boxes,
// A x B from 0.5 x 0.5 to 12 x 12): spreads within 5% for most, 8% at
// worst, peak and total kept.
vec2 RegionStretch(float A, float B, bool box)
{
    float minor = 1.0 + 1.05 * A * B / (A * B + 6.75), thin = 1.0 - B / max(A, 1e-6);
    return vec2(1.0 + (minor - 1.0) * (1.0 - thin), minor + (box ? 0.0 : 0.1 * thin));
}

// The stretched lobe of the last LightRegion/SphereRegion: widened by
// regionStretch along regionStretchDir (a tangent) and across it.
vec3 regionStretchDir = vec3(0.0);
vec2 regionStretch = vec2(1.0);

// GGX's D stretched as above over the plain one, for light from l.
float StretchRatio(vec3 n, vec3 v, vec3 l, float a)
{
    if (regionStretch == vec2(1.0) || regionStretchDir == vec3(0.0)) {
        return 1.0;
    }
    vec3 h = normalize(v + l);
    float hu = dot(h, regionStretchDir), hw = dot(h, cross(n, regionStretchDir)), hn = dot(h, n);
    float aa = a * a;
    float plain = (hu * hu + hw * hw) / aa + hn * hn;
    float stretched = (hu * hu / (regionStretch.x * regionStretch.x) + hw * hw / (regionStretch.y * regionStretch.y)) / aa +
                      hn * hn;
    return plain * plain / (stretched * stretched * regionStretch.x * regionStretch.y);
}

// The tangent along the half-vector direction (x along ePar, y along ePerp).
vec3 StretchTangent(vec3 n, vec3 ePar, vec3 ePerp, vec2 dir)
{
    vec3 t = dir.x * (ePar - dot(ePar, n) * n) + dir.y * (ePerp - dot(ePerp, n) * n);
    return dot(t, t) > 1e-12 ? normalize(t) : vec3(0.0);
}

// For an ellipse of semi-axes A >= B in half-vector space (units of a) and
// the lobe stretched by s (along A, B): returns N, the plain lobe's share
// within it (exact) over its area and the stretched lobe's peak, and sets
// mu, the inset (in the stretched space, where the lobe is round again) for
// which the lobe's integral beyond the inset region, area / pi + perimeter
// / 4 (Steiner, Ramanujan's perimeter: continuous down to a segment), is
// 1 / N - 1 (bisection; negative grows it).
float EllipseInset(float A, float B, vec2 s, out float mu)
{
    float norm = s.x * s.y / sqrt((1.0 + A * A) * (1.0 + B * B));
    float target = max(1.0 / norm - 1.0, 0.0), lo = -target, hi = A / s.x;
    for (int i = 0; i < 8; ++i) {
        mu = 0.5 * (lo + hi);
        float a = A / s.x - mu, b = max(B / s.y - mu, 0.0);
        float spread = a * b + 0.25 * PI * (3.0 * (a + b) - sqrt((3.0 * a + b) * (a + 3.0 * b)));
        lo = spread > target ? mu : lo;
        hi = spread > target ? hi : mu;
    }
    mu = 0.5 * (lo + hi);
    return norm;
}

// Energy-conserving representative point for a light with a size (Karis
// 2013's idea, normalized exactly instead of by (a / a')^2, which made small
// lights' highlights 1.2-4x too bright). The light is a region around
// toLight (its centre, at distance d) with half axes a1, a2, an ellipse or a
// box, of solid angle omega; uv is where the reflection ray passes in it, in
// half axes (inout: the representative point's). In half-vector space, where
// the GGX lobe is isotropic, the region is a1, a2 projected (/ 2 in the
// plane of v and l, / (2 VoH) across it; area omega / (4 VoH)):
// - the lobe is stretched along the region's axes (RegionStretch; set in
//   regionStretch, for StretchRatio), so the highlight's spread is the
//   light convolved with the lobe's, not the light's shape;
// - the returned N (the light's specular is Specular(l) * StretchRatio *
//   E * N) gives the reflection's centre exactly the plain lobe's share of
//   slopes within the region (the product of its axes' shares: exact for
//   ellipses, fitted within 2% for boxes), so mirrors reflect the light's
//   radiance;
// - l, the point of the region inset by mu nearest the ray (in the
//   stretched space; ellipses in their principal frame, ClampEllipse; boxes
//   in their own axes), gives the reflection's integral exactly the light's
//   power (Steiner's formula for the lobe around a convex region: area /
//   (pi a^2) + perimeter / (4 a) + 1 = 1 / N).
// Against brute-force GGX integrals: peaks, totals and spreads within ~5%
// (docs/features.md: lights.usda's floor, light_reflections.usda's sun and
// tube).
// regionArea: the region's half-vector area / pi, for the coat (LayerLight).
// ponytail: small-angle half-vector mapping around the representative
// point; LTC (Heitz 2016) would be exact for polygons of any size.
float LightRegion(vec3 n, vec3 v, float a, vec3 toLight, float d, vec3 a1, vec3 a2, inout vec2 uv, bool box,
                  float omega, out vec3 l, out float regionArea)
{
    vec2 c0 = box ? clamp(uv, -1.0, 1.0) : uv / max(length(uv), 1.0);
    vec3 l0 = normalize(toLight + mat2x3(a1, a2) * c0);
    float VoH = max(dot(v, normalize(v + l0)), 1e-3);
    vec3 ePar = v - dot(v, l0) * l0;
    float len = length(ePar);
    ePar = len > 1e-5 ? ePar / len
                      : normalize(cross(l0, abs(l0.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
    vec3 ePerp = cross(l0, ePar);
    // The half axes in half-vector space, in units of a, scaled to the
    // exact solid angle (the projection is linear), but a line or a point
    // keeps its own limit.
    mat2 m = mat2(vec2(dot(a1, ePar), dot(a1, ePerp) / VoH), vec2(dot(a2, ePar), dot(a2, ePerp) / VoH)) / (2.0 * d);
    float g = box ? 4.0 : PI;
    float cr = abs(determinant(m));
    float s2 = cr > 1e-12 ? min(omega / (4.0 * VoH * g * cr), 4.0) : 1.0;
    regionArea = g * cr * s2 / PI;
    m *= sqrt(s2) / a;
    float norm;
    if (box) {
        // Stretched along the box's first axis and across it, by its
        // equal-area ellipse's stretch.
        float x = max(length(m[0]), 1e-3), y = max(length(m[1]), 1e-3);
        vec2 s = RegionStretch(1.1283792 * max(x, y), 1.1283792 * min(x, y), true);
        s = x >= y ? s : s.yx;
        regionStretch = s;
        regionStretchDir = StretchTangent(n, ePar, ePerp, m[0] / x);
        float share = x * inversesqrt(1.0 + x * x) * y * inversesqrt(1.0 + y * y) *
                      (1.0 + (4.0 / PI - 1.0) * pow((1.0 + x * x) * (1.0 + y * y), -0.75));
        norm = share * s.x * s.y / (4.0 / PI * x * y);
        // Inset by mu (stretched): area 4 (x - mu)(y - mu), perimeter
        // 4 (x + y - 2 mu). Where that would swallow the shorter half axis
        // (a thin box), the lobe is stretched across it only as far as
        // keeps the energy with the inset at that axis (a segment of the
        // longer one's rest, whose spread is its length): s = (4 x y / (pi
        // share) + y S) / (x + S), the longer x stretched by S.
        vec2 q = vec2(x, y) / s;
        float target = max(1.0 / norm - 1.0, 0.0);
        float qa = 4.0 / PI, qb = -(qa * (q.x + q.y) + 2.0), qc = qa * q.x * q.y + q.x + q.y - target;
        float mu = 2.0 * qc / (-qb + sqrt(max(qb * qb - 4.0 * qa * qc, 0.0)));
        if (mu > min(q.x, q.y)) {
            bool longX = x >= y;
            float L = longX ? x : y, W = longX ? y : x, S = longX ? s.x : s.y;
            float across = max((4.0 / PI * x * y / max(share, 1e-12) + W * S) / (L + S), 1.0);
            s = longX ? vec2(S, across) : vec2(across, S);
            regionStretch = s;
            norm = share * s.x * s.y / (4.0 / PI * x * y);
            q = vec2(x, y) / s;
            mu = min(q.x, q.y);
        }
        vec2 rho = max(1.0 - mu / q, 1e-4);
        uv = clamp(uv, -rho, rho);
    } else {
        // In the principal frame (semi-axes A >= B, A along e1): the half
        // axes are conjugate there, skewed by the projection.
        mat2 gram = outerProduct(m[0], m[0]) + outerProduct(m[1], m[1]);
        float tr = gram[0][0] + gram[1][1], det = determinant(m) * determinant(m);
        float disc = sqrt(max(tr * tr * 0.25 - det, 0.0));
        float A = sqrt(max(tr * 0.5 + disc, 1e-6)), B = sqrt(max(tr * 0.5 - disc, 1e-6));
        // A's direction: the longer of the eigenvector's two forms (the
        // other is rounding noise when the axis is near x or y).
        vec2 ex = vec2(A * A - gram[1][1], gram[1][0]), ey = vec2(gram[1][0], A * A - gram[0][0]);
        vec2 e1 = dot(ex, ex) >= dot(ey, ey) ? ex : ey;
        e1 = dot(e1, e1) > 1e-20 ? normalize(e1) : vec2(1.0, 0.0);
        vec2 s = RegionStretch(A, B, false);
        regionStretch = s;
        regionStretchDir = StretchTangent(n, ePar, ePerp, e1);
        float mu;
        norm = EllipseInset(A, B, s, mu);
        if (abs(determinant(m)) > 1e-12) {
            mat2 frame = mat2(e1, vec2(-e1.y, e1.x));  // principal to m's space
            vec2 p = transpose(frame) * (m * uv) / s;
            uv = inverse(m) * (frame * (s * ClampEllipse(p, vec2(A / s.x - mu, max(B / s.y - mu, 0.0)))));
        }
    }
    l = normalize(toLight + mat2x3(a1, a2) * uv);
    return norm;
}

// A sphere (or the sun's disc, at d = 1) of radius R seen from p, as
// LightRegion's ellipse: the disc facing p, whose half axes in half-vector
// space are already principal (R / (2 d) in the plane of v, R / (2 d VoH)
// across it), so it needs no eigenvectors (and keeps the sun's path in
// every shader light).
float SphereRegion(vec3 n, vec3 v, vec3 r, float a, vec3 toLight, float d, float R, float omega, out vec3 l,
                   out float regionArea)
{
    vec3 w = toLight / d, e = v - dot(v, w) * w;
    e = dot(e, e) > 1e-10 ? normalize(e)
                          : normalize(cross(w, abs(w.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
    vec3 f = cross(w, e);
    vec3 centerToRay = dot(toLight, r) * r - toLight;
    vec2 uv = vec2(dot(centerToRay, e), dot(centerToRay, f)) / max(R, 1e-12);  // in radii
    vec3 l0 = normalize(toLight + R * mat2x3(e, f) * (uv / max(length(uv), 1.0)));
    float VoH = max(dot(v, normalize(v + l0)), 1e-3);
    // Semi-axes in units of a, scaled to the exact solid angle; the major
    // axis across (f), the minor in the plane of v (e).
    float s = R > 0.0 ? sqrt(min(omega * d * d / (PI * R * R), 4.0)) : 1.0;
    float B = max(s * R / (2.0 * d * a), 1e-3), A = max(B / VoH, 1e-3);
    regionArea = A * B * a * a;
    vec2 st = RegionStretch(A, B, false);
    vec3 across = f - dot(f, n) * n;
    regionStretch = st;
    regionStretchDir = dot(across, across) > 1e-12 ? normalize(across) : vec3(0.0);
    float mu, norm = EllipseInset(A, B, st, mu);
    vec2 q = st * ClampEllipse(vec2(uv.y * A, uv.x * B) / st, vec2(A / st.x - mu, max(B / st.y - mu, 0.0)));
    uv = vec2(q.y / B, q.x / A);
    l = normalize(toLight + R * mat2x3(e, f) * uv);
    return norm;
}

// Sphere, rect, disk or cylinder light (UsdLux semantics as in hdEmbree:
// cone and focus shaping around the emission axis; rects and disks emit
// from their -Z side, cylinders from their sides). Diffuse: Frostbite's
// horizon-clipped sphere/disk illuminance; for rects (and cylinders' rect
// silhouettes) the exact polygon form factor vector with Heitz's horizon
// approximation. Specular: LightRegion's representative point.
vec3 LocalLight(uint index, vec3 p, vec3 n, vec3 ng, vec3 v, vec3 diffuseColor, vec3 f0, float a,
                float NoV, vec3 energyCompensation)
{
    Light light = lights[index];
    vec3 toLight = light.position.xyz - p;
    float d = length(toLight);
    float window = clamp(1.0 - pow(d / LightRange(light), 4.0), 0.0, 1.0);
    if (window <= 0.0 || d <= 0.0) {
        return vec3(0.0);
    }
    vec3 radiance = light.color.rgb * window * window;
    float cosOff = dot(-toLight / d, light.axis.xyz);
    if (light.shaping.z > 0.0) {
        radiance *= mix(light.focusTint.rgb, vec3(1.0), pow(abs(cosOff), light.shaping.z));
    }
    float theta = acos(clamp(cosOff, -1.0, 1.0));
    radiance *= light.shaping.y <= light.shaping.x ? (theta <= light.shaping.y ? 1.0 : 0.0)
                                                   : 1.0 - smoothstep(light.shaping.x, light.shaping.y, theta);

    vec3 r = reflect(-v, n);
    float diffuseE, specularE, norm, regionArea;
    vec3 l;
    vec3 diffuseTint = vec3(1.0), specularTint = vec3(1.0);  // a rect or disk light's texture
    if (uint(light.position.w) == kLightSphere) {
        float sinSigmaSqr = min(light.axis.w * light.axis.w / (d * d), 1.0);
        diffuseE = SphereIlluminance(dot(n, toLight / d), sinSigmaSqr);
        specularE = PI * sinSigmaSqr;
        norm = SphereRegion(n, v, r, a, toLight, d, light.halfWidth.w, specularE, l, regionArea);
    } else if (uint(light.position.w) == kLightCylinder) {
        // Diffuse: the form factor of its side facing p (the caps don't
        // emit). Specular: its silhouette as a box, the axis by the side
        // facing p: the point of the axis closest to the reflection ray
        // (Karis 2013), towards it by up to the radius.
        vec3 axis = light.halfWidth.xyz;
        vec3 formFactor = CylinderFormFactor(toLight, axis, light.axis.w);
        float f = length(formFactor);
        diffuseE = PI * max((f * f + dot(formFactor, n)) / (f + 1.0), 0.0);
        specularE = PI * f;
        vec3 side = cross(axis, toLight);
        side = dot(side, side) > 1e-16 ? normalize(side) * light.halfWidth.w : vec3(0.0);
        float ra = dot(r, axis);
        float t = (dot(r, toLight) * ra - dot(toLight, axis)) / max(dot(axis, axis) - ra * ra, 1e-8);
        vec3 closest = toLight + axis * clamp(t, -1.0, 1.0);
        vec3 centerToRay = dot(closest, r) * r - closest;
        vec2 uv = vec2(t, dot(centerToRay, side) / max(dot(side, side), 1e-12));
        norm = LightRegion(n, v, a, toLight, d, axis, side, uv, true, specularE, l, regionArea);
    } else {
        if (dot(toLight, light.axis.xyz) >= 0.0) {
            return vec3(0.0);  // behind the emitting side
        }
        vec3 hw = light.halfWidth.xyz, hh = light.halfHeight.xyz;
        bool rect = uint(light.position.w) == kLightRect;
        if (rect) {
            vec3 formFactor = RectFormFactor(toLight, hw, hh);
            float f = length(formFactor);
            diffuseE = PI * max((f * f + dot(formFactor, n)) / (f + 1.0), 0.0);
            specularE = PI * f;
        } else {
            float r2 = length(cross(hw, hh));  // radius^2 of the equal-area circle
            float cosLight = clamp(dot(light.axis.xyz, -toLight / d), 0.0, 1.0);
            float sinSigmaSqr = r2 / (r2 + d * d);
            diffuseE = SphereIlluminance(dot(n, toLight / d), sinSigmaSqr) * cosLight;
            specularE = PI * sinSigmaSqr * cosLight;
        }
        // Where the reflection ray meets the light's plane (in units of its
        // half extents); LightRegion clamps it to the rectangle or disk.
        float rDotAxis = dot(r, light.axis.xyz);
        vec3 q = rDotAxis < -1e-4 ? r * (dot(toLight, light.axis.xyz) / rDotAxis) - toLight : vec3(0.0);
        vec2 uv = vec2(dot(q, hw) / dot(hw, hw), dot(q, hh) / dot(hh, hh));
        norm = LightRegion(n, v, a, toLight, d, hw, hh, uv, rect, specularE, l, regionArea);
        int tex = int(light.focusTint.w);
        if (tex >= 0 && (frame.reflections.z & kLightTextures) != 0u) {
            // The texture (s = 0 at +X as UsdLux says; the image's top row at +Y, as Karma, i.e. t = 1: rows are stored bottom-up) where the specular representative point is, and
            // above p (its foot on the plane) for diffuse, each at the mip
            // of what it gathers: the lobe's footprint at the light (its
            // radius in light directions, 2 a, stretched as LightRegion
            // stretched it), and the light seen from its height over the
            // plane.
            // ponytail: two taps; no prefiltered form factor (Heitz's
            // filtered polygon lights would get the wide lobes right).
            float extent = length(hw) + length(hh);
            float texels = float(max(textureSize(textures[nonuniformEXT(tex)], 0).x,
                                     textureSize(textures[nonuniformEXT(tex)], 0).y));
            float aLobe = min(2.0 * a * max(regionStretch.x, regionStretch.y), 1.0);
            float lobeLod = log2(texels * clamp(aLobe * d / extent, 1.0 / texels, 1.0));
            specularTint = textureLod(textures[nonuniformEXT(tex)], vec2(0.5, 0.5) + uv * vec2(-0.5, 0.5), lobeLod).rgb;
            vec2 foot = vec2(dot(-toLight, hw) / dot(hw, hw), dot(-toLight, hh) / dot(hh, hh));
            foot = rect ? clamp(foot, -1.0, 1.0) : foot / max(length(foot), 1.0);
            float height = abs(dot(toLight, light.axis.xyz));
            float footLod = log2(texels * clamp(height / extent, 1.0 / texels, 1.0));
            diffuseTint = textureLod(textures[nonuniformEXT(tex)], vec2(0.5, 0.5) + foot * vec2(-0.5, 0.5), footLod).rgb;
        }
    }
    vec3 diffuse = diffuseColor / PI * diffuseE * light.halfHeight.w * diffuseTint;
    vec3 specular = Specular(n, v, l, a, f0, NoV) * StretchRatio(n, v, l, a) * energyCompensation * specularE * norm *
                    light.shaping.w;
    specular += LayerLight(n, v, l, NoV, regionArea) * specularE * light.shaping.w;
    specular *= specularTint;
    if (LightSplits()) {
        splitDirectDiffuse += radiance * diffuse * LocalShadow(index, light, p, ng) * LocalShadowTint(index, light, p, ng);
    }
    return radiance * (diffuse + specular) * LocalShadow(index, light, p, ng) * LocalShadowTint(index, light, p, ng);
}

// Soft sun shadows: PCSS in the cascade's light space, as LocalShadow()
// but with the sun's angular radius (tan, `tanHalf`) and a parallel
// projection: penumbra radius = blocker-to-receiver distance * tanHalf.
// Every tap compares against the receiver's plane there (the geometric
// normal's slope in light space), so wide kernels don't shadow the surface
// itself. Blockers are searched on a spiral whose radius grows
// geometrically from 1.5 texels to as far as the penumbra of one at the
// light box's near side reaches, and only count where their own penumbra
// reaches the receiver: every scale gets taps, and a wide search doesn't
// soften contact shadows. The search stops where no blocker's penumbra can
// reach from: shadowMin bounds how near the light the blockers out to the
// last tap are, so how high above the receiver's plane, so how far their
// penumbrae reach. The taps up to there are the same, so is the result
// (for a 0.5 degree sun over a crowd the search is 1-4 taps, not 16).
// uv/ref are the hard lookup's, c its cascade.
// ponytail: kernels clamped to 5% of the cascade; no blend between cascades.
float SoftSunShadow(int c, vec2 uv, float ref, vec3 ng, vec3 l, float tanHalf)
{
    mat4 m = frame.cascadeViewProj[c];
    vec3 gx = vec3(m[0][0], m[1][0], m[2][0]), gy = vec3(m[0][1], m[1][1], m[2][1]);
    vec3 gz = vec3(m[0][2], m[1][2], m[2][2]);  // NDC per world unit (orthographic)
    float depthRange = 2.0 / length(gz);        // world units per unit of window depth
    float uvPerUnit = 0.5 * length(gx);
    vec2 grad = -dot(gz, l) / max(dot(ng, l), 0.2) * vec2(dot(ng, gx) / dot(gx, gx), dot(ng, gy) / dot(gy, gy));
    float size = float(textureSize(shadowMap, 0).x);
    float maxUv = 0.05;
    float angle = 6.2831853 * SampleNoise(kDimSunShadow, gl_FragCoord.xy, float(frame.localLights.y));
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    int samples = int(frame.features.z);

    float minUv = 1.5 / size;
    float growth = max(ref * depthRange * tanHalf * uvPerUnit, minUv) / minUv;
    float reach = min(minUv * growth, maxUv);
    int levels = int(frame.features.w), base = textureSize(shadowMin, 0).x;
    if (reach > minUv && levels > 0) {
        // The level whose bilinear footprint, at least a texel (4 << level
        // shadow texels) around uv, covers every texel within reach.
        int level = min(findMSB(int(ceil(reach * size))), levels - 1);
        float span = float((4 << level) * max(base >> level, 1));  // shadow texels the level's texels span
        float nearest = textureLod(shadowMin, vec3(uv * size / span, float(c)), float(level)).r;
        float above = ref + length(grad) * reach - nearest;  // at most, for blockers within reach
        reach = min(reach, max(above * depthRange * tanHalf * uvPerUnit * 1.001, minUv));
    }
    // The receiver's own texel first: a blocker right above always counts.
    float above = ref - texelFetch(shadowMapDepth, ivec3(clamp(ivec2(uv * size), ivec2(0), ivec2(size - 1.0)), c), 0).r;
    float blockers = max(above, 0.0), count = above > 0.0 ? 1.0 : 0.0;
    for (int i = 0; i < samples; ++i) {
        float r = minUv * pow(growth, float(i) / float(samples)), a = angle + 2.3999632 * float(i);
        if (min(r, maxUv) > reach) {
            break;  // nor any further out
        }
        vec2 o = min(r, maxUv) * vec2(cos(a), sin(a));
        ivec2 texel = clamp(ivec2((uv + o) * size), ivec2(0), ivec2(size - 1.0));
        above = ref + dot(grad, o) - texelFetch(shadowMapDepth, ivec3(texel, c), 0).r;
        if (above > 0.0 && length(o) <= max(above * depthRange * tanHalf * uvPerUnit, minUv)) {
            blockers += above;
            count += 1.0;
        }
    }
    if (count == 0.0 || count == float(samples + 1)) {
        return count == 0.0 ? 1.0 : 0.0;  // no blocker, or the umbra
    }
    float filterUv = clamp(blockers / count * depthRange * tanHalf * uvPerUnit, 1.5 / size, maxUv);
    float sum = 0.0;
    for (int i = 0; i < samples; ++i) {
        vec2 o = rotation * kPoisson[i] * filterUv;
        sum += texture(shadowMap, vec4(uv + o, float(c), ref + dot(grad, o)));
    }
    return sum / float(samples);
}

// Cascaded shadow lookup with normal offset and 3x3 hardware PCF; soft
// (SoftSunShadow) for a sun with an angle when soft sun shadows are on.
float Shadow(vec3 p, vec3 ng, vec3 l, float tanHalf)
{
    float traced;
    if (RtShadow(frame.counts.y, traced)) {
        return traced;
    }
    float depth = dot(p - frame.cameraPos.xyz, frame.cameraForward.xyz);
    int c = 0;
    while (c < 4 && depth > frame.cascadeSplits[c]) {
        ++c;
    }
    if (c == 4) {
        return 1.0;
    }
    float texel = frame.cascadeTexel[c];
    float NoL = clamp(dot(ng, l), 0.0, 1.0);
    vec3 offset = ng * texel * 1.5 * (1.0 - NoL) + l * texel * 0.5;
    vec4 clip = frame.cascadeViewProj[c] * vec4(p + offset, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float ref = ndc.z * 0.5 + 0.5;  // same z remap as the vertex shader
    if (Feature(kFeatureSoftSun) && tanHalf > 0.0 && frame.shadowJitter.x == 0u) {
        return SoftSunShadow(c, uv, ref, ng, l, tanHalf);
    }
    vec2 ts = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(shadowMap, vec4(uv + vec2(x, y) * ts, float(c), ref));
        }
    }
    return sum / 9.0;
}

// Interreflections brighten occlusion on light surfaces (Jimenez 2016).
vec3 MultiBounce(float visibility, vec3 albedo)
{
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(visibility), ((visibility * a + b) * visibility + c) * visibility);
}

vec3 Unproject(vec3 ndc)
{
    vec4 p = frame.invViewProj * vec4(ndc, 1.0);
    return p.xyz / p.w;
}

float SceneDepth(vec2 ndc)
{
    ivec2 size = textureSize(sceneDepth, 0);
    return texelFetch(sceneDepth, clamp(ivec2((ndc * 0.5 + 0.5) * vec2(size)), ivec2(0), size - 1), 0).r;
}

// Whether a ray at window depth z is behind the depth buffer's `scene`
// (EEVEE 5.2's precision fix): by more than 7 steps of the depth's
// precision (2^-24 near 1, D32F as 24-bit), so a ray leaving its surface
// towards the camera doesn't hit it again where the depths round alike,
// nor where fast math puts an equal depth a hair behind.
bool Behind(float z, float scene)
{
    return scene < 1.0 && z - scene > 7.0 / 16777216.0;
}

// Denoised SSR (renderer/ssr.h, frame.ssr.z): the opaque surface's one
// GGX-sampled ray per pixel, for the denoiser.
layout(set = 0, binding = 72, rgba16f) uniform writeonly image2D ssrRays;        // radiance along it, its pdf
layout(set = 0, binding = 73, rgba16f) uniform writeonly image2D ssrDirections;  // octahedral, hit distance, GGX alpha
layout(set = 0, binding = 74, rgba16f) uniform writeonly image2D ssrNormals;     // octahedral
layout(set = 0, binding = 75, rgba16f) uniform writeonly image2D ssrFactor;      // the reflection's weight, 1
float ssrHitDistance = -1.0;  // TraceReflection's last hit, world units

// The visible surface's normal at `ndc` (world point p) from the depth:
// per axis the shorter of the two one-pixel steps, so silhouettes don't
// bend it; facing the camera.
vec3 DepthNormal(vec2 ndc, vec3 p)
{
    vec3 t[2];
    for (int i = 0; i < 2; ++i) {
        vec2 o = i == 0 ? vec2(2.0 / frame.viewport.x, 0.0) : vec2(0.0, 2.0 / frame.viewport.y);
        vec3 a = Unproject(vec3(ndc + o, SceneDepth(ndc + o) * 2.0 - 1.0)) - p;
        vec3 b = p - Unproject(vec3(ndc - o, SceneDepth(ndc - o) * 2.0 - 1.0));
        t[i] = dot(a, a) < dot(b, b) ? a : b;
    }
    vec3 n = normalize(cross(t[0], t[1]));
    return dot(n, frame.cameraPos.xyz - p) < 0.0 ? -n : n;
}

// Screen-space reflection: marches the ray linearly through NDC (where
// it stays a straight line with affine depth) against the prepass depth,
// refines the first crossing by bisection and shades it with last frame's
// result reprojected to the hit. Returns the hit's weight (0 = miss) and
// its premultiplied radiance in `hit`.
// Without the denoiser (Frame::ssrDenoise): mirror rays only, faded out
// from roughness 0.3 to 0.6. `cone`: the tangent of the half angle of the
// cone of directions the ray stands for (Frame::ssrHitFilter).
float TraceReflection(vec3 p, vec3 r, float cone, out vec4 hit)
{
    int steps = int(frame.features.y);
    hit = vec4(0.0);
    vec4 c0 = frame.viewProj * vec4(p, 1.0);
    vec4 dc = frame.viewProj * vec4(r, 0.0);
    // End far along the ray but in front of the near plane (z >= -w).
    float t = 1000.0 * max(length(p - frame.cameraPos.xyz), 1e-3);
    if (dc.z + dc.w < 0.0) {
        t = min(t, 0.99 * (c0.z + c0.w) / -(dc.z + dc.w));
    }
    vec4 c1 = c0 + dc * t;
    vec3 s0 = c0.xyz / c0.w, s1 = c1.xyz / c1.w;
    // Only the part of the segment that is on screen.
    vec2 d = s1.xy - s0.xy;
    float uEnd = 1.0;
    for (int i = 0; i < 2; ++i) {
        if (abs(d[i]) > 1e-8) {
            uEnd = min(uEnd, ((d[i] > 0.0 ? 1.0 : -1.0) - s0[i]) / d[i]);
        }
    }
    float pixels = length(d * uEnd * 0.5 * frame.viewport.xy);
    if (pixels < 2.0) {
        return 0.0;  // heads (almost) straight into or out of the screen
    }
    float uStart = 1.5 * uEnd / pixels;  // past the starting pixel
    float noise = SampleNoise(kDimSsr, gl_FragCoord.xy, frame.ssr.y);
    float uPrev = 0.0;
    vec3 sPrev = s0;
    bool wasBehind = false;
    // The thickness estimator (Frame::ssgiThickness): every step at least
    // a pixel on; jumps in depth between steps searched pixel by pixel.
    // ponytail: EEVEE also spreads the steps quadratically; here that cost
    // the crowd 1.4 ms for nothing the pixel search doesn't already catch.
    bool estimator = frame.screenTrace.x > 0.0;
    float prevScene = 1.0;
    // Pixels searched between steps, per ray: 4 per step (128 at the
    // default 32), so fewer steps cost less. A fixed 128 let the search
    // take over at few steps, whose gaps are wider: 8 steps were slower
    // than 32.
    int budget = 4 * steps;
    vec2 pixelNdc = 2.0 * normalize(d * frame.viewport.xy) / frame.viewport.xy;  // one pixel along the ray
    float lo = 0.0, hi = 0.0;  // the crossing's bracket
    bool crossed = false;
    for (int i = 0; i < steps && !crossed; ++i) {
        float u = mix(uStart, uEnd, (float(i) + noise) / float(steps));
        if (estimator) {
            u = max(u, uPrev + uEnd / pixels);
            if (u > uEnd) {
                break;
            }
        }
        vec3 s = mix(s0, s1, u);
        float z = s.z * 0.5 + 0.5;
        if (z >= 1.0) {
            break;  // beyond the far plane: only sky, which the dome provides
        }
        float scene = SceneDepth(s.xy);
        if (estimator && i > 0 && !wasBehind && !Behind(z, scene)) {
            // Across a jump in depth between two steps (an edge, the end
            // of a thin strip of surface seen edge-on) a thin feature can
            // hide in front of the ray: search the pixels in between.
            float a = 1.0 - prevScene, b = 1.0 - scene;  // ~ 1 / distance
            if ((prevScene >= 1.0) != (scene >= 1.0) || abs(a - b) > 0.1 * max(a, b)) {
                int between = min(int((u - uPrev) * pixels / uEnd), budget);
                budget -= between;
                for (int k = 1; k <= between; ++k) {
                    float uk = mix(uPrev, u, float(k) / float(between + 1));
                    vec3 sk = mix(s0, s1, uk);
                    float dk = SceneDepth(sk.xy);
                    if (Behind(sk.z * 0.5 + 0.5, dk)) {
                        u = uk;
                        s = sk;
                        z = sk.z * 0.5 + 0.5;
                        scene = dk;
                        break;
                    }
                    uPrev = uk;
                    sPrev = sk;
                }
            }
        }
        prevScene = scene;
        // Behind this pixel's surface by about a step: the ray went through
        // it. Further behind, it passes behind a separate object. Surfaces
        // seen at grazing angles put a crossing ray far behind along the
        // view ray, so objects are also assumed 5% of their distance thick.
        // A step that goes from in front of the depth to behind it is
        // refined first and judged where it crosses: at grazing angles a
        // whole step can land far behind a surface it did pass through
        // (otherwise a miss that TAA's clipping keeps as grain).
        bool behind = Behind(z, scene);
        lo = uPrev;
        hi = u;
        if (behind) {
            float stepLength = length(Unproject(s) - Unproject(sPrev));
            if (!wasBehind) {
                for (int k = 0; k < 5; ++k) {
                    float mid = 0.5 * (lo + hi);
                    vec3 m = mix(s0, s1, mid);
                    if (Behind(m.z * 0.5 + 0.5, SceneDepth(m.xy))) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
            }
            vec3 c = mix(s0, s1, hi);
            vec3 ray = Unproject(c);
            vec3 surface = Unproject(vec3(c.xy, SceneDepth(c.xy) * 2.0 - 1.0));
            float thickness = max(2.0 * stepLength, 0.05 * length(surface - frame.cameraPos.xyz));
            crossed = length(ray - surface) <= thickness;
            if (estimator && !crossed) {
                // At least the depth one pixel of this surface spans along
                // the ray (the nearer neighbour: not across an edge), and
                // a crossing of one continuous surface (no jump in depth
                // between the refined steps around it) is a hit at any
                // depth: grazing surfaces aren't porous.
                vec3 ahead = Unproject(vec3(c.xy + pixelNdc, SceneDepth(c.xy + pixelNdc) * 2.0 - 1.0));
                vec3 back = Unproject(vec3(c.xy - pixelNdc, SceneDepth(c.xy - pixelNdc) * 2.0 - 1.0));
                float footprint = min(length(ahead - surface), length(back - surface));
                crossed = length(ray - surface) <= footprint;
                if (!wasBehind && !crossed) {
                    vec3 l = mix(s0, s1, lo);
                    vec3 before = Unproject(vec3(l.xy, SceneDepth(l.xy) * 2.0 - 1.0));
                    crossed = length(before - surface) <= 2.0 * footprint + 1e-6 * length(surface - frame.cameraPos.xyz);
                }
            }
        }
        wasBehind = behind;
        uPrev = u;
        sPrev = s;
    }
    // Resolved after the march, so its code holds no registers in the
    // loop (it went 1 ms faster on the crowd than inside it).
    if (!crossed) {
        return 0.0;
    }
    for (int k = 0; k < 5; ++k) {
        float mid = 0.5 * (lo + hi);
        vec3 m = mix(s0, s1, mid);
        if (Behind(m.z * 0.5 + 0.5, SceneDepth(m.xy))) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    vec2 ndc = mix(s0.xy, s1.xy, hi);
    vec3 hitPos = Unproject(vec3(ndc, SceneDepth(ndc) * 2.0 - 1.0));
    ssrHitDistance = length(hitPos - p);
    vec4 prev = frame.prevViewProj * vec4(hitPos, 1.0);
    vec2 prevUv = prev.xy / prev.w * 0.5 + 0.5;
    if (prev.w <= 0.0 || any(lessThan(prevUv, vec2(0.0))) || any(greaterThan(prevUv, vec2(1.0)))) {
        return 0.0;  // not in last frame's image: a miss (the probe or dome)
    }
    // Footprint-filtered (Frame::ssrHitFilter, EEVEE 5.2): the
    // history's mip whose pixels are as wide as the ray's cone at
    // the hit, so a rough ray doesn't pick one sparkling pixel.
    float lod = 0.0, prevEdge = 1.0;
    if (frame.screenTrace.z > 0.0) {
        float hitNdcZ = SceneDepth(ndc) * 2.0 - 1.0;
        float pixel = length(Unproject(vec3(ndc.x + 2.0 / frame.viewport.x, ndc.y, hitNdcZ)) - hitPos);
        lod = clamp(log2(2.0 * ssrHitDistance * cone / max(pixel, 1e-20)), 0.0, 5.0);
        // Faded out at the edges of last frame's image too, where
        // the footprint reaches past it.
        vec2 e = 1.0 - abs(prevUv * 2.0 - 1.0);
        prevEdge = smoothstep(0.0, 0.1, min(e.x, e.y));
    }
    hit = textureLod(previousColor, prevUv, lod);
    if (frame.ssr.w > 0.0) {
        // Last frame's colour carries the light path intensities; its
        // splits take them out, so the reflection is scaled once (as
        // indirect glossy) like traced reflections.
        // ponytail: the splits at the hit, not over the mip's footprint
        // nor TAA's blend over frames.
        vec3 direct = textureLod(previousSplits, vec3(prevUv, 0.0), 0.0).rgb +
                      textureLod(previousSplits, vec3(prevUv, 2.0), 0.0).rgb;
        vec3 indirect = textureLod(previousSplits, vec3(prevUv, 1.0), 0.0).rgb +
                        textureLod(previousSplits, vec3(prevUv, 3.0), 0.0).rgb;
        float fogged = textureLod(previousSplits, vec3(prevUv, 0.0), 0.0).a;  // its fog's transmittance
        hit.rgb += fogged * ((1.0 - frame.lightPath.x) * direct + (1.0 - frame.lightPath.y) * indirect);
    }
    hit.rgb = BrightnessClamp(hit.rgb, frame.lightClamp.y);
    // Met from behind (the ray went behind an object): the light of
    // its unseen side is guessed as a share of the front's
    // (Frame::screenBackface).
    // A small tolerance: grazing hits (normal from depth across
    // the ray, rounding either way) count as front faces.
    if (frame.screenTrace.y < 1.0 && dot(DepthNormal(ndc, hitPos), r) > 1e-3) {
        hit.rgb *= frame.screenTrace.y;
    }
    // Fade towards the screen edges, beyond which the ray is blind.
    vec2 edge = 1.0 - abs(ndc);
    return smoothstep(0.0, 0.1, min(edge.x, edge.y)) * prevEdge;
}

// Same SH9 basis order as BuildEnvironment.
vec3 Irradiance(vec3 d)
{
    return frame.envSh[0].rgb * 0.282095
         + frame.envSh[1].rgb * 0.488603 * d.y
         + frame.envSh[2].rgb * 0.488603 * d.z
         + frame.envSh[3].rgb * 0.488603 * d.x
         + frame.envSh[4].rgb * 1.092548 * d.x * d.y
         + frame.envSh[5].rgb * 1.092548 * d.y * d.z
         + frame.envSh[6].rgb * 0.315392 * (3.0 * d.z * d.z - 1.0)
         + frame.envSh[7].rgb * 1.092548 * d.x * d.z
         + frame.envSh[8].rgb * 0.546274 * (d.x * d.x - d.y * d.y);
}

void main()
{
    vec3 baseColor = inColor;
    vec3 emissive = vec3(0.0);
    float metallic = 0.0, roughness = 0.5, occlusion = 1.0;
    vec3 tangentNormal = vec3(0.0, 0.0, 1.0);
    bool hasNormalMap = false;
    bool present = true;
    float alpha = 1.0;
    vec3 specularTint = vec3(1.0);
    if (inMaterial != kNoMaterial) {
        Material m = materials[inMaterial];
        present = Present(m, inUv);
        specularTint = m.specular.rgb;
        // Translucent (opacity without a cutout threshold): diffuse light and
        // emission scale with opacity, highlights don't (premultiplied, as
        // glass). Opaque materials have opacity 1.
        if (m.emissive.w <= 0.0) {
            alpha = clamp(Fetch(m.tex[6], m.params.wwww).r, 0.0, 1.0);
        }
        baseColor = Fetch(m.tex[0], m.baseColor.w > 0.5 ? vec4(inColor, 1.0) : m.baseColor).rgb;
        emissive = Fetch(m.tex[1], m.emissive).rgb;
        metallic = Fetch(m.tex[2], m.params.xxxx).r;
        roughness = Fetch(m.tex[3], m.params.yyyy).r;
        occlusion = clamp(Fetch(m.tex[4], m.params.zzzz).r, 0.0, 1.0);
        hasNormalMap = m.tex[5].index >= 0;
        tangentNormal = Fetch(m.tex[5], vec4(0.0, 0.0, 1.0, 0.0)).xyz;
#ifdef GRAPH
        if (m.program.y > 0u && (frame.features.x & kFeatureMaterialGraphs) != 0u) {
            RunProgram(m.program, baseColor, emissive, metallic, roughness, occlusion, tangentNormal, hasNormalMap,
                       alpha);
        }
#endif
    }
    TransmissionBegin(alpha);
    SubsurfaceBegin(baseColor, emissive, metallic, specularTint);

    vec3 v = normalize(frame.cameraPos.xyz - inWorldPos);
    // Geometric normal facing the viewer: renders everything double-sided and
    // makes winding/orientation and normal sign irrelevant.
    vec3 ng = normalize(cross(dFdx(inWorldPos), dFdy(inWorldPos)));
    if (dot(ng, v) < 0.0) {
        ng = -ng;
    }
    vec3 n = dot(inNormal, inNormal) > 1e-12 ? normalize(inNormal) : ng;
    // Bumped in the authored orientation (displacement moves along it, so
    // a back face's heights run the other way), then turned to the viewer.
    bool backface = dot(n, ng) < 0.0;
    n = DisplacedNormal(n, inSlot, inWorldPos, inUv);
    if (backface) {
        n = -n;
    }
    if (hasNormalMap && (frame.features.x & kFeatureNormalMaps) != 0u) {
        n = normalize(CotangentFrame(n, inWorldPos, inUv) * tangentNormal);
    }

    metallic = clamp(metallic, 0.0, 1.0);
    float perceptual = clamp(roughness, 0.045, 1.0);
    float a = perceptual * perceptual;
    vec3 diffuseColor = baseColor * (1.0 - metallic) * alpha;
    emissive *= alpha;
    vec3 f0 = mix(vec3(0.04), baseColor, metallic);  // ior 1.5
    float NoV = max(dot(n, v), 1e-4);

    // Split-sum DFG (A, B) and multiple-scattering compensation
    // (Fdez-Aguera 2019; Filament's energy compensation for direct light).
    vec2 ab = texture(brdfLut, vec2(NoV, perceptual)).rg;
    float Ess = ab.x + ab.y;
    // standard_surface's specular weight and colour scale the dielectric
    // lobe (and give its energy back to the diffuse one).
    vec3 tint = mix(specularTint, vec3(1.0), metallic);
    PrepareLayers(n, NoV, f0, diffuseColor, tint, emissive);
    SubsurfaceLayers();
    vec3 FssEss = (f0 * ab.x + ab.y) * tint;
    vec3 energyCompensation = (1.0 + f0 * (1.0 / max(Ess, 1e-4) - 1.0)) * tint;

    vec3 color = emissive;  // premultiplied by alpha
    for (uint i = 0; i < frame.counts.x; ++i) {
        Light light = lights[i];
        vec3 l = light.position.xyz;
        float NoL = dot(n, l);
        if (NoL <= 0.0) {
            continue;
        }
        // The sun's disc (angular radius axis.w) as a sphere at distance 1,
        // unless its half-vector area (~ tan^2 / (4 NoV)) is under 0.1% of
        // the lobe's a^2 (then N > 0.999 and l the centre: most surfaces).
        float tanHalf = tan(light.axis.w), sunNorm = 1.0, sunArea = 0.0;
        vec3 ls = l;
        regionStretch = vec2(1.0);
        if (Feature(kFeatureSunRegion) && tanHalf * tanHalf > 4e-3 * NoV * a * a) {
            sunNorm = SphereRegion(n, v, reflect(-v, n), a, l, 1.0, tanHalf, PI * tanHalf * tanHalf, ls, sunArea);
        }
        vec3 radiance = (diffuseColor / PI * NoL * light.halfHeight.w +
                         (Specular(n, v, ls, a, f0, NoV) * StretchRatio(n, v, ls, a) * energyCompensation * sunNorm +
                          LayerLight(n, v, ls, NoV, sunArea)) * light.shaping.w) *
                        light.color.rgb;
        if (i == frame.counts.y) {
            radiance *= Shadow(inWorldPos, ng, l, tanHalf) * ShadowTint(inWorldPos, ng, l);
        }
        color += radiance;
        if (LightSplits()) {
            vec3 d = diffuseColor / PI * NoL * light.halfHeight.w * light.color.rgb;
            splitDirectDiffuse += i == frame.counts.y
                ? d * Shadow(inWorldPos, ng, l, tan(light.axis.w)) * ShadowTint(inWorldPos, ng, l) : d;
        }
    }

    // Sphere and rect lights from this pixel's cluster (without clusters:
    // all of them).
    if (frame.localLights.x > 0u && (frame.features.x & kFeatureClusters) != 0u) {
        float viewDepth = dot(inWorldPos - frame.cameraPos.xyz, frame.cameraForward.xyz);
        uint base = ClusterIndex(gl_FragCoord.xy, viewDepth) * (kMaxClusterLights + 1u);
        uint count = clusterLights[base];
        for (uint k = 0u; k < count; ++k) {
            color += LocalLight(clusterLights[base + 1u + k], inWorldPos, n, ng, v, diffuseColor, f0,
                                a, NoV, energyCompensation);
        }
    } else {
        for (uint i = frame.counts.x; i < frame.counts.x + frame.localLights.x; ++i) {
            color += LocalLight(i, inWorldPos, n, ng, v, diffuseColor, f0, a, NoV, energyCompensation);
        }
    }

    if (frame.lightClamp.x > 0.0) {
        const vec3 lit = color - emissive, clamped = BrightnessClamp(lit, frame.lightClamp.x);
        splitDirectDiffuse *= dot(clamped, vec3(1.0)) / max(dot(lit, vec3(1.0)), 1e-6);  // the split follows
        color = emissive + clamped;
    }
    vec3 splitDirect = color - emissive;  // the lights' diffuse and glossy light
    const vec3 splitThrough = SubsurfaceTransmission(inWorldPos, n);  // direct diffuse
    color += splitThrough;

    // Image-based lighting from the dome, if any.
    vec3 specular = vec3(0.0), Lavg = vec3(0.0);
    if (frame.envRadiance.w > 0.0) {
        mat3 toLocal = mat3(frame.envWorldToLocal);
        vec3 r = normalize(toLocal * reflect(-v, BentNormal(n, v, perceptual)));
        float lod = perceptual * float(frame.counts.z - 1u);
        specular = textureLod(envCube, r, lod).rgb * frame.envRadiance.rgb;
        Lavg = max(Irradiance(normalize(toLocal * n)), 0.0) * frame.envRadiance.rgb / PI;
    }
    // Reflections beyond the screen (probes.glsl): the mirror reflects the
    // planar reflection (below, over the dome's sky), other surfaces the
    // reflection probe's captured surroundings instead of the dome.
    // Material and screen-space occlusion; specular occlusion derived from
    // it (Lagarde 2014), as in Filament.
    vec3 domeSpecular = specular, domeDiffuse = Lavg;  // the dome's alone (light splits)
    Lavg = GiDiffuse(inWorldPos, n, v, Lavg);
    float diffuseAO = min(occlusion, texelFetch(ambientOcclusion, ivec2(gl_FragCoord.xy), 0).r);
    int mirrorIndex = OnMirror(instances[inSlot].draw, inWorldPos);
    bool mirror = mirrorIndex >= 0;
    if (Feature(kFeatureProbe) && !mirror) {
        // The local ambient light probes are normalized by (GI or SSGI's).
        specular = ProbeSpecular(inWorldPos, reflect(-v, BentNormal(n, v, perceptual)), perceptual, specular, n,
                                 Lavg * diffuseAO + SsgiDiffuse());
    }
    float specularAO = clamp(pow(NoV + diffuseAO, exp2(-16.0 * a - 1.0)) - 1.0 + diffuseAO, 0.0, 1.0);
    specular *= MultiBounce(specularAO, f0);
    // The mirror's planar reflection, elsewhere screen-space reflections:
    // they replace the dome (or probe) where they hit something; where the
    // hit is partly background the dome shows through.
    if (mirror) {
        specular = PlanarSpecular(mirrorIndex, specular, inWorldPos, n, perceptual);
    }
    float ssrWeight = frame.ssr.x > 0.0 && !mirror && kShadingPass != 2u ? 1.0 - smoothstep(0.3, 0.6, perceptual) : 0.0;
#ifdef LAYERS
    ssrWeight *= 1.0 - layers.anisotropy;  // mirror rays can't follow a stretched highlight (brdf.glsl)
#endif
    if (frame.ssr.z > 0.0 && ssrWeight > 0.0 && RtOpaque()) {
        // Denoised: one GGX-sampled ray (glossy up to roughness 0.8), its
        // radiance (the probe's or dome's where it misses) and pdf for the
        // denoiser, which adds FssEss times the weight times its estimate.
        ssrWeight = 1.0 - smoothstep(0.5, 0.8, perceptual);
#ifdef LAYERS
        ssrWeight *= 1.0 - layers.anisotropy;
#endif
        vec3 tx = normalize(cross(abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), n)), ty = cross(n, tx);
        vec3 ve = vec3(dot(v, tx), dot(v, ty), NoV);
        // Two sample dimensions; without them interleaved gradient noise,
        // two lookups shifted per frame.
        vec2 u;
        if (frame.sampleInfo.x != 0u) {
            u = vec2(SampleNoise(kDimSsrGgx, gl_FragCoord.xy, 0.0), SampleNoise(kDimSsrGgx2, gl_FragCoord.xy, 0.0));
        } else {
            vec2 q = gl_FragCoord.xy + 5.588238 * frame.ssr.y;
            u = fract(52.9829189 * fract(vec2(dot(q, vec2(0.06711056, 0.00583715)),
                                              dot(q.yx + vec2(19.0, 47.0), vec2(0.06711056, 0.00583715)))));
        }
        vec3 h = SampleGgxVndf(ve, a, u);
        h = tx * h.x + ty * h.y + n * h.z;
        vec3 l = reflect(-v, h);
        if (dot(l, ng) <= 0.0) {
            h = n;  // below the surface: the mirror direction
            l = reflect(-v, n);
        }
        // The visible normals' pdf: D(h) G1(v) / (4 NoV).
        float pdf = D_GGX(max(dot(n, h), 0.0), a) * 2.0 / (NoV + sqrt(a * a + (1.0 - a * a) * NoV * NoV)) / 4.0;
        vec4 hit;
        // The ray's share of the lobe: the solid angle of one of the 9
        // rays the denoiser's spatial pass reuses.
        float w = TraceReflection(inWorldPos, l, sqrt(1.0 / (9.0 * PI * max(pdf, 1e-6))), hit);
        ivec2 px = ivec2(gl_FragCoord.xy);
        imageStore(ssrRays, px, vec4(mix(specular, hit.rgb + (1.0 - hit.a) * specular, w), pdf));
        imageStore(ssrDirections, px, vec4(OctEncode(l), w > 0.0 ? ssrHitDistance : -1.0, a));
        imageStore(ssrNormals, px, vec4(OctEncode(n), 0.0, 0.0));
        imageStore(ssrFactor, px, vec4(FssEss * ssrWeight, 1.0));
        specular *= 1.0 - ssrWeight;
        domeKept *= 1.0 - ssrWeight;  // ponytail: the denoised rays' misses aren't in the splits
    } else if (ssrWeight > 0.0) {
        vec4 hit;
        ssrWeight *= TraceReflection(inWorldPos, reflect(-v, n), a, hit);  // the lobe's width
        specular = mix(specular, hit.rgb + (1.0 - hit.a) * specular, ssrWeight);
        domeKept *= 1.0 - ssrWeight * hit.a;
    }
    if (LightSplits() && Feature(kFeatureRtReflections) && RtOpaque()) {
        domeKept *= 1.0 - imageLoad(rtReflectionImage, ivec2(gl_FragCoord.xy)).a;  // as RtReflection
    }
    specular = RtReflection(specular, inWorldPos, n, perceptual);
    float Ems = 1.0 - Ess;
    vec3 Favg = f0 + (1.0 - f0) / 21.0;
    vec3 Fms = FssEss * Favg / (1.0 - Ems * Favg);
    vec3 kd = diffuseColor * (1.0 - FssEss - Fms * Ems);
    // With SSGI the bounces are traced: the visibility hides the rest alone.
    vec3 splitAmbient = color;
    color += FssEss * specular + (Fms * Ems + kd) * (Lavg * MultiBounce(diffuseAO, Feature(kFeatureSsgi) ? vec3(0.0)
                                                                                                     : diffuseColor) +
                                                    SsgiDiffuse() * occlusion);
    vec3 splitIndirectGlossy = FssEss * specular;
    vec3 splitIndirectDiffuse = color - splitAmbient - splitIndirectGlossy;
    // Karma's convention (frame.aovs.y): the dome's light that reaches the
    // surface unblocked is direct. Diffuse: its irradiance under the
    // visibility (AO, or SSGI's); what GI's probes, the multi-bounce
    // approximation and SSGI add beyond it is indirect. Glossy: its
    // reflection under the specular occlusion where no probe, mirror, SSR
    // or traced reflection replaced it. At most the whole (where the probes
    // see less of the dome than the AO does), so the splits still add up.
    vec3 splitDomeGlossy = vec3(0.0);
    if (LightSplits() && frame.aovs.y != 0u) {
        vec3 domeDirect = min((Fms * Ems + kd) * domeDiffuse * diffuseAO, splitIndirectDiffuse);
        splitDirectDiffuse += domeDirect;
        splitDirect += domeDirect;  // layer 2 is splitDirect - splitDirectDiffuse
        splitIndirectDiffuse -= domeDirect;
        splitDomeGlossy = min(FssEss * domeSpecular * domeKept * specularAO, splitIndirectGlossy);
        splitIndirectGlossy -= splitDomeGlossy;
    }
    if (LightPathScaled()) {
        color += (frame.lightPath.x - 1.0) * (splitDirect + splitDomeGlossy + splitThrough) +
                 (frame.lightPath.y - 1.0) * (splitIndirectDiffuse + splitIndirectGlossy);
    }
    if (kShadingPass != 2u) {  // the subsurface pass: the diffuse light alone
        splitAmbient = color;
        color += LayerAmbient(n, v, Lavg, diffuseAO, specularAO);
        // ponytail: the lobes' dome light (coat, sheen, hair) all one way;
        // a sheen under GI's probes counts their light as the dome's too.
        if (frame.aovs.y != 0u) {
            splitDomeGlossy += color - splitAmbient;
        } else {
            splitIndirectGlossy += color - splitAmbient;
        }
        if (LightPathScaled()) {  // the lobes' dome light, one way as above
            color += ((frame.aovs.y != 0u ? frame.lightPath.x : frame.lightPath.y) - 1.0) * (color - splitAmbient);
        }
        color = ToonShade(color, emissive, diffuseColor + f0);
    }
    alpha = TransmissionEnd(color, n, v, perceptual);
    if (LightPathScaled()) {
        // What it refracts is indirect glossy (screen space: the copied
        // colour behind has the intensities taken out, background_unscale.comp).
        color += (frame.lightPath.y - 1.0) * gRefractedLight;
    }
    // Cutouts: last, so every derivative above saw the whole quad; and
    // the other level's pixels of a level of detail crossfade.
    if (!present || ((kCompiledFeatures & kFeatureLod) != 0u && !LodKeep(inEntry, gl_FragCoord.xy))) {
        discard;
    }
    // Volumetric fog between the camera and the surface (premultiplied;
    // also for OIT, not on the subsurface pass's diffuse light).
    if (kShadingPass != 2u) {
        vec4 fog = Fog(gl_FragCoord.xy, dot(inWorldPos - frame.cameraPos.xyz, frame.cameraForward.xyz));
        color = color * fog.a + fog.rgb * alpha;
        fogTransmittance = fog.a;
    }
    // NaN/Inf here (screen-space or probe input gone bad) would enter the
    // TAA, SSGI and exposure histories and spread from there; and above
    // half-float range it turns Inf in the RGBA16F target.
    color = any(isnan(color)) || any(isinf(color)) ? vec3(0.0) : min(color, vec3(60000.0));
    outColor = vec4(color, alpha);
    // ponytail: opaque surfaces only (the translucent pass would overwrite
    // what is behind); a pixel's last surface at equal depth wins. Opaque
    // includes refracting glass, its back faces too (pass 3): refraction's
    // later copies take the intensities out of them by these splits, and
    // the front faces drawn after replace them.
    if (LightSplits() && frame.aovs.x != 0u && (kShadingPass == 0u || kShadingPass == 3u) && alpha >= 1.0) {
        ivec2 pixel = ivec2(gl_FragCoord.xy);
        // a: the fog's transmittance in front of it (the splits are
        // unfogged; what is copied or reflected of the colour is fogged).
        imageStore(lightSplits, ivec3(pixel, 0), vec4(splitDirectDiffuse + splitThrough, fogTransmittance));
        imageStore(lightSplits, ivec3(pixel, 1), vec4(splitIndirectDiffuse, 1.0));
        imageStore(lightSplits, ivec3(pixel, 2), vec4(splitDirect - splitDirectDiffuse + splitDomeGlossy, 1.0));
        imageStore(lightSplits, ivec3(pixel, 3), vec4(splitIndirectGlossy + gRefractedLight, 1.0));
    }
    outPrimId = inPrimId;
    outInstanceId = inInstanceId;
    vec4 now = frame.prevViewProj * vec4(inWorldPos, 1.0);
    vec4 before = frame.prevViewProj * vec4(inPrevWorldPos, 1.0);
    outVelocity = before.w > 0.0 && now.w > 0.0 ? (before.xy / before.w - now.xy / now.w) * 0.5 : vec2(0.0);
    PassOutput(color, alpha);
}
