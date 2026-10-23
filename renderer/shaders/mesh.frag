#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "lights.glsl"
#include "material.glsl"
#include "scene.glsl"

// Depth is read-only here (the prepass wrote it), so the depth test can run
// before the shader even though cutouts discard.
layout(early_fragment_tests) in;
layout(set = 0, binding = 4) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 5) uniform samplerCube envCube;
layout(set = 0, binding = 6) uniform sampler2D brdfLut;
layout(set = 0, binding = 11) uniform sampler2D ambientOcclusion;  // GTAO, 1 = unoccluded
layout(set = 0, binding = 12) uniform sampler2D previousColor;     // last frame's HDR result, premultiplied
layout(set = 0, binding = 13) uniform sampler2D sceneDepth;        // prepass depth, window [0, 1]

// Local lights' shadows (Renderer::_PlanLocalShadows): per light its first
// atlas tile; spots up to kSpotShadowCone have one, other lights six cube
// faces along +X, -X, +Y, -Y, +Z, -Z.
const float kSpotShadowCone = 1.3962634;
struct ShadowTile {
    mat4 viewProj;
    vec4 rect;    // atlas uv offset, uv scale
    vec4 params;  // x: shadow texel size per unit of distance from the light, y/z: near/far, w: light size
};
layout(set = 0, binding = 17) uniform sampler2DShadow shadowAtlas;
layout(std430, set = 0, binding = 18) readonly buffer LightShadows { uint lightShadow[]; };
layout(std430, set = 0, binding = 19) readonly buffer ShadowTiles { ShadowTile shadowTiles[]; };
layout(set = 0, binding = 20) uniform sampler2D shadowAtlasDepth;  // same atlas, raw depths

const vec2 kPoisson[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2(0.34495938, 0.29387760), vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379), vec2(0.44323325, -0.97511554),
    vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367),
    vec2(0.14383161, -0.14100790));

// mesh.vert's packed outputs.
layout(location = 0) in vec4 inWorldPosPrevX;
layout(location = 1) in vec4 inNormalPrevY;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inColorPrevZ;
layout(location = 4) flat in uint inSlot;  // instance slot: draw (material, prim id) and instance index
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

#include "brdf.glsl"

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
        return AnisotropicSpecular(n, v, l, a, f0, NoV);  // brdf.glsl
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
    uint tile = lightShadow[index];
    if (tile == kNone) {
        return 1.0;
    }
    vec3 fromLight = p - light.position.xyz;
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
    float angle = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * float(frame.localLights.y),
                                                           vec2(0.06711056, 0.00583715))));
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

