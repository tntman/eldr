// Displacement (vkr::Material::displacement): every pass's vertex stage
// moves vertices along their normal (the same positions everywhere), the
// shading pass bumps the normal by the height map's slope. Include after
// frame.glsl and scene.glsl; the fragment stage defines DISPLACE_FRAGMENT
// and includes material.glsl first (TexRef, Fetch, udimTiles).
// std430 mirror of GpuDisplacement in scene.cpp.

#ifndef DISPLACE_FRAGMENT
// material.glsl's TexRef and UDIM table (that file's Fetch needs
// screen-space derivatives, which other stages lack).
struct TexRef {
    int index;
    int channel;
    int udim;
    int pad;
    vec4 scale;
    vec4 bias;
    vec4 uvRow0;
    vec4 uvRow1;
};
layout(std430, set = 0, binding = 25) readonly buffer UdimTiles { int udimTiles[]; };
#endif

struct Displacement {
    vec4 params;  // x: the offset without a texture, y: the largest offset either way
    TexRef tex;
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

#ifndef DISPLACE_FRAGMENT
// `position` moved along `normal` (object space) by the material's offset
// at uv: its texture's texel (top mip), else its constant. As it is without
// displacement or a normal.
// ponytail: the top mip can alias where a map's texels outnumber the
// vertices; pick a level from the edge length if that shows.
vec3 Displace(vec3 position, vec3 normal, uint material, vec2 uv)
{
    if (!kDisplacement || (frame.features.x & kFeatureDisplacement) == 0u || material == 0xFFFFFFFFu ||
        dot(normal, normal) < 1e-12) {
        return position;
    }
    Displacement d = displacements[material];
    float offset = d.params.x;
    int index = d.tex.index;
    if (index >= 0) {
        uv = mat2(d.tex.uvRow0.x, d.tex.uvRow1.x, d.tex.uvRow0.y, d.tex.uvRow1.y) * uv +
             vec2(d.tex.uvRow0.z, d.tex.uvRow1.z);
        if (d.tex.udim != 0) {  // as material.glsl's Fetch
            vec2 tile = floor(uv);
            bool inside = all(greaterThanEqual(tile, vec2(0.0))) && all(lessThanEqual(tile, vec2(9.0)));
            index = inside ? udimTiles[d.tex.index + int(tile.x) + 10 * int(tile.y)] : -1;
            uv -= tile;
        }
    }
    if (index >= 0) {
        vec4 t = textureLod(textures[nonuniformEXT(index)], uv, 0.0) * d.tex.scale + d.tex.bias;
        offset = d.tex.channel < 0 ? t.r : t[d.tex.channel];
    }
    return position + normalize(normal) * offset * frame.geometry.x;
}
#else
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
    if (material == kNoMaterial || displacements[material].tex.index < 0) {
        return n;
    }
    Displacement d = displacements[material];
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
