#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "tiles.glsl"
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
#include "caustics.glsl"  // caustic photons

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
#define inLinkMask (instances[inSlot].linkMask)  // light linking (lights.glsl's LightLinked)
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
// The splits' layers past the 4 (Aovs::EnsureSplits): what the shadows
// took (all of it, the diffuse), the coat's light, each light group's
// direct light (Light::color.w / 4 - 1 its group).
vec3 splitShadow = vec3(0.0), splitShadowDiffuse = vec3(0.0), splitCoat = vec3(0.0);
const uint kMaxLightGroups = 8u;  // Aovs::kMaxLightGroups
const int kCausticsLayer = 10;
const int kGroupLayer = 11;       // Aovs::kSplitLayers
vec3 splitGroups[kMaxLightGroups] = vec3[](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0),
                                           vec3(0.0), vec3(0.0));
int LightGroup(Light light)
{
    return int(uint(light.color.w) >> 2u) - 1;
}
void SplitGroup(int group, vec3 light)
{
    if (group >= 0 && group < int(kMaxLightGroups)) {
        splitGroups[group] += light;
    }
}
// A light's share of them: `lit` its direct light, `unlit` the same
// without its shadow, the diffuse of both, the coat's (lit).
void SplitLight(Light light, vec3 lit, vec3 unlit, vec3 diffuseLit, vec3 diffuseUnlit, vec3 coat)
{
    splitShadow += unlit - lit;
    splitShadowDiffuse += diffuseUnlit - diffuseLit;
    splitCoat += coat;
    SplitGroup(LightGroup(light), lit);
}
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

