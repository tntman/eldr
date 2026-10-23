// Materials and their texture lookups, shared by shading (mesh.frag) and the
// alpha-tested depth passes (mask.frag). Include after frame.glsl (textures).
// std430 mirrors of GpuTexRef/GpuMaterial in scene.cpp.

const uint kNoMaterial = 0xFFFFFFFFu;

struct TexRef {
    int index;     // -1: none; with udim the first of its 100 entries in udimTiles
    int channel;   // 0..3, or -1 for all
    int udim;
    int pad;
    vec4 scale;
    vec4 bias;
    vec4 uvRow0;  // texture coordinates: (dot(uvRow0.xyz, (u, v, 1)), dot(uvRow1.xyz, ...))
    vec4 uvRow1;
};
struct Material {
    vec4 baseColor;  // w = 1: use the mesh's displayColor
    vec4 emissive;   // w: opacity threshold; > 0 makes the material a cutout
    vec4 params;     // metallic, roughness, occlusion, opacity
    TexRef tex[7];   // baseColor, emissive, metallic, roughness, occlusion, normal, opacity
    uvec4 program;   // first op, ops, first constant, first image (program.glsl)
    vec4 specular;   // dielectric specular weight times colour
};
layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };
layout(std430, set = 0, binding = 25) readonly buffer UdimTiles { int udimTiles[]; };  // -1: missing tile

vec4 Fetch(TexRef r, vec4 constant, vec2 uv)
{
    if (r.index < 0) {
        return constant;
    }
    int index = r.index;
    // UsdTransform2d / MaterialX place2d; gradients through the same matrix.
    mat2 a = mat2(r.uvRow0.x, r.uvRow1.x, r.uvRow0.y, r.uvRow1.y);
    vec2 dx = a * dFdx(uv), dy = a * dFdy(uv);
    uv = a * uv + vec2(r.uvRow0.z, r.uvRow1.z);
    vec2 tileUv = uv;
    if (r.udim != 0) {
        // UDIM: tile 1001 + u + 10 v, sampled with the unwrapped uv's
        // gradients so mips don't jump at tile borders. Missing tiles and uv
        // outside 10 x 10 tiles show the constant.
        vec2 tile = floor(uv);
        if (any(lessThan(tile, vec2(0.0))) || any(greaterThan(tile, vec2(9.0)))) {
            return constant;
        }
        index = udimTiles[r.index + int(tile.x) + 10 * int(tile.y)];
        if (index < 0) {
            return constant;
        }
        tileUv -= tile;
    }
    vec4 t = textureGrad(textures[nonuniformEXT(index)], tileUv, dx, dy) * r.scale + r.bias;
    return r.channel < 0 ? t : vec4(t[r.channel]);
}

// False where a cutout material has no surface (opacity under its threshold).
bool Present(Material m, vec2 uv)
{
    return m.emissive.w <= 0.0 || Fetch(m.tex[6], m.params.wwww, uv).r >= m.emissive.w;
}
