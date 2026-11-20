// Materials and their texture lookups, shared by shading (mesh.frag) and the
// alpha-tested depth passes (mask.frag). Include after frame.glsl (textures).
// std430 mirrors of GpuTexRef/GpuMaterial in scene.cpp.

const uint kNoMaterial = 0xFFFFFFFFu;

struct TexRef {
    int index;     // -1: none; with udim the first of its 100 entries in udimTiles
    int channel;   // 0..3, or -1 for all
    int udim;
    int pad;       // MaterialX address mode constant: bit 0 on u, 1 on v (outside [0, 1] the constant)
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
    vec4 specular;   // specular colour, w: weight (brdf.glsl's SetupBase)
    // Lobes (brdf.glsl).
    vec4 specularColor;  // specular workflow F0, w = 1 when used; w < 0: flat dielectric Fresnel (F90 = F0)
    vec4 coat;           // weight (0: none), roughness, F0
    vec4 coatColor;      // tints the base beneath
    vec4 sheen;          // weight (0: none), roughness
    vec4 sheenColor;
    vec4 thinFilm;       // weight (0: none), thickness (nm), ior
    vec4 anisotropy;     // amount (0: none), rotation (turns), roughness mapping (0 standard_surface, 1 OpenPBR, 2 glTF)
    vec4 toon;           // bands (0: none), softness
    vec4 transmission;  // tint (transmission colour), weight (transparency.glsl)
    vec4 refraction;    // ior, thin walled, transmission depth, opacity refracts
    vec4 subsurface;        // colour, weight (subsurface.glsl)
    vec4 subsurfaceRadius;  // per channel, world units; w: 1 = opacity presence (transparency.glsl)
    vec4 surface;           // model (0 UsdPreviewSurface, 1 standard_surface, 2 OpenPBR), diffuse roughness,
                            // coat affect colour (OpenPBR: coat darkening), coat affect roughness
};
layout(std430, set = 0, binding = 1) readonly buffer Materials { Material materials[]; };
layout(std430, set = 0, binding = 25) readonly buffer UdimTiles { int udimTiles[]; };  // -1: missing tile

// frame.features.x's anisotropy bit (renderer.cpp's Frame switches).
const uint kFeatureAnisotropy = 128u;

// The stretched GGX's alphas along the tangent and across it, at
// roughness `a`, by the material's anisotropy and its roughness mapping
// (Material::anisotropy.xz).
vec2 AnisotropicAlphas(float a, float anisotropy, int mapping)
{
    float at, ab;
    if (mapping == 2) {
        // glTF (KHR_materials_anisotropy, MaterialX's gltf_pbr): alpha_t = mix(a, 1, anisotropy^2), alpha_b = a.
        at = mix(a, 1.0, anisotropy * anisotropy);
        ab = max(a, 1e-4);
    } else if (mapping == 1) {
        // OpenPBR: alpha_t = a sqrt(2 / (1 + (1 - anisotropy)^2)), alpha_b = (1 - anisotropy) alpha_t.
        float k = 1.0 - anisotropy;
        at = a * sqrt(2.0 / (1.0 + k * k));
        ab = max(k * at, 1e-4);
    } else {
        // standard_surface (mx_roughness_anisotropy).
        float aspect = sqrt(1.0 - min(anisotropy, 0.98));
        at = min(a / aspect, 1.0);
        ab = a * aspect;
    }
    return vec2(at, ab);
}

#if defined(RAY_TRACING) || defined(UV_FOOTPRINT)
// Ray tracing and vertex stages have no derivatives: hit shaders set the
// ray cone's footprint in uv here (rt.glsl), the vertex stage the mesh's
// uv spacing (displace.glsl).
float rtUvFootprint = 0.0;
#define UV_DX(uv) vec2(rtUvFootprint, 0.0)
#define UV_DY(uv) vec2(0.0, rtUvFootprint)
#else
#define UV_DX(uv) dFdx(uv)
#define UV_DY(uv) dFdy(uv)
#endif

