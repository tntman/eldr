// Displacement (vkr::Material::displacement): every pass's vertex stage
// moves vertices along their normal (the same positions everywhere), the
// shading pass bumps the normal by the height map's slope. Include after
// frame.glsl and scene.glsl; the fragment stage defines DISPLACE_FRAGMENT
// and includes material.glsl first (TexRef, Fetch, udimTiles), mesh.vert
// DISPLACE_VERTEX (Displace, with displacement graphs).
// std430 mirror of GpuDisplacement in scene.cpp.

#ifndef DISPLACE_FRAGMENT
// material.glsl's Fetch with a footprint instead of screen derivatives.
#define UV_FOOTPRINT
#include "material.glsl"
#endif

struct Displacement {
    vec4 params;  // x: the offset without a texture, y: the largest offset either way
    TexRef tex;
    uvec4 program;  // a displacement graph (program.glsl), y = 0: none
};
layout(std430, set = 0, binding = 39) readonly buffer Displacements { Displacement displacements[]; };

// Pipelines of displaced materials (Scene::Gpu::kDisplaced) set this.
layout(constant_id = 10) const bool kDisplacement = false;

// The largest distance displacement moves an instance's surface, world
// units (culling bounds): its draw's bound (DrawInfo::boundsMax.w).
float DisplacementReach(DrawInfo d, Instance inst)
{
    if (d.boundsMax.w <= 0.0 || (frame.features.x & kFeatureDisplacement) == 0u) {
        return 0.0;
    }
    mat3 m = mat3(InstanceTransform(inst));
    return d.boundsMax.w * abs(frame.geometry.x) * max(length(m[0]), max(length(m[1]), length(m[2])));
}

#ifdef DISPLACE_VERTEX
// A displacement graph's inputs at this vertex (program.glsl's names; inUv
// is mesh.vert's), set by Displace.
vec3 inColor;
uint inSlot;
vec4 inPrimvars;
vec3 programPosition, programNormal;
mat4 programModel;
float programDisplacement;
#define PROGRAM_VERTEX
#include "program.glsl"

// `position` moved along `normal` (object space) by the material's offset
// at mesh.vert's inUv: its graph's, or its texture's texel, else its
// constant. Textures are read at the mip whose texels are as far apart as
// the mesh's vertices (DrawInfo::uvSpacing), so a map finer than the mesh
// doesn't alias. As it is without displacement or a normal.
// ponytail: one spacing per mesh (its mean edge), not per vertex.
vec3 Displace(vec3 position, vec3 normal, DrawInfo draw, uint slot, mat4 model, vec3 vertexColor)
{
    if (!kDisplacement || (frame.features.x & kFeatureDisplacement) == 0u || draw.material == 0xFFFFFFFFu ||
        dot(normal, normal) < 1e-12) {
        return position;
    }
    Displacement d = displacements[draw.material];
    rtUvFootprint = draw.uvSpacing;
    float offset;
    if (d.program.y > 0u && frame.geometry.w > 0.5) {  // Frame::displacementGraphs
        inColor = draw.vertexColors != 0u ? vertexColor : draw.color.rgb;
        inSlot = slot;
        inPrimvars = draw.primvarVertices == kFreeSlot
                         ? vec4(0.0)
                         : primvarVertices[draw.primvarVertices + uint(gl_VertexIndex) - draw.vertexBase];
        programPosition = position;
        programNormal = normal;
        programModel = model;
        programDisplacement = 0.0;
        vec3 c3 = vec3(0.0), e3 = vec3(0.0), t3 = vec3(0.0, 0.0, 1.0);
        float f0 = 0.0, f1 = 0.0, f2 = 0.0, f3 = 0.0;
        bool b = false;
        RunProgram(d.program, c3, e3, f0, f1, f2, t3, b, f3);
        offset = programDisplacement;
    } else {
        offset = Fetch(d.tex, d.params.xxxx, inUv).r;
    }
    return position + normalize(normal) * offset * frame.geometry.x;
}
#endif
#ifdef DISPLACE_FRAGMENT
// The displaced surface's shading normal: n bumped by the height map's
// slope (Mikkelsen 2010, the surface gradient, which only sees the
// tangential part of p's derivatives: displaced or not, the same), heights
// in world units by the instance's scale. A constant offset keeps n.
// ponytail: skinning's stretch doesn't scale the bump.
vec3 DisplacedNormal(vec3 n, uint slot, vec3 p, vec2 uv)
{
    if (!kDisplacement || (frame.features.x & kFeatureDisplacement) == 0u) {
        return n;
    }
    Instance inst = instances[slot];
    uint material = draws[inst.draw].material;
    if (material == kNoMaterial) {
        return n;
    }
    Displacement d = displacements[material];
    if (d.program.y > 0u && frame.geometry.w > 0.5) {
        // A graph's displaced surface: its own (facet) normal, on n's side.
        // ponytail: facets show on coarse meshes; a per-vertex height
        // varying would allow the surface gradient below.
        vec3 g = cross(dFdx(p), dFdy(p));
        g = dot(g, g) > 1e-30 ? normalize(g) : n;
        return dot(g, n) < 0.0 ? -g : g;
    }
    if (d.tex.index < 0) {
        return n;
    }
    float scale = pow(abs(determinant(mat3(InstanceTransform(inst)))), 1.0 / 3.0);
    float h = Fetch(d.tex, d.params.xxxx, uv).r * frame.geometry.x * scale;
    vec3 sigmaS = dFdx(p), sigmaT = dFdy(p);
    vec3 r1 = cross(sigmaT, n), r2 = cross(n, sigmaS);
    float det = dot(sigmaS, r1);
    vec3 gradient = sign(det) * (dFdx(h) * r1 + dFdy(h) * r2);
    vec3 bumped = abs(det) * n - gradient;
    return dot(bumped, bumped) > 1e-30 ? normalize(bumped) : n;
}
#endif
