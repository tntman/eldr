// A ray hit's material as the shading pass evaluates it (textures, the
// material program and its lobe inputs, the normal map): traced refraction's
// interfaces (rt_refraction.rgen) and the hits ShadeHit shades (EvaluateHit).
// Include after rt_shade.glsl.

// Material programs (program.glsl) at a hit: their inputs, set by Evaluate,
// and per-point geometry as in the vertex stage (no screen derivatives).
#define LAYERS
#define PROGRAM_VERTEX
vec2 inUv;
vec3 inColor;
uint inSlot;
vec4 inPrimvars;
vec3 programPosition, programNormal;
mat4 programModel;
float programDisplacement;
layout(std430, set = 0, binding = 32) readonly buffer PrimvarVertices { vec4 primvarVertices[]; };
#include "program.glsl"
#include "displace.glsl"

// A translucent hit's surface, its material as shading sees it.
struct Surface {
    vec3 n;           // shading normal (normal-mapped), facing the ray
    vec3 ng;          // geometric normal, facing the ray
    bool backface;    // the authored normal faces away from the ray
    float roughness;  // perceptual
    float refracted;  // the fractions TransmissionBegin splits it in: refracted,
    float straight;   // seen straight through (the rest: its diffuse surface)
    Material m;       // with the program's inputs (ProgramMaterial)
};