// With uv's gradients given (dx, dy: per pixel, before the texture's own
// transform).
vec4 FetchGrad(TexRef r, vec4 constant, vec2 uv, vec2 dx, vec2 dy)
{
    if (r.index < 0) {
        return constant;
    }
    int index = r.index;
    // UsdTransform2d / MaterialX place2d; gradients through the same matrix.
    mat2 a = mat2(r.uvRow0.x, r.uvRow1.x, r.uvRow0.y, r.uvRow1.y);
    dx = a * dx;
    dy = a * dy;
    uv = a * uv + vec2(r.uvRow0.z, r.uvRow1.z);
    vec2 tileUv = uv;
    if (r.pad != 0 && (((r.pad & 1) != 0 && (uv.x < 0.0 || uv.x > 1.0)) ||
                       ((r.pad & 2) != 0 && (uv.y < 0.0 || uv.y > 1.0)))) {
        return constant;
    }
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

vec4 Fetch(TexRef r, vec4 constant, vec2 uv)
{
    return FetchGrad(r, constant, uv, UV_DX(uv), UV_DY(uv));
}

// FetchGrad, but where the texture is magnified (a texel wider than the
// footprint) a cubic B-spline over its texels (four bilinear taps, Sigg and
// Hadwiger 2005): heights between texels stay smooth, so finely diced
// displacement and its bump show no texel facets. UDIMs read as FetchGrad.
// ponytail: ray tracing stages read bilinear (glslc's optimizer crashed on
// rt_pixels.rgen with it): a traced hit's displaced triangle (rt_evaluate)
// sits up to a texel's smoothing off the BLAS's (rt_displace.comp, smooth).
vec4 FetchSmooth(TexRef r, vec4 constant, vec2 uv, vec2 dx, vec2 dy)
{
#ifdef RAY_TRACING
    return FetchGrad(r, constant, uv, dx, dy);
#endif
    if (r.index < 0 || r.udim != 0) {
        return FetchGrad(r, constant, uv, dx, dy);
    }
    mat2 a = mat2(r.uvRow0.x, r.uvRow1.x, r.uvRow0.y, r.uvRow1.y);
    vec2 size = vec2(textureSize(textures[nonuniformEXT(r.index)], 0));
    vec2 tdx = a * dx * size, tdy = a * dy * size;
    if (max(dot(tdx, tdx), dot(tdy, tdy)) >= 1.0 || abs(determinant(a)) < 1e-12) {
        return FetchGrad(r, constant, uv, dx, dy);
    }
    vec2 offset = vec2(r.uvRow0.z, r.uvRow1.z);
    vec2 t = (a * uv + offset) * size - 0.5, i = floor(t), f = t - i;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 p0 = (i + 0.5 - 1.0 + w1 / g0) / size, p1 = (i + 0.5 + 1.0 + w3 / g1) / size;  // texture space
    mat2 back = inverse(a);
    // One FetchGrad in a loop: four inlined copies overran glslc's ids in rt_pixels.rgen.
    vec4 sum = vec4(0.0);
    for (int k = 0; k < 4; ++k) {
        bvec2 hi = bvec2((k & 1) != 0, (k & 2) != 0);
        vec2 at = mix(p0, p1, hi), w = mix(g0, g1, hi);
        sum += w.x * w.y * FetchGrad(r, constant, back * (at - offset), vec2(0.0), vec2(0.0));
    }
    return sum;
}

// What a translucent shadow caster lets through (shadow_tint.frag's tint
// maps, rt_shadow.rahit's traced shadows): what transmits through a
// refracting surface loses its two interfaces' reflections at normal
// incidence. With a transmission depth the colour absorbs over `path`,
// the light's way inside from this surface (scene.glsl CasterPath:
// Beer-Lambert, the colour at the depth's distance); path < 0 (not a ball
// or box) or no depth: each surface tints by the colour.
// ponytail: constants and textures only (material programs are skipped);
// other closed meshes tint per surface.
vec3 ShadowTransmission(Material m, vec2 uv, float path)
{
    float opacity = clamp(Fetch(m.tex[6], m.params.wwww, uv).r, 0.0, 1.0);
    float f0 = (m.refraction.x - 1.0) / (m.refraction.x + 1.0);
    float interfaces = (1.0 - f0 * f0) * (1.0 - f0 * f0);
    vec3 tint = m.transmission.rgb;
    if (path >= 0.0 && m.refraction.z > 0.0 && m.refraction.y <= 0.0) {
        tint = pow(max(tint, vec3(1e-4)), vec3(path / m.refraction.z));
    }
    vec3 transmitted = opacity * m.transmission.w * tint * interfaces;
    // A thin-walled subsurface surface (a leaf) lets its subsurface colour
    // through diffusely: kThinTranslucency of it reaches what lies in its
    // shadow (measured in Karma: a back-lit leaf's shadow gains about 5% of
    // the sun times its colour, the rest scatters away).
    // ponytail: one constant for the re-emitted light's spread; a receiver
    // right behind the leaf gets more of it, a far one less.
    const float kThinTranslucency = 0.05;
    if (m.refraction.y > 0.0) {
        transmitted += opacity * m.subsurface.w * m.subsurface.rgb * kThinTranslucency;
    }
    return m.refraction.w > 0.0 ? vec3((1.0 - opacity) * interfaces) : vec3(1.0 - opacity) + transmitted;
}

// False where a cutout material has no surface (opacity under its threshold).
// The opacity is read at the finest mip, as the traced passes read it: a
// minified cutout's mips average its holes in (foliage cards a few pixels
// wide fell under the threshold and vanished); the film's samples
// antialias the edges instead.
bool Present(Material m, vec2 uv)
{
    return m.emissive.w <= 0.0 || FetchGrad(m.tex[6], m.params.wwww, uv, vec2(0.0), vec2(0.0)).r >= m.emissive.w;
}
