#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"

const uint kNoMaterial = 0xFFFFFFFFu;

// std430 mirrors of GpuTexRef/GpuMaterial/GpuLight in scene.cpp.
struct TexRef {
    int index;     // -1: none
    int channel;   // 0..3, or -1 for all
    int pad0, pad1;
    vec4 scale;
    vec4 bias;
};
struct Material {
    vec4 baseColor;  // w = 1: use the mesh's displayColor
    vec4 emissive;
    vec4 params;     // metallic, roughness, occlusion
    TexRef tex[6];   // baseColor, emissive, metallic, roughness, occlusion, normal
};
struct Light {
    vec4 direction;  // towards the light
    vec4 irradiance;
};

layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };
layout(std430, set = 0, binding = 2) readonly buffer Lights { Light lights[]; };
layout(set = 0, binding = 4) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 5) uniform samplerCube envCube;
layout(set = 0, binding = 6) uniform sampler2D brdfLut;
layout(set = 0, binding = 11) uniform sampler2D ambientOcclusion;  // GTAO, 1 = unoccluded
layout(set = 0, binding = 12) uniform sampler2D previousColor;     // last frame's HDR result, premultiplied
layout(set = 0, binding = 13) uniform sampler2D sceneDepth;        // prepass depth, window [0, 1]

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec3 inColor;
layout(location = 4) flat in uint inMaterial;
layout(location = 5) flat in int inPrimId;
layout(location = 6) flat in int inInstanceId;

layout(location = 0) out vec4 outColor;
layout(location = 1) out int outPrimId;      // Hydra picking / selection
layout(location = 2) out int outInstanceId;

vec4 Fetch(TexRef r, vec4 constant)
{
    if (r.index < 0) {
        return constant;
    }
    vec4 t = texture(textures[nonuniformEXT(r.index)], inUv) * r.scale + r.bias;
    return r.channel < 0 ? t : vec4(t[r.channel]);
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
    const int kSteps = 32;
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
    for (int i = 0; i < kSteps; ++i) {
        float u = mix(uStart, uEnd, (float(i) + noise) / float(kSteps));
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
    if (inMaterial != kNoMaterial) {
        Material m = materials[inMaterial];
        baseColor = Fetch(m.tex[0], m.baseColor.w > 0.5 ? vec4(inColor, 1.0) : m.baseColor).rgb;
        emissive = Fetch(m.tex[1], m.emissive).rgb;
        metallic = Fetch(m.tex[2], m.params.xxxx).r;
        roughness = Fetch(m.tex[3], m.params.yyyy).r;
        occlusion = clamp(Fetch(m.tex[4], m.params.zzzz).r, 0.0, 1.0);
        hasNormalMap = m.tex[5].index >= 0;
        tangentNormal = Fetch(m.tex[5], vec4(0.0, 0.0, 1.0, 0.0)).xyz;
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
    if (hasNormalMap) {
        n = normalize(CotangentFrame(n, inWorldPos, inUv) * tangentNormal);
    }

    metallic = clamp(metallic, 0.0, 1.0);
    float perceptual = clamp(roughness, 0.045, 1.0);
    float a = perceptual * perceptual;
    vec3 diffuseColor = baseColor * (1.0 - metallic);
    vec3 f0 = mix(vec3(0.04), baseColor, metallic);  // ior 1.5
    float NoV = max(dot(n, v), 1e-4);

    // Split-sum DFG (A, B) and multiple-scattering compensation
    // (Fdez-Aguera 2019; Filament's energy compensation for direct light).
    vec2 ab = texture(brdfLut, vec2(NoV, perceptual)).rg;
    float Ess = ab.x + ab.y;
    vec3 FssEss = f0 * ab.x + ab.y;
    vec3 energyCompensation = 1.0 + f0 * (1.0 / max(Ess, 1e-4) - 1.0);

    vec3 color = emissive;
    for (uint i = 0; i < frame.counts.x; ++i) {
        vec3 l = lights[i].direction.xyz;
        float NoL = dot(n, l);
        if (NoL <= 0.0) {
            continue;
        }
        vec3 h = normalize(v + l);
        float NoH = max(dot(n, h), 0.0);
        float LoH = max(dot(l, h), 0.0);
        vec3 specular = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F_Schlick(f0, LoH);
        vec3 radiance = (diffuseColor / PI + specular * energyCompensation) * lights[i].irradiance.rgb * NoL;
        if (i == frame.counts.y) {
            radiance *= Shadow(inWorldPos, ng, l);
        }
        color += radiance;
    }

    // Image-based lighting from the dome, if any.
    vec3 specular = vec3(0.0), Lavg = vec3(0.0);
    if (frame.envRadiance.w > 0.0) {
        mat3 toLocal = mat3(frame.envWorldToLocal);
        vec3 r = normalize(toLocal * reflect(-v, n));
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
    outColor = vec4(color, 1.0);
    outPrimId = inPrimId;
    outInstanceId = inInstanceId;
}
