#version 450
#extension GL_EXT_nonuniform_qualifier : require

const float PI = 3.14159265359;
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
    vec4 params;   // metallic, roughness, occlusion
    TexRef tex[6]; // baseColor, emissive, metallic, roughness, occlusion, normal
};
struct Light {
    vec4 direction;  // towards the light
    vec4 irradiance;
};

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };
layout(std430, set = 0, binding = 2) readonly buffer Lights { Light lights[]; };

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec3 cameraPos;
    uint lightCount;
} pc;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec3 inColor;
layout(location = 4) flat in uint inMaterial;

layout(location = 0) out vec4 outColor;

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

void main()
{
    vec3 baseColor = inColor;
    vec3 emissive = vec3(0.0);
    float metallic = 0.0, roughness = 0.5;
    vec3 tangentNormal = vec3(0.0, 0.0, 1.0);
    bool hasNormalMap = false;
    if (inMaterial != kNoMaterial) {
        Material m = materials[inMaterial];
        baseColor = Fetch(m.tex[0], m.baseColor.w > 0.5 ? vec4(inColor, 1.0) : m.baseColor).rgb;
        emissive = Fetch(m.tex[1], m.emissive).rgb;
        metallic = Fetch(m.tex[2], m.params.xxxx).r;
        roughness = Fetch(m.tex[3], m.params.yyyy).r;
        // occlusion (tex[4]) only scales indirect light, which arrives with IBL.
        hasNormalMap = m.tex[5].index >= 0;
        tangentNormal = Fetch(m.tex[5], vec4(0.0, 0.0, 1.0, 0.0)).xyz;
    }

    vec3 v = normalize(pc.cameraPos - inWorldPos);
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
    float a = max(roughness, 0.045);
    a *= a;
    vec3 diffuseColor = baseColor * (1.0 - metallic);
    vec3 f0 = mix(vec3(0.04), baseColor, metallic);  // ior 1.5
    float NoV = max(dot(n, v), 1e-4);

    vec3 color = emissive;
    for (uint i = 0; i < pc.lightCount; ++i) {
        vec3 l = lights[i].direction.xyz;
        float NoL = dot(n, l);
        if (NoL <= 0.0) {
            continue;
        }
        vec3 h = normalize(v + l);
        float NoH = max(dot(n, h), 0.0);
        float LoH = max(dot(l, h), 0.0);
        vec3 specular = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F_Schlick(f0, LoH);
        color += (diffuseColor / PI + specular) * lights[i].irradiance.rgb * NoL;
    }
    outColor = vec4(color, 1.0);
}