// Sphere, rect or disk light (UsdLux semantics as in hdEmbree: cone and
// focus shaping around the emission axis; rects and disks emit from their
// -Z side). Diffuse: Frostbite's horizon-clipped sphere/disk illuminance;
// for rects the exact polygon form factor vector with Heitz's horizon
// approximation.
// Specular: Karis' representative point with (a / a')^2 normalization.
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
    float diffuseE, specularE, sourceRadius;
    vec3 l;
    if (uint(light.position.w) == kLightSphere) {
        float sinSigmaSqr = min(light.axis.w * light.axis.w / (d * d), 1.0);
        diffuseE = SphereIlluminance(dot(n, toLight / d), sinSigmaSqr);
        specularE = PI * sinSigmaSqr;
        sourceRadius = light.halfWidth.w;
        vec3 centerToRay = dot(toLight, r) * r - toLight;
        l = normalize(toLight + centerToRay * clamp(sourceRadius / max(length(centerToRay), 1e-8), 0.0, 1.0));
    } else {
        if (dot(toLight, light.axis.xyz) >= 0.0) {
            return vec3(0.0);  // behind the emitting side
        }
        vec3 hw = light.halfWidth.xyz, hh = light.halfHeight.xyz;
        bool rect = uint(light.position.w) == kLightRect;
        if (rect) {
            vec3 corners[4] = vec3[4](toLight + hw + hh, toLight - hw + hh, toLight - hw - hh, toLight + hw - hh);
            vec3 formFactor = vec3(0.0);
            for (int i = 0; i < 4; ++i) {
                vec3 v0 = normalize(corners[i]), v1 = normalize(corners[(i + 1) % 4]);
                vec3 c = cross(v0, v1);
                float len = length(c);
                if (len > 1e-8) {
                    formFactor += acos(clamp(dot(v0, v1), -1.0, 1.0)) * c / len;
                }
            }
            formFactor /= 2.0 * PI;
            if (dot(formFactor, toLight) < 0.0) {
                formFactor = -formFactor;  // winding-independent: points at the light
            }
            float f = length(formFactor);
            diffuseE = PI * max((f * f + dot(formFactor, n)) / (f + 1.0), 0.0);
            specularE = PI * f;
            sourceRadius = sqrt(4.0 * length(cross(hw, hh)) / PI);
        } else {
            float r2 = length(cross(hw, hh));  // radius^2 of the equal-area circle
            float cosLight = clamp(dot(light.axis.xyz, -toLight / d), 0.0, 1.0);
            float sinSigmaSqr = r2 / (r2 + d * d);
            diffuseE = SphereIlluminance(dot(n, toLight / d), sinSigmaSqr) * cosLight;
            specularE = PI * sinSigmaSqr * cosLight;
            sourceRadius = sqrt(r2);
        }
        // Where the reflection ray meets the light's plane, clamped to the
        // rectangle or disk (in units of its half extents).
        float rDotAxis = dot(r, light.axis.xyz);
        vec3 q = rDotAxis < -1e-4 ? r * (dot(toLight, light.axis.xyz) / rDotAxis) - toLight : vec3(0.0);
        vec2 uv = vec2(dot(q, hw) / dot(hw, hw), dot(q, hh) / dot(hh, hh));
        uv = rect ? clamp(uv, -1.0, 1.0) : uv / max(length(uv), 1.0);
        l = normalize(toLight + hw * uv.x + hh * uv.y);
    }
    float aWide = clamp(a + sourceRadius / (3.0 * d), 0.0, 1.0);
    vec3 diffuse = diffuseColor / PI * diffuseE * light.halfHeight.w;
    vec3 specular = Specular(n, v, l, a, f0, NoV) * energyCompensation * specularE *
                    (a * a) / (aWide * aWide) * light.shaping.w;
    specular += LayerLight(n, v, l, NoV, sourceRadius / (3.0 * d)) * specularE * light.shaping.w;
    return radiance * (diffuse + specular) * LocalShadow(index, light, p, ng);
}