Surface Evaluate(vec3 dir, float cone)
{
    Surface s;
    uint slot = hit.slot, triangle = hit.triangle;
    DrawInfo d = draws[slot];
    uint v[3] = uint[3](RtVertex(slot, triangle, 0u), RtVertex(slot, triangle, 1u), RtVertex(slot, triangle, 2u));
    vec3 b = vec3(1.0 - hit.bary.x - hit.bary.y, hit.bary);
    vec3 p[3], objectN = vec3(0.0), color = vec3(0.0);
    vec2 uv[3];
    mat4 model = mat4(1.0);
    bool skinned = hit.instance >= rt.skinned.x;
    uint posed = skinned ? rtSkinned[hit.instance - rt.skinned.x].firstVertex - d.vertexBase : 0u;
    uint instance = skinned ? rtSkinned[hit.instance - rt.skinned.x].instance : hit.instance;
    if (!skinned) {
        model = InstanceTransform(instances[instance]);
    }
    // Displaced as rt_displace.comp displaces the BLAS's vertices, graphs
    // too (skinned ones are posed displaced, rt_skin.comp). First: a graph
    // sets the program's inputs, which the hit's own program needs below.
    bool displaced = rt.refraction.y != 0u && d.boundsMax.w > 0.0 && d.material != kNoMaterial &&
                     (frame.features.x & kFeatureDisplacement) != 0u;
    bool graph = displaced && displacements[d.material].program.y > 0u && frame.geometry.w > 0.5;
    vec3 shift[3] = vec3[3](vec3(0.0), vec3(0.0), vec3(0.0));
    for (int k = 0; k < 3 && displaced && !skinned; ++k) {
        vec3 vn = RtNormal(v[k]);
        if (dot(vn, vn) > 1e-12) {
            float offset = DisplacementOffset(displacements[d.material], d, instance, model, RtPosition(v[k]), vn,
                                              RtColor(v[k]), v[k], RtUv(v[k]));
            shift[k] = normalize(vn) * offset * frame.geometry.x;
        }
    }
    inSlot = instance;
    inPrimvars = vec4(0.0);
    for (int k = 0; k < 3; ++k) {
        p[k] = (skinned ? RtSkinnedPosition(posed + v[k]) : RtPosition(v[k])) + shift[k];
        objectN += b[k] * (skinned ? RtSkinnedNormal(posed + v[k]) : RtNormal(v[k]));
        uv[k] = RtUv(v[k]);
        color += b[k] * RtColor(v[k]);
        if (d.primvarVertices != kFreeSlot) {
            inPrimvars += b[k] * primvarVertices[d.primvarVertices + v[k] - d.vertexBase];
        }
    }
    vec3 w0 = (model * vec4(p[0], 1.0)).xyz, w1 = (model * vec4(p[1], 1.0)).xyz, w2 = (model * vec4(p[2], 1.0)).xyz;
    inUv = uv[0] * b.x + uv[1] * b.y + uv[2] * b.z;
    inColor = d.vertexColors != 0u ? color : d.color.rgb;
    programPosition = p[0] * b.x + p[1] * b.y + p[2] * b.z;
    programNormal = objectN;
    programModel = model;
    // Normals as the closest hit shader has them.
    vec3 ng = normalize(cross(w1 - w0, w2 - w0));
    vec3 n = dot(objectN, objectN) > 1e-12 ? normalize(transpose(inverse(mat3(model))) * objectN) : ng;
    // The ray cone's footprint in uv (material.glsl's Fetch).
    vec2 e1 = uv[1] - uv[0], e2 = uv[2] - uv[0];
    float worldArea = length(cross(w1 - w0, w2 - w0));
    float uvDet = e1.x * e2.y - e1.y * e2.x;
    rtUvFootprint = cone * hit.t * sqrt(abs(uvDet) / max(worldArea, 1e-20));
    if (graph) {
        n = dot(ng, n) < 0.0 ? -ng : ng;  // displace.glsl's DisplacedNormal: the facet, on n's side
    } else if (displaced && displacements[d.material].tex.index >= 0 && abs(uvDet) > 1e-20) {
        // displace.glsl's DisplacedNormal with uv derivatives: the surface
        // and its height (world units) a step along u and along v, on the
        // authored side.
        // ponytail: forward differences over at least 1/4096 of uv (a 4K
        // map's texel: bilinear weights have 8 bits).
        Displacement dm = displacements[d.material];
        vec3 dpdu = ((w1 - w0) * e2.y - (w2 - w0) * e1.y) / uvDet;
        vec3 dpdv = ((w2 - w0) * e1.x - (w1 - w0) * e2.x) / uvDet;
        float du = max(rtUvFootprint, 1.0 / 4096.0);
        float scale = pow(abs(determinant(mat3(InstanceTransform(instances[inSlot])))), 1.0 / 3.0) * frame.geometry.x;
        float h = Fetch(dm.tex, dm.params.xxxx, inUv).r;
        float hu = Fetch(dm.tex, dm.params.xxxx, inUv + vec2(du, 0.0)).r;
        float hv = Fetch(dm.tex, dm.params.xxxx, inUv + vec2(0.0, du)).r;
        n = BumpedNormal(n, dpdu * du, dpdv * du, (hu - h) * scale, (hv - h) * scale);
    }
    s.backface = dot(n, dir) > 0.0;
    ng = dot(ng, dir) > 0.0 ? -ng : ng;
    n = dot(n, ng) < 0.0 ? -n : n;

    // The material as mesh.frag reads it.
    Material m = materials[d.material];
    float opacity = m.emissive.w <= 0.0 ? clamp(Fetch(m.tex[6], m.params.wwww, inUv).r, 0.0, 1.0) : 1.0;
    float roughness = Fetch(m.tex[3], m.params.yyyy, inUv).r;
    bool hasNormalMap = m.tex[5].index >= 0;
    vec3 tangentNormal = Fetch(m.tex[5], vec4(0.0, 0.0, 1.0, 0.0), inUv).xyz;
    if (m.program.y > 0u && (frame.features.x & kFeatureMaterialGraphs) != 0u) {
        // From the closest hit's constants and textures, as mesh.frag starts;
        // the results go back to `hit` for ShadeHit.
        vec3 baseColor = hit.baseColor, emissive = hit.emissive;
        float metallic = hit.metallic, occlusion = 1.0;
        programLobes = 0u;
        RunProgram(m.program, baseColor, emissive, metallic, roughness, occlusion, tangentNormal, hasNormalMap,
                   opacity);
        m = ProgramMaterial(m);
        hit.baseColor = baseColor;
        hit.emissive = emissive;
        hit.metallic = clamp(metallic, 0.0, 1.0);
        hit.roughness = clamp(roughness, 0.045, 1.0);
    }
    if (hasNormalMap && (frame.features.x & kFeatureNormalMaps) != 0u) {
        // mesh.frag's CotangentFrame, from the triangle's edges instead of
        // screen derivatives.
        vec3 dp1 = w1 - w0, dp2 = w2 - w0;
        vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
        vec3 t = dp2perp * e1.x + dp1perp * e2.x;
        vec3 bt = dp2perp * e1.y + dp1perp * e2.y;
        float invmax = inversesqrt(max(max(dot(t, t), dot(bt, bt)), 1e-20));
        vec3 mapped = mat3(t * invmax, bt * invmax, n) * tangentNormal;
        n = dot(mapped, mapped) > 1e-12 ? normalize(mapped) : n;
    }
    s.n = n;
    s.ng = ng;
    s.roughness = clamp(roughness, 0.045, 1.0);
    // transparency.glsl's TransmissionBegin.
    s.straight = 1.0 - opacity;
    s.refracted = opacity * m.transmission.w;
    if (m.refraction.w > 0.0) {
        s.refracted += s.straight;
        s.straight = 0.0;
    }
    s.m = m;
    return s;
}

// Before ShadeHit: a hit on a material program gets the program's base
// colour, emission, metallic and roughness (the closest hit shader reads
// only constants and textures: a graph-coloured neighbour reflected grey).
void EvaluateHit(vec3 dir, float cone)
{
    uint material = draws[hit.slot].material;
    if (hit.t >= 0.0 && material != kNoMaterial && materials[material].program.y > 0u &&
        (frame.features.x & kFeatureMaterialGraphs) != 0u) {
        Evaluate(dir, cone);
    }
}
