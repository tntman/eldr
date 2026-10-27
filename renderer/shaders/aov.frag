#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "frame.glsl"
#include "material.glsl"
#include "scene.glsl"

// Extra AOVs' G-buffer (aov.h): the first opaque surface's shading normal,
// texture coordinates, base colour and emission, with the shading pass's
// vertex stage against its finished depth (read-only, so the depth test can
// run first although cutouts discard). The material inputs and the normal
// are mesh.frag's.
layout(early_fragment_tests) in;

layout(location = 0) in vec4 inWorldPosPrevX;
layout(location = 1) in vec4 inNormalPrevY;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec4 inColorPrevZ;
layout(location = 4) flat in uint inEntry;  // visible-list entry (frame.glsl's EntrySlot, LodKeep)
#define inSlot EntrySlot(inEntry)
#define inWorldPos (inWorldPosPrevX.xyz)
#define inNormal (inNormalPrevY.xyz)
#define inColor (inColorPrevZ.rgb)
#ifdef GRAPH
layout(location = 5) in vec4 inPrimvars;
#include "program.glsl"
#endif

layout(location = 0) out vec4 outNormalSt;  // octahedral world normal, st
layout(location = 1) out vec4 outAlbedo;    // base colour, metallic
layout(location = 2) out vec4 outEmission;

// As mesh.frag.
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

vec2 OctEncode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
}

void main()
{
    vec3 baseColor = inColor, emissive = vec3(0.0);
    float metallic = 0.0, roughness = 0.5, occlusion = 1.0, alpha = 1.0;
    vec3 tangentNormal = vec3(0.0, 0.0, 1.0);
    bool hasNormalMap = false, present = true;
    uint material = draws[instances[inSlot].draw].material;
    if (material != kNoMaterial) {
        Material m = materials[material];
        present = Present(m, inUv);
        baseColor = Fetch(m.tex[0], m.baseColor.w > 0.5 ? vec4(inColor, 1.0) : m.baseColor, inUv).rgb;
        emissive = Fetch(m.tex[1], m.emissive, inUv).rgb;
        metallic = Fetch(m.tex[2], m.params.xxxx, inUv).r;
        hasNormalMap = m.tex[5].index >= 0;
        tangentNormal = Fetch(m.tex[5], vec4(0.0, 0.0, 1.0, 0.0), inUv).xyz;
#ifdef GRAPH
        if (m.program.y > 0u && (frame.features.x & kFeatureMaterialGraphs) != 0u) {
            RunProgram(m.program, baseColor, emissive, metallic, roughness, occlusion, tangentNormal, hasNormalMap,
                       alpha);
        }
#endif
    }
    vec3 v = ViewDirection(inWorldPos);
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
    if (!present || !LodKeep(inEntry, gl_FragCoord.xy)) {
        discard;
    }
    outNormalSt = vec4(OctEncode(n), inUv);
    outAlbedo = vec4(baseColor, clamp(metallic, 0.0, 1.0));
    outEmission = vec4(emissive, 1.0);
}