// Cascaded shadow lookup with normal offset and 3x3 hardware PCF.
float Shadow(vec3 p, vec3 ng, vec3 l)
{
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

// Screen-space reflection: marches the ray linearly through NDC (where
// it stays a straight line with affine depth) against the prepass depth,
// refines the first crossing by bisection and shades it with last frame's
// result reprojected to the hit. Returns the hit's weight (0 = miss) and
// its premultiplied radiance in `hit`.
// ponytail: mirror rays only, faded out from roughness 0.3 to 0.6; glossy
// SSR would jitter the ray by GGX sampling and let TAA average it.
float TraceReflection(vec3 p, vec3 r, out vec4 hit)
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
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * frame.ssr.y,
                                               vec2(0.06711056, 0.00583715))));
    float uPrev = 0.0;
    vec3 sPrev = s0;
    for (int i = 0; i < steps; ++i) {
        float u = mix(uStart, uEnd, (float(i) + noise) / float(steps));
        vec3 s = mix(s0, s1, u);
        float z = s.z * 0.5 + 0.5;
        if (z >= 1.0) {
            break;  // beyond the far plane: only sky, which the dome provides
        }
        float scene = SceneDepth(s.xy);
        // Behind this pixel's surface by about a step: the ray went through
        // it. Further behind, it passes behind a separate object. Surfaces
        // seen at grazing angles put a crossing ray far behind along the
        // view ray, so objects are also assumed 5% of their distance thick.
        bool crossed = false;
        if (scene < 1.0 && z > scene) {
            vec3 ray = Unproject(s);
            vec3 surface = Unproject(vec3(s.xy, scene * 2.0 - 1.0));
            float thickness = max(2.0 * length(ray - Unproject(sPrev)),
                                  0.05 * length(surface - frame.cameraPos.xyz));
            crossed = length(ray - surface) <= thickness;
        }
        if (crossed) {
            float lo = uPrev, hi = u;
            for (int k = 0; k < 5; ++k) {
                float mid = 0.5 * (lo + hi);
                vec3 m = mix(s0, s1, mid);
                if (m.z * 0.5 + 0.5 > SceneDepth(m.xy)) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            vec2 ndc = mix(s0.xy, s1.xy, hi);
            vec4 prev = frame.prevViewProj * vec4(Unproject(vec3(ndc, SceneDepth(ndc) * 2.0 - 1.0)), 1.0);
            vec2 prevUv = prev.xy / prev.w * 0.5 + 0.5;
            if (prev.w <= 0.0 || any(lessThan(prevUv, vec2(0.0))) || any(greaterThan(prevUv, vec2(1.0)))) {
                return 0.0;
            }
            hit = textureLod(previousColor, prevUv, 0.0);
            // Fade towards the screen edges, beyond which the ray is blind.
            vec2 edge = 1.0 - abs(ndc);
            return smoothstep(0.0, 0.1, min(edge.x, edge.y));
        }
        uPrev = u;
        sPrev = s;
    }
    return 0.0;
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

    vec3 v = normalize(frame.cameraPos.xyz - inWorldPos);
    // Geometric normal facing the viewer: renders everything double-sided and
    // makes winding/orientation and normal sign irrelevant.
    vec3 ng = normalize(cross(dFdx(inWorldPos), dFdy(inWorldPos)));
    if (dot(ng, v) < 0.0) {
        ng = -ng;
    }
    vec3 n = dot(inNormal, inNormal) > 1e-12 ? normalize(inNormal) : ng;
    if (dot(n, ng) < 0.0) {
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
        vec3 radiance = (diffuseColor / PI * NoL * light.halfHeight.w +
                         (Specular(n, v, l, a, f0, NoV) * energyCompensation + LayerLight(n, v, l, NoV, 0.0)) *
                             light.shaping.w) *
                        light.color.rgb;
        if (i == frame.counts.y) {
            radiance *= Shadow(inWorldPos, ng, l);
        }
        color += radiance;
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

    // Image-based lighting from the dome, if any.
    vec3 specular = vec3(0.0), Lavg = vec3(0.0);
    if (frame.envRadiance.w > 0.0) {
        mat3 toLocal = mat3(frame.envWorldToLocal);
        vec3 r = normalize(toLocal * reflect(-v, BentNormal(n, v, perceptual)));
        float lod = perceptual * float(frame.counts.z - 1u);
        specular = textureLod(envCube, r, lod).rgb * frame.envRadiance.rgb;
        Lavg = max(Irradiance(normalize(toLocal * n)), 0.0) * frame.envRadiance.rgb / PI;
    }
    // Material and screen-space occlusion; specular occlusion derived from
    // it (Lagarde 2014), as in Filament.
    float diffuseAO = min(occlusion, texelFetch(ambientOcclusion, ivec2(gl_FragCoord.xy), 0).r);
    float specularAO = clamp(pow(NoV + diffuseAO, exp2(-16.0 * a - 1.0)) - 1.0 + diffuseAO, 0.0, 1.0);
    specular *= MultiBounce(specularAO, f0);
    // Screen-space reflections replace the dome where they hit something;
    // where the hit is partly background the dome shows through.
    float ssrWeight = frame.ssr.x > 0.0 ? 1.0 - smoothstep(0.3, 0.6, perceptual) : 0.0;
#ifdef LAYERS
    ssrWeight *= 1.0 - layers.anisotropy;  // mirror rays can't follow a stretched highlight (brdf.glsl)
#endif
    if (ssrWeight > 0.0) {
        vec4 hit;
        ssrWeight *= TraceReflection(inWorldPos, reflect(-v, n), hit);
        specular = mix(specular, hit.rgb + (1.0 - hit.a) * specular, ssrWeight);
    }
    float Ems = 1.0 - Ess;
    vec3 Favg = f0 + (1.0 - f0) / 21.0;
    vec3 Fms = FssEss * Favg / (1.0 - Ems * Favg);
    vec3 kd = diffuseColor * (1.0 - FssEss - Fms * Ems);
    color += FssEss * specular + (Fms * Ems + kd) * Lavg * MultiBounce(diffuseAO, diffuseColor);
    color += LayerAmbient(n, v, Lavg, diffuseAO, specularAO);
    color = ToonShade(color, emissive, diffuseColor + f0);
    // Cutouts: last, so every derivative above saw the whole quad.
    if (!present) {
        discard;
    }
    outColor = vec4(color, alpha);
    outPrimId = inPrimId;
    outInstanceId = inInstanceId;
    vec4 now = frame.prevViewProj * vec4(inWorldPos, 1.0);
    vec4 before = frame.prevViewProj * vec4(inPrevWorldPos, 1.0);
    outVelocity = before.w > 0.0 && now.w > 0.0 ? (before.xy / before.w - now.xy / now.w) * 0.5 : vec2(0.0);
}
