#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#define RAY_TRACING
#include "frame.glsl"
#include "scene.glsl"
#include "material.glsl"
#include "rt.glsl"

// The surface a ray hit: position, normals and the material's inputs at
// the hit (constants and textures; material programs are not run).
layout(location = 0) rayPayloadInEXT RtHit hit;
hitAttributeEXT vec2 barycentrics;

void main()
{
    uint slot = gl_InstanceCustomIndexEXT;
    DrawInfo d = draws[slot];
    uint triangle = uint(gl_PrimitiveID);
    uint v0 = RtVertex(slot, triangle, 0u), v1 = RtVertex(slot, triangle, 1u), v2 = RtVertex(slot, triangle, 2u);
    vec3 b = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics);
    vec3 p0 = RtPosition(v0), p1 = RtPosition(v1), p2 = RtPosition(v2);
    vec2 uv0 = RtUv(v0), uv1 = RtUv(v1), uv2 = RtUv(v2);
    vec2 uv = uv0 * b.x + uv1 * b.y + uv2 * b.z;
    vec3 dir = gl_WorldRayDirectionEXT;

    // Normals to world space by the inverse transpose; meshes without
    // normals are flat. Both end up facing the ray (everything is
    // double-sided); kHitBackface remembers the shading normal's side
    // (meshes without normals have no back: their winding means nothing).
    vec3 ng = normalize(vec3(cross(p1 - p0, p2 - p0) * gl_WorldToObjectEXT));
    vec3 objectN = RtNormal(v0) * b.x + RtNormal(v1) * b.y + RtNormal(v2) * b.z;
    bool normals = dot(objectN, objectN) > 1e-12;
    vec3 n = normals ? normalize(vec3(objectN * gl_WorldToObjectEXT)) : ng;
    hit.flags = normals && dot(n, dir) > 0.0 ? kHitBackface : 0u;
    ng = dot(ng, dir) > 0.0 ? -ng : ng;
    n = dot(n, ng) < 0.0 ? -n : n;

    // Texture footprint of the ray cone: its width at the hit in uv units
    // (the triangle's uv area per world area).
    mat3 toWorld = mat3(gl_ObjectToWorldEXT);
    float worldArea = length(cross(toWorld * (p1 - p0), toWorld * (p2 - p0)));
    vec2 e1 = uv1 - uv0, e2 = uv2 - uv0;
    float uvArea = abs(e1.x * e2.y - e1.y * e2.x);
    rtUvFootprint = hit.t * gl_HitTEXT * sqrt(uvArea / max(worldArea, 1e-20));

    vec3 color = d.vertexColors != 0u ? RtColor(v0) * b.x + RtColor(v1) * b.y + RtColor(v2) * b.z : d.color.rgb;
    vec3 baseColor = color, emissive = vec3(0.0);
    float metallic = 0.0, roughness = 0.5;
    if (d.material != kNoMaterial) {
        Material m = materials[d.material];
        baseColor = Fetch(m.tex[0], m.baseColor.w > 0.5 ? vec4(color, 1.0) : m.baseColor, uv).rgb;
        emissive = Fetch(m.tex[1], m.emissive, uv).rgb;
        metallic = clamp(Fetch(m.tex[2], m.params.xxxx, uv).r, 0.0, 1.0);
        roughness = Fetch(m.tex[3], m.params.yyyy, uv).r;
    }
    hit.position = gl_WorldRayOriginEXT + dir * gl_HitTEXT;
    hit.t = gl_HitTEXT;
    hit.normal = n;
    hit.geometric = ng;
    hit.roughness = clamp(roughness, 0.045, 1.0);
    hit.baseColor = baseColor;
    hit.metallic = metallic;
    hit.emissive = emissive;
}
