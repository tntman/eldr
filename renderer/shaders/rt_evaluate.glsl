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

// One refracted path from the first refracting surface s (hit: its hit),
// arriving along dir: rgb the light it brings, a 1, or -1 where it left the
// scene with no dome. `ray` steps the first microfacet normal along R2
// (stratified across a pixel's rays). Traced refraction's
// (rt_refraction.rgen) and glass seen in traced reflections
// (rt_pixels.rgen); the first surface's Fresnel, weight and tint are the
// caller's.
// Nested media: a stack of the media the ray is in (the camera in air), so
// an interface bends by the ratio of the iors on its two sides (liquid
// against glass) and absorbs by the medium the ray is in (an ice cube in a
// drink: the drink's again after the cube). Up to `interfaces` refractions
// (Karma's refract limit) and rt.skinned.z reflections inside (its reflect
// limit) bend the ray; past either, as Karma's Thinwall at Refract Limit,
// further surfaces let it through straight, the media still absorbing, up
// to kMaxHits surfaces in all (past them the path ends black).
// ponytail: 4 nested media, entered and left in order (no priorities).
const uint kMaxMedia = 4u, kMaxHits = 24u;
vec4 RefractedPath(Surface s, vec3 dir, float cone, uint interfaces, uvec3 seed, uint ray)
{
    const uint kMaskAll = kRtMaskSurfaces | kRtMaskTranslucent;
    vec3 throughput = vec3(1.0);
    vec4 media[kMaxMedia];  // rgb the transmission colour, a its depth (0: absorbs at the surface)
    float mediaIor[kMaxMedia];
    uint depth = 0u, refractions = 0u, reflections = 0u;
    for (uint bounce = 0u; bounce < kMaxHits; ++bounce) {
        if (bounce > 0u) {
            s = Evaluate(dir, cone);
        }
        Material m = s.m;
        vec2 u = Random2(seed + uvec3(0u, 0u, 104729u + bounce), 0u);
        // After the first surface: straight through, refracted, or its own
        // diffuse surface, by its fractions.
        bool straight = bounce > 0u && u.x < s.straight;
        if (bounce > 0u && !straight && u.x >= s.straight + s.refracted) {
            return vec4(throughput * ShadeHit(dir, seed, true, (rt.shadowInfo.w & 1u) != 0u, false, true), 1.0);
        }
        vec3 n = s.n, ng = s.ng;
        if (!straight) {
            bool thin = m.refraction.y > 0.0;
            bool exiting = !thin && s.backface;
            bool limit = refractions >= interfaces || reflections >= rt.skinned.z;
            float ior = max(m.refraction.x, 1e-3);
            // The iors on both sides: the medium the ray is in and the one
            // it enters (leaving: the one around this medium).
            float outer = depth > 0u ? mediaIor[depth - 1u] : 1.0;
            float under = depth > 1u ? mediaIor[depth - 2u] : 1.0;
            float eta = exiting ? ior / under : outer / ior;
            // A microfacet normal (facing the ray) by the roughness.
            vec3 t, bt;
            Basis(n, t, bt);
            vec3 v = -dir;
            vec3 h = SampleGgxVndf(vec3(dot(v, t), dot(v, bt), max(dot(v, n), 1e-4)), s.roughness * s.roughness,
                                   Random2(seed + uvec3(0u, 0u, 7907u * (bounce + 1u)), bounce == 0u ? ray : 0u));
            n = normalize(t * h.x + bt * h.y + n * h.z);
            vec3 r = thin || limit ? dir : refract(dir, n, eta);
            bool total = dot(r, r) == 0.0;
            // Schlick's cosine is the one on the thinner medium's side.
            float f0 = (eta - 1.0) / (eta + 1.0);
            f0 *= f0;
            float cosine = eta > 1.0 ? (total ? 0.0 : clamp(-dot(r, n), 0.0, 1.0)) : clamp(dot(n, v), 0.0, 1.0);
            float F = total ? 1.0 : f0 + (1.0 - f0) * pow(1.0 - cosine, 5.0);
            bool mirrored = !limit && (total || (bounce > 0u && u.y < F));
            if (!mirrored) {
                // Without a depth the colour tints on entry (an ior of 1
                // on both sides, as the shading pass tints those faces).
                bool tints = thin || (m.refraction.z <= 0.0 && (!exiting || m.refraction.x == 1.0));
                if (bounce > 0u && tints) {
                    throughput *= m.transmission.rgb;  // the first surface's tint is the shading pass's
                }
                if (exiting) {
                    depth -= depth > 0u ? 1u : 0u;
                } else if (!thin && depth < kMaxMedia) {
                    media[depth] = vec4(m.transmission.rgb, m.refraction.z);
                    mediaIor[depth] = ior;
                    ++depth;
                }
                ++refractions;
            } else {
                ++reflections;
            }
            dir = mirrored ? reflect(dir, n) : r;
        }
        TraceMask(OffsetRay(hit.position, dot(dir, ng) > 0.0 ? ng : -ng), dir, cone, kMaskAll);
        if (depth > 0u && media[depth - 1u].a > 0.0) {
            float travelled = hit.t < 0.0 ? 1e30 : hit.t;
            throughput *= pow(max(media[depth - 1u].rgb, vec3(1e-4)), vec3(travelled / media[depth - 1u].a));
        }
        if (hit.t < 0.0) {
            return vec4(throughput * (Dome(dir, 0.0) + SunDiscs(dir)), frame.envRadiance.w > 0.0 ? 1.0 : -1.0);
        }
        if ((hit.flags & kHitTranslucent) == 0u) {
            EvaluateHit(dir, cone);
            return vec4(throughput * ShadeHit(dir, seed, true, (rt.shadowInfo.w & 1u) != 0u, true, true), 1.0);
        }
    }
    return vec4(0.0, 0.0, 0.0, 1.0);  // ended: as Karma's paths past their limits
}