// The base's GGX specular (brdf.glsl's BaseFresnel: its metal and
// dielectric lobes, their weights and multiple scattering) times cos for
// light from direction l; f0 for hair.
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
    return D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, max(NoL, 1e-4), a) * BaseFresnel(LoH) * NoL;
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
// Taps where the receiver's plane comes nearer the light compare against
// the plane there (window depth is affine in atlas uv on a plane, so the
// gradient is exact): a tilted receiver near a big light (a wall beside an
// emissive panel) would otherwise find itself as a blocker. Where it
// recedes they keep p's depth: what is in front of the plane there but
// farther from the light than p (the floor in front of a wall) blocks
// nothing. The normal offset covers the texels' quantization.
// ponytail: blockers are searched assuming none closer than a quarter of
// the way to the receiver, and kernels are clamped to 24 texels and to the
// tile (cube face seams can show under very soft shadows).
float LocalShadow(uint index, Light light, vec3 p, vec3 ng)
{
    float traced;
    if (RtShadow(index, p, ng, traced)) {
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
    vec3 q = p + ng * texel * 1.5 * (1.0 - NoL) + l * texel * 0.5;
    vec4 clip = t.viewProj * vec4(q, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = t.rect.xy + (ndc.xy * 0.5 + 0.5) * t.rect.zw;
    float ref = min(ndc.z * 0.5 + 0.5, kShadowFarRef);  // same z remap as the vertex shader
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

    // The receiver plane's window depth per atlas uv, from two steps along
    // it (8 texels: well above float precision, well short of the light).
    vec3 b1 = normalize(cross(ng, abs(ng.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
    vec4 c1 = t.viewProj * vec4(q + b1 * texel * 8.0, 1.0), c2 = t.viewProj * vec4(q + cross(ng, b1) * texel * 8.0, 1.0);
    vec3 d1 = c1.xyz / c1.w - ndc, d2 = c2.xyz / c2.w - ndc;
    mat2 steps = mat2(d1.xy * 0.5 * t.rect.zw, d2.xy * 0.5 * t.rect.zw);  // uv per step
    float det = determinant(steps);
    vec2 grad = c1.w > 0.0 && c2.w > 0.0 && abs(det) > 1e-24 ? inverse(transpose(steps)) * (0.5 * vec2(d1.z, d2.z)) : vec2(0.0);

    float searchUv = min(size * 3.0 / receiver * uvPerUnit, maxUv);
    int samples = int(frame.features.z);
    float spread = 0.0, count = 0.0;  // sum of 1 / blocker - 1 / receiver
    for (int i = 0; i < samples; ++i) {
        vec2 s = clamp(uv + rotation * kPoisson[i] * searchUv, lo, hi);
        float d = texelFetch(shadowAtlasDepth, ivec2(s * atlasSize), 0).r;
        if (d < ref + min(dot(grad, s - uv), 0.0)) {
            spread += 1.0 / ShadowDistance(d, t.params.y, t.params.z) - 1.0 / receiver;
            count += 1.0;
        }
    }
    if (count == 0.0) {
        return 1.0;
    }
    float filterUv = clamp(size * spread / count * uvPerUnit, 1.0 / atlasSize, maxUv);
    float sum = 0.0;
    for (int i = 0; i < samples; ++i) {
        vec2 s = clamp(uv + rotation * kPoisson[i] * filterUv, lo, hi);
        sum += texture(shadowAtlas, vec3(s, ref + min(dot(grad, s - uv), 0.0)));
    }
    return sum / float(samples);
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
// ponytail: small-angle half-vector mapping around the representative
// point; LTC (Heitz 2016) would be exact for polygons of any size.
float LightRegion(vec3 n, vec3 v, float a, vec3 toLight, float d, vec3 a1, vec3 a2, inout vec2 uv, bool box,
                  float omega, out vec3 l)
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
float SphereRegion(vec3 n, vec3 v, vec3 r, float a, vec3 toLight, float d, float R, float omega, out vec3 l)
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
// horizon-clipped sphere/disk illuminance; for rects the exact form factor
// of the polygon clipped to the horizon; for cylinders' rect silhouettes
// the form factor vector with Heitz's horizon approximation. Specular:
// LightRegion's representative point. scale: ClusterLightScale's.
vec3 LocalLight(uint index, vec3 p, vec3 n, vec3 ng, vec3 v, vec3 diffuseColor, vec3 f0, float a,
                float NoV, float scale)
{
    Light light = lights[index];
    vec3 toLight = light.position.xyz - p;
    float d = length(toLight);
    float window = LightWindow(light, index, d);
    if (window <= 0.0 || d <= 0.0 || !LightLinked(light, inLinkMask)) {
        return vec3(0.0);
    }
    vec3 radiance = light.color.rgb * (window * EmitterClamp(light, diffuseColor) * scale) * LightIes(light, p);
    float cosOff = dot(-toLight / d, light.axis.xyz);
    if (light.shaping.z > 0.0) {
        radiance *= mix(light.focusTint.rgb, vec3(1.0), pow(abs(cosOff), light.shaping.z));
    }
    float theta = acos(clamp(cosOff, -1.0, 1.0));
    radiance *= light.shaping.y <= light.shaping.x ? (theta <= light.shaping.y ? 1.0 : 0.0)
                                                   : 1.0 - smoothstep(light.shaping.x, light.shaping.y, theta);

    vec3 r = reflect(-v, n);
    float diffuseE, specularE, norm;
    vec3 l;
    vec3 diffuseTint = vec3(1.0), specularTint = vec3(1.0);  // a rect or disk light's texture
    // The region as LightRegion got it (half axes, where the reflection ray
    // passes, box), for the coat's own representative point.
    vec3 regionA1 = vec3(0.0), regionA2 = vec3(0.0);
    vec2 regionUv = vec2(0.0);
    bool regionBox = false;
    if (uint(light.position.w) == kLightSphere) {
        float sinSigmaSqr = min(light.axis.w * light.axis.w / (d * d), 1.0);
        diffuseE = SphereIlluminance(dot(n, toLight / d), sinSigmaSqr);
        specularE = PI * sinSigmaSqr;
        norm = SphereRegion(n, v, r, a, toLight, d, light.halfWidth.w, specularE, l);
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
        regionA1 = axis;
        regionA2 = side;
        regionUv = uv;
        regionBox = true;
        norm = LightRegion(n, v, a, toLight, d, axis, side, uv, true, specularE, l);
    } else {
        if (dot(toLight, light.axis.xyz) >= 0.0) {
            return vec3(0.0);  // behind the emitting side
        }
        vec3 hw = light.halfWidth.xyz, hh = light.halfHeight.xyz;
        bool rect = uint(light.position.w) == kLightRect;
        if (rect) {
            vec3 formFactor = RectFormFactor(toLight, hw, hh);
            diffuseE = PI * RectFormFactorClipped(toLight, hw, hh, n);
            specularE = PI * length(formFactor);
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
        regionA1 = hw;
        regionA2 = hh;
        regionUv = uv;
        regionBox = rect;
        norm = LightRegion(n, v, a, toLight, d, hw, hh, uv, rect, specularE, l);
        int tex = int(light.focusTint.w);
        if (tex >= 0 && (frame.reflections.z & kLightTextures) != 0u) {
            // The texture (s = 0 at +X as UsdLux says; the image's top row
            // at +Y, as Karma, i.e. t = 1: rows are stored bottom-up) where
            // the specular representative point is, at the mip of the
            // lobe's footprint at the light (its radius in light
            // directions, 2 a, stretched as LightRegion stretched it); for
            // diffuse from 4 x 4 cells of the light (LightTextureDiffuse).
            // ponytail: two taps; no prefiltered form factor (Heitz's
            // filtered polygon lights would get the wide lobes right).
            float extent = length(hw) + length(hh);
            float texels = float(max(textureSize(textures[nonuniformEXT(tex)], 0).x,
                                     textureSize(textures[nonuniformEXT(tex)], 0).y));
            float aLobe = min(2.0 * a * max(regionStretch.x, regionStretch.y), 1.0);
            float lobeLod = log2(texels * clamp(aLobe * d / extent, 1.0 / texels, 1.0));
            specularTint = textureLod(textures[nonuniformEXT(tex)], vec2(0.5, 0.5) + uv * vec2(-0.5, 0.5), lobeLod).rgb;
            diffuseTint = LightTextureDiffuse(light, tex, toLight, n, 4);
        }
    }
    // The diffuse lobe (Oren-Nayar) towards the light's centre.
    vec3 lc = toLight / d;
    vec3 diffuse = DiffuseLobe(diffuseColor, max(dot(n, lc), 0.0), NoV, dot(lc, v)) / PI * diffuseE *
                   light.halfHeight.w * diffuseTint;
    vec3 specular = Specular(n, v, l, a, f0, NoV) * StretchRatio(n, v, l, a) * specularE * norm *
                    light.shaping.w;
    // The coat's own representative point at its roughness: a smooth coat
    // over a rough base reflects the light's sharp shape.
    // ponytail: the coat takes the base's texture tap.
    vec3 lCoat = l;
    float coatNorm = 1.0, ca = CoatAlpha();
    if (ca > 0.0) {
        coatNorm = uint(light.position.w) == kLightSphere
            ? SphereRegion(n, v, r, ca, toLight, d, light.halfWidth.w, specularE, lCoat)
            : LightRegion(n, v, ca, toLight, d, regionA1, regionA2, regionUv, regionBox, specularE, lCoat);
        coatNorm *= StretchRatio(n, v, lCoat, ca);
    }
    specular += LayerLight(n, v, l, NoV, lCoat, coatNorm) * specularE * light.shaping.w;
    specular *= specularTint;
    vec3 shadow = LocalShadow(index, light, p, ng) * LocalShadowTint(index, light, p, ng);
    if (LightSplits()) {
        splitDirectDiffuse += radiance * diffuse * shadow;
        SplitLight(light, radiance * (diffuse + specular) * shadow, radiance * (diffuse + specular),
                   radiance * diffuse * shadow, radiance * diffuse,
                   radiance * gCoatLight * specularE * light.shaping.w * specularTint * shadow);
    }
    return radiance * (diffuse + specular) * shadow;
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
    if (RtShadow(frame.counts.y, p, ng, traced)) {
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

// A further sun's shadow (distant light `index` past the first): its atlas
// tile, one orthographic map over the view (params.x: world units per
// texel), with Shadow()'s normal offset and 3x3 PCF, times the translucent
// casters' and volumes' tint. Unshadowed without a tile (kMaxSunTiles).
// ponytail: hard (no PCSS); one map, so far from the camera coarser than
// the main sun's cascades.
vec3 SunTileShadow(uint index, vec3 p, vec3 ng, vec3 l)
{
    uint tile = lightShadow[index];
    if (tile == kNone) {
        return vec3(1.0);
    }
    ShadowTile t = shadowTiles[tile];
    float texel = t.params.x;
    float NoL = clamp(dot(ng, l), 0.0, 1.0);
    vec4 clip = t.viewProj * vec4(p + ng * texel * 1.5 * (1.0 - NoL) + l * texel * 0.5, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    if (any(greaterThan(abs(ndc.xy), vec2(1.0)))) {
        return vec3(1.0);
    }
    vec2 uv = t.rect.xy + (ndc.xy * 0.5 + 0.5) * t.rect.zw;
    float ref = ndc.z * 0.5 + 0.5;  // same z remap as the vertex shader
    float atlasSize = float(textureSize(shadowAtlas, 0).x);
    vec2 lo = t.rect.xy + 0.5 / atlasSize, hi = t.rect.xy + t.rect.zw - 0.5 / atlasSize;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(shadowAtlas, vec3(clamp(uv + vec2(x, y) / atlasSize, lo, hi), ref));
        }
    }
    vec3 tint = Feature(kFeatureTranslucentShadows)
                    ? TintLookup(sunTint, sunTintDepth, atlasTint, atlasTintDepth, false, uv, 0.0, ref, t.rect.xy,
                                 t.rect.xy + t.rect.zw)
                    : vec3(1.0);
    return sum / 9.0 * tint;
}

// Interreflections brighten occlusion on light surfaces (Jimenez 2016).
vec3 MultiBounce(float visibility, vec3 albedo)
{
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(visibility), ((visibility * a + b) * visibility + c) * visibility);
}

// Denoised SSR (renderer/ssr.h, frame.ssr.z): the opaque surface's one
// GGX-sampled ray per pixel, for the denoiser. At a lower resolution
// (frame.ssr.z 2, 3) ssr_trace.frag traces after this pass: ssrRays then
// holds what the pixel reflects without a hit and its ray's GGX alpha,
// ssrNormals the geometric normal too, and ssrDirections is the trace's.
// Interleaved (4) it traces the rougher surfaces' rays, a quarter of them
// a frame, into these images: their pixels store a pdf of 0 (no ray yet)
// and the alpha with the directions.
layout(set = 0, binding = 72, rgba16f) uniform writeonly image2D ssrRays;        // radiance along it, its pdf
layout(set = 0, binding = 73, rgba16f) uniform writeonly image2D ssrDirections;  // octahedral, hit distance, GGX alpha
layout(set = 0, binding = 74, rgba16f) uniform writeonly image2D ssrNormals;     // octahedral
layout(set = 0, binding = 75, rgba16f) uniform writeonly image2D ssrFactor;      // the reflection's weight, 1 + the share that shows
#include "ssr_trace.glsl"
// Interleaved denoised SSR (frame.ssr.z 4): from this perceptual roughness
// up a surface's rays are traced for a quarter of its pixels a frame (the
// denoiser's spatial kernel is then at least 2.5 pixels wide).
const float kSsrInterleaved = 0.2;

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

#ifdef LAYERS
// The coat's reflection of its surroundings at its own roughness (not the
// base's): the dome, the probes over it, the mirror's planar reflection or
// else one screen-space ray, then the traced reflection (rt_pixels.rgen
// traces the coat's lobe). `local`: the probes' normalization (ProbeSpecular).
vec3 CoatReflection(vec3 n, vec3 v, vec3 local, int mirrorIndex)
{
    float cp = layers.coatPerceptual, kept = domeKept;  // the base's splits keep their domeKept
    vec3 r = reflect(-v, n), c = vec3(0.0);
    if (frame.envRadiance.w > 0.0) {
        c = textureLod(envCube, normalize(mat3(frame.envWorldToLocal) * r), cp * float(frame.counts.z - 1u)).rgb *
            frame.envRadiance.rgb;
    }
    if (mirrorIndex >= 0) {
        c = PlanarSpecular(mirrorIndex, c, inWorldPos, n, cp);
    } else {
        if (Feature(kFeatureProbe)) {
            c = ProbeSpecular(inWorldPos, r, cp, c, n, local);
        }
        if (frame.ssr.x > 0.0 && kShadingPass != 2u && !(Feature(kFeatureRtReflections) && RtOpaque())) {
            vec4 hit;
            float w = TraceReflection(inWorldPos, r, 2.0 * cp * cp, hit) * (1.0 - smoothstep(0.3, 0.6, cp));
            c = mix(c, hit.rgb + (1.0 - hit.a) * c, w);
        }
    }
    domeKept = kept;
    return RtReflection(c, inWorldPos, n, cp);
}
#endif

void main()
{
    // Tile-based adaptive sampling: a stopped tile keeps its ids and motion
    // but isn't shaded (whole 2 x 2 quads: tiles are 16 pixels).
    if (kShadingPass == 0u && TileSkipped(ivec2(gl_FragCoord.xy))) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        outPrimId = inPrimId;
        outInstanceId = inInstanceId;
        vec4 now = frame.prevViewProj * vec4(inWorldPos, 1.0);
        vec4 before = frame.prevViewProj * vec4(inPrevWorldPos, 1.0);
        outVelocity = before.w > 0.0 && now.w > 0.0 ? (before.xy / before.w - now.xy / now.w) * 0.5 : vec2(0.0);
        return;
    }
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
        // Translucent (opacity without a cutout threshold): diffuse light and
        // emission scale with opacity, highlights don't (premultiplied, as
        // glass). Opaque materials have opacity 1.
        if (m.emissive.w <= 0.0) {
            alpha = clamp(Fetch(m.tex[6], m.params.wwww).r, 0.0, 1.0);
        }
        baseColor = Fetch(m.tex[0], m.baseColor.w > 0.5 ? vec4(inColor * m.baseColor.rgb, 1.0) : m.baseColor).rgb;
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

    vec3 v = ViewDirection(inWorldPos);
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
    CoatRoughening(roughness);
    // ponytail: roughness under 0.045 (alpha 0.002) shades as 0.045, so
    // point-like lights keep a finite highlight.
    float perceptual = clamp(roughness, 0.045, 1.0);
    float a = perceptual * perceptual;
    emissive *= alpha;
    float NoV = max(dot(n, v), 1e-4);

    // The base by the material's surface model (brdf.glsl's SetupBase),
    // with the split-sum DFG (A, B, and C for F82) at its roughness; the
    // lobes on top of it. FssEss: the base's specular albedo towards the
    // viewer (multiple scattering included where the model has it).
    vec3 dfg = texture(brdfLut, vec2(NoV, perceptual)).rgb;
    vec3 diffuseColor = SetupBase(baseColor, metallic, specularTint, NoV, dfg) * alpha;
    PrepareLayers(n, NoV, diffuseColor, emissive);
    SubsurfaceLayers();
    vec3 f0 = base.metalW * base.metalF0 + base.dielW * base.dielF0;  // hair, multi-bounce, toon
    vec3 FssEss = BaseAlbedo(NoV, dfg);

    emissive = BrightnessClamp(emissive, frame.lightClamp.z);  // Karma's colour limit (40 shows as 20)
    vec3 color = emissive;  // premultiplied by alpha
    for (uint i = 0; i < frame.counts.x; ++i) {
        Light light = lights[i];
        vec3 l = light.position.xyz;
        float NoL = dot(n, l);
        if (NoL <= 0.0 || !LightLinked(light, inLinkMask)) {
            continue;
        }
        // The sun's disc (angular radius axis.w) as a sphere at distance 1,
        // unless its half-vector area (~ tan^2 / (4 NoV)) is under 0.1% of
        // the lobe's a^2 (then N > 0.999 and l the centre: most surfaces).
        float tanHalf = tan(light.axis.w), sunNorm = 1.0;
        vec3 ls = l;
        regionStretch = vec2(1.0);
        if (Feature(kFeatureSunRegion) && tanHalf * tanHalf > 4e-3 * NoV * a * a) {
            sunNorm = SphereRegion(n, v, reflect(-v, n), a, l, 1.0, tanHalf, PI * tanHalf * tanHalf, ls);
        }
        vec3 sunDiffuse = DiffuseLobe(diffuseColor, NoL, NoV, dot(l, v));
        vec3 baseSpecular = Specular(n, v, ls, a, f0, NoV) * StretchRatio(n, v, ls, a) * sunNorm;
        // The coat's own point on the disc, at its roughness.
        vec3 lc = l;
        float coatNorm = 1.0, ca = CoatAlpha();
        regionStretch = vec2(1.0);
        if (ca > 0.0 && Feature(kFeatureSunRegion) && tanHalf * tanHalf > 4e-3 * NoV * ca * ca) {
            coatNorm = SphereRegion(n, v, reflect(-v, n), ca, l, 1.0, tanHalf, PI * tanHalf * tanHalf, lc);
            coatNorm *= StretchRatio(n, v, lc, ca);
        }
        vec3 radiance = (sunDiffuse / PI * NoL * light.halfHeight.w +
                         (baseSpecular + LayerLight(n, v, ls, NoV, lc, coatNorm)) * light.shaping.w) *
                        light.color.rgb;
        vec3 shadow = vec3(1.0);
        if (i == frame.counts.y) {
            shadow = Shadow(inWorldPos, ng, l, tanHalf) * ShadowTint(inWorldPos, ng, l);
        } else if (!RtTraced(i, inWorldPos, ng, shadow)) {
            shadow = SunTileShadow(i, inWorldPos, ng, l);  // further suns: traced, else their atlas tile
        }
        color += radiance * shadow;
        if (LightSplits()) {
            vec3 d = sunDiffuse / PI * NoL * light.halfHeight.w * light.color.rgb;
            splitDirectDiffuse += d * shadow;
            SplitLight(light, radiance * shadow, radiance, d * shadow, d,
                       gCoatLight * light.shaping.w * light.color.rgb * shadow);
        }
    }

    // Sphere and rect lights from this pixel's cluster (without clusters:
    // all of them).
    if (frame.localLights.x > 0u && (frame.features.x & kFeatureClusters) != 0u) {
        float viewDepth = dot(inWorldPos - frame.cameraPos.xyz, frame.cameraForward.xyz);
        uint base = ClusterIndex(gl_FragCoord.xy, viewDepth) * (kMaxClusterLights + 1u);
        uint count = clusterLights[base];
        for (uint k = 0u; k < count; ++k) {
            color += LocalLight(ClusterLight(base, k), inWorldPos, n, ng, v, diffuseColor, f0,
                                a, NoV, ClusterLightScale(base, k));
        }
    } else {
        for (uint i = frame.counts.x; i < frame.counts.x + frame.localLights.x; ++i) {
            color += LocalLight(i, inWorldPos, n, ng, v, diffuseColor, f0, a, NoV, 1.0);
        }
    }

    if (frame.lightClamp.x > 0.0) {
        const vec3 lit = color - emissive, clamped = BrightnessClamp(lit, frame.lightClamp.x);
        float kept = dot(clamped, vec3(1.0)) / max(dot(lit, vec3(1.0)), 1e-6);  // the splits follow
        splitDirectDiffuse *= kept;
        splitCoat *= kept;
        for (uint g = 0u; g < kMaxLightGroups; ++g) {
            splitGroups[g] *= kept;
        }
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
    // No dome: where reflections miss, the bounce light stands in instead
    // of black: the GI probes' around the mirror direction, else SSGI's
    // at the pixel (without either there is no ambient light to show).
    // Glossy surfaces only (SSR's range): on rough ones it brightened the
    // Cornell box past Karma (mald 0.025 to 0.052).
    if (frame.envRadiance.w <= 0.0 && perceptual < 0.6) {
        specular = GiDiffuse(inWorldPos, reflect(-v, BentNormal(n, v, perceptual)), v, SsgiDiffuse());
    }
    Lavg = GiDiffuse(inWorldPos, n, v, Lavg);
    float diffuseAO = min(occlusion, texelFetch(ambientOcclusion, ivec2(gl_FragCoord.xy), 0).r);
    // Final gather: traced, so occluded already; neither AO nor SSGI on it.
    vec3 gathered;
    const bool finalGather = RtGather(gathered);
    if (finalGather) {
        Lavg = gathered;
    }
    // Under glass the sky light takes its colour (the gather traces it).
    const vec3 indirectDiffuse = finalGather ? Lavg * occlusion
                                        : Lavg * TranslucentSkyTint(inWorldPos, n) *
                                                  MultiBounce(diffuseAO, Feature(kFeatureSsgi) ? vec3(0.0) : diffuseColor) +
                                              SsgiDiffuse() * occlusion;
    int mirrorIndex = OnMirror(instances[inSlot].draw, inWorldPos);
    bool mirror = mirrorIndex >= 0;
    if (Feature(kFeatureProbe) && !mirror) {
        // The local ambient light probes are normalized by (GI or SSGI's).
        specular = ProbeSpecular(inWorldPos, reflect(-v, BentNormal(n, v, perceptual)), perceptual, specular, n,
                                 finalGather ? Lavg * occlusion : Lavg * diffuseAO + SsgiDiffuse());
    }
    // Never below the diffuse occlusion: on smooth surfaces at grazing
    // angles the formula goes black where the hidden reflection (an
    // object's underside) is only darker, blotting object bases.
    float specularAO = max(clamp(pow(NoV + diffuseAO, exp2(-16.0 * a - 1.0)) - 1.0 + diffuseAO, 0.0, 1.0), diffuseAO);
    specular *= MultiBounce(specularAO, f0);
    // Traced diffuse light (GI's probes or final gather) also tells how much
    // of the dome the surroundings let through: the dome's reflection, which
    // rough surfaces keep (traced reflections fade out over them), is
    // dimmed to match, so a room's rough stone doesn't mirror the open sky.
    // (A reflection probe's capture and a mirror's reflection are occluded already.)
    if (Feature(kFeatureGi) && frame.envRadiance.w > 0.0 && !mirror && !Feature(kFeatureProbe)) {
        const vec3 kLuminance = vec3(0.2126, 0.7152, 0.0722);
        float open = dot(domeDiffuse, kLuminance);
        specular *= open > 1e-6 ? min(dot(Lavg, kLuminance) / open, 1.0) : 1.0;
    }
    // The mirror's planar reflection, elsewhere screen-space reflections:
    // they replace the dome (or probe) where they hit something; where the
    // hit is partly background the dome shows through.
    if (mirror) {
        specular = PlanarSpecular(mirrorIndex, specular, inWorldPos, n, perceptual);
    }
    // No base specular (e.g. standard_surface specular 0): nothing to reflect,
    // so no ray (the crowd's agents: SSR was most of the shading pass).
    const bool reflects = max(max(FssEss.r, FssEss.g), FssEss.b) > 1e-4;
    // Holdouts write black (PassOutput): no ray, or denoised SSR adds its light back later.
    const bool holdoutPixel = Feature(kFeatureHoldouts) && draws[instances[inSlot].draw].holdout != 0u;
    float ssrWeight = frame.ssr.x > 0.0 && !mirror && kShadingPass != 2u && reflects && !holdoutPixel
                          ? 1.0 - smoothstep(0.3, 0.6, perceptual) : 0.0;
    // The single ray's direction and width: anisotropic surfaces reflect
    // along their bent normal, as wide as the stretched lobe's mean alpha.
    vec3 ssrDir = reflect(-v, n);
    float ssrWidth = 2.0 * a;
#ifdef LAYERS
    if (layers.hair) {
        ssrWeight = 0.0;  // strand highlights: no mirror direction
    } else if (layers.anisotropy > 0.0) {
        vec2 alphas = AnisotropicAlphas(a);
        ssrDir = reflect(-v, BentNormal(n, v, perceptual));
        ssrWidth = 2.0 * sqrt(alphas.x * alphas.y);
    }
#endif
    // What SsrDenoise adds times its reflection (stored last: how much of
    // it shows through the fog and the surface's presence rides in alpha).
    vec3 ssrAdded = vec3(0.0);
    bool ssrAdds = false;
    if (frame.ssr.z >= 2.0 && ssrWeight > 0.0 && RtOpaque() && (frame.ssr.z < 4.0 || perceptual >= kSsrInterleaved)) {
        // At a lower resolution (Frame::movingSsrResolution): ssr_trace.frag
        // traces one pixel of each block after this pass and SsrDenoise
        // adds FssEss times the weight times the reflection. Denoised
        // (frame.ssr.z 2) a GGX-sampled ray, glossy up to roughness 0.8;
        // else (3) the mirror ray along ssrDir, as wide as ssrWidth.
        // Interleaved (4, Frame::ssrInterleave): the denoised ray of one
        // pixel of each block, another each frame, for surfaces rough
        // enough that the denoiser's kernel spans the block; smoother
        // ones trace here, below.
        const bool glossy = frame.ssr.z != 3.0, interleaved = frame.ssr.z >= 4.0;
        if (glossy) {
            ssrWeight = 1.0 - smoothstep(0.5, 0.8, perceptual);
#ifdef LAYERS
            ssrWeight *= 1.0 - layers.anisotropy;
#endif
        }
        ivec2 px = ivec2(gl_FragCoord.xy);
        vec3 m = v + ssrDir;  // the mirror ray's normal
        imageStore(ssrRays, px, vec4(specular, interleaved ? 0.0 : glossy ? a : 0.5 * ssrWidth));
        if (interleaved) {
            imageStore(ssrDirections, px, vec4(0.0, 0.0, -1.0, a));
        }
        imageStore(ssrNormals, px, vec4(OctEncode(glossy || dot(m, m) < 1e-8 ? n : normalize(m)), OctEncode(ng)));
        ssrAdded = FssEss * ssrWeight;
        ssrAdds = true;
        specular *= 1.0 - ssrWeight;
        domeKept *= 1.0 - ssrWeight;  // ponytail: as the denoised rays below
    } else if (frame.ssr.z > 0.0 && ssrWeight > 0.0 && RtOpaque()) {
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
        ssrAdded = FssEss * ssrWeight;
        ssrAdds = true;
        specular *= 1.0 - ssrWeight;
        domeKept *= 1.0 - ssrWeight;  // ponytail: the denoised rays' misses aren't in the splits
    } else if (ssrWeight > 0.0) {
        vec4 hit;
        // The lobe's width: reflected directions spread about twice its
        // half-vectors' alpha (roughness 0.2 was sharper than Karma's).
        ssrWeight *= TraceReflection(inWorldPos, ssrDir, ssrWidth, hit);
        specular = mix(specular, hit.rgb + (1.0 - hit.a) * specular, ssrWeight);
        domeKept *= 1.0 - ssrWeight * hit.a;
    }
    if (LightSplits() && Feature(kFeatureRtReflections) && RtOpaque()) {
        domeKept *= 1.0 - imageLoad(rtReflectionImage, ivec2(gl_FragCoord.xy)).a;  // as RtReflection
    }
    // Under a coat the traced reflection is the coat's (CoatReflection).
    vec3 coatReflection = vec3(0.0);
#ifdef LAYERS
    if (layers.coat > 0.0) {
        coatReflection = CoatReflection(n, v, finalGather ? Lavg * occlusion : Lavg * diffuseAO + SsgiDiffuse(),
                                        mirrorIndex);
    } else
#endif
    {
        specular = GlassReflection(RtReflection(specular, inWorldPos, n, perceptual));
    }
    // The diffuse colour already lost what the specular reflects (SetupBase).
    vec3 kd = DiffuseAlbedo(diffuseColor, NoV);
    // With SSGI the bounces are traced: the visibility hides the rest alone.
    vec3 splitAmbient = color;
    color += FssEss * specular + kd * indirectDiffuse;
    vec3 splitIndirectGlossy = FssEss * specular;
    vec3 splitIndirectDiffuse = color - splitAmbient - splitIndirectGlossy;
    // Caustics (caustics.glsl): the photons that came through or off the
    // casters, on a receiver's diffuse surface; indirect diffuse light.
    vec3 splitCaustics = vec3(0.0);
    if (Feature(kFeatureCaustics) && (draws[instances[inSlot].draw].caustics & kCausticsReceive) != 0u) {
        splitCaustics = kd * CausticDiffuse(inWorldPos, n, true);
        color += splitCaustics;
        splitIndirectDiffuse += splitCaustics;
    }
    // Karma's convention (frame.aovs.y): the dome's light that reaches the
    // surface unblocked is direct. Diffuse: its irradiance under the
    // visibility (AO, or SSGI's); what GI's probes, the multi-bounce
    // approximation and SSGI add beyond it is indirect. Glossy: its
    // reflection under the specular occlusion where no probe, mirror, SSR
    // or traced reflection replaced it. At most the whole (where the probes
    // see less of the dome than the AO does), so the splits still add up.
    vec3 splitDomeGlossy = vec3(0.0), domeDirect = vec3(0.0);
    if (LightSplits() && frame.aovs.y != 0u) {
        domeDirect = min(kd * domeDiffuse * diffuseAO, splitIndirectDiffuse);
        // What its occlusion took is its shadow (Karma's shadow;, the
        // dome's light unoccluded minus occluded).
        splitShadow += kd * domeDiffuse - domeDirect;
        splitShadowDiffuse += kd * domeDiffuse - domeDirect;
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
        color += LayerAmbient(n, v, Lavg, diffuseAO, specularAO, coatReflection);
        splitCoat += gCoatAmbient;
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
        color = ToonShade(color, emissive, BaseToonAlbedo() * alpha);
    }
    alpha = TransmissionEnd(color, n, v, perceptual);
    color *= gPresence;
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
    if (ssrAdds) {
        imageStore(ssrFactor, ivec2(gl_FragCoord.xy), vec4(ssrAdded, 1.0 + gPresence * fogTransmittance));
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
        // Holdouts are black: none of their light, but their shadow
        // (Karma's holdouts;shadow;CDL, a shadow matte).
        bool holdout = Feature(kFeatureHoldouts) && draws[instances[inSlot].draw].holdout != 0u;
        float keep = holdout ? 0.0 : 1.0;
        imageStore(lightSplits, ivec3(pixel, 0), vec4(keep * (splitDirectDiffuse + splitThrough), fogTransmittance));
        imageStore(lightSplits, ivec3(pixel, 1), vec4(keep * splitIndirectDiffuse, 1.0));
        imageStore(lightSplits, ivec3(pixel, 2), vec4(keep * (splitDirect - splitDirectDiffuse + splitDomeGlossy), 1.0));
        imageStore(lightSplits, ivec3(pixel, 3), vec4(keep * (splitIndirectGlossy + gRefractedLight), 1.0));
        int splitLayers = imageSize(lightSplits).z;
        if (splitLayers > 4) {  // the split AOVs (aov.comp takes 4..6 out of diffuse and glossy)
            // The groups: the dome's direct light (Karma's convention) and
            // the sun's through thin subsurface parts too.
            if (frame.aovs.y != 0u) {
                SplitGroup(int(frame.aovs.z) - 1, domeDirect + splitDomeGlossy);
            }
            if (frame.counts.y != kNone) {
                SplitGroup(LightGroup(lights[frame.counts.y]), splitThrough);
            }
            imageStore(lightSplits, ivec3(pixel, 4), vec4(keep * gRefractedLight, 1.0));
            imageStore(lightSplits, ivec3(pixel, 5), vec4(keep * (gSubsurfaceShare * splitDirectDiffuse + splitThrough), 1.0));
            imageStore(lightSplits, ivec3(pixel, 6), vec4(keep * gSubsurfaceShare * splitIndirectDiffuse, 1.0));
            imageStore(lightSplits, ivec3(pixel, 7), vec4(keep * splitCoat, 1.0));
            imageStore(lightSplits, ivec3(pixel, 8), holdout ? vec4(splitShadowDiffuse, 1.0) : vec4(splitShadow, 0.0));
            imageStore(lightSplits, ivec3(pixel, kCausticsLayer), vec4(keep * splitCaustics, 1.0));
            for (int g = 0; g < min(splitLayers - kGroupLayer, int(kMaxLightGroups)); ++g) {
                imageStore(lightSplits, ivec3(pixel, kGroupLayer + g), vec4(keep * splitGroups[g], 1.0));
            }
        }
    }
    outPrimId = inPrimId;
    outInstanceId = inInstanceId;
    vec4 now = frame.prevViewProj * vec4(inWorldPos, 1.0);
    vec4 before = frame.prevViewProj * vec4(inPrevWorldPos, 1.0);
    outVelocity = before.w > 0.0 && now.w > 0.0 ? (before.xy / before.w - now.xy / now.w) * 0.5 : vec2(0.0);
    PassOutput(color, alpha);
}
