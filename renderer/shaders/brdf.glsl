// The base's specular and diffuse by the material's surface model
// (SetupBase, every build), and the lobes on top of it from the material's
// constants (material.glsl): a clearcoat, sheen and fuzz, thin-film
// iridescence, anisotropy, hair (curves) and toon bands. mesh.frag includes
// it after its inputs and calls SetupBase, then
// PrepareLayers once per pixel (before the lights), LayerLight per light
// (and TangentSpecular for the base's GGX), BentNormal and LayerAmbient
// for the dome, ToonShade last. Only materials with lobes
// (vkr::Material::Layered) shade with the LAYERS build of mesh.frag; the
// plain build gets the empty versions at the end, so plain materials pay
// nothing. Within LAYERS each lobe costs a branch unless the material uses
// it and its FrameData.features.x flag is on.

// frame.features.x bits (renderer.cpp's Frame switches).
const uint kFeatureSpecularWorkflow = 8u, kFeatureClearcoat = 16u, kFeatureSheen = 32u, kFeatureThinFilm = 64u,
           kFeatureAnisotropy = 128u;
const uint kFeatureToon = 512u;  // toon bands for every material (frame.styles.y), an optional feature
// frame.styles.x bits.
const uint kStyleToon = 1u, kStyleHair = 2u, kStyleTexturedLobes = 4u, kStyleTexturedTransmission = 8u;

// The pixel's material (inMaterial): its constants, with the lobe inputs a
// material program computed (textures, graphs; program.glsl's programLobe
// in MaterialProgram::Input order from kSpecularColor) in their place when
// the frame's Textured Lobe (and Transmission) Inputs are on. Only the GRAPH + LAYERS shaders
// have them (program.glsl's ProgramMaterial). Call after RunProgram.
Material ShadingMaterial()
{
    Material m = materials[inMaterial];
#if defined(GRAPH) && defined(LAYERS)
    m = ProgramMaterial(m);
#endif
    return m;
}

// The base's specular and diffuse as the surface model's spec defines them
// (MaterialX's nodegraphs and pbrlib, libraries/bxdf): a metal and a
// dielectric GGX lobe, mixed by metallic, with the diffuse beneath the
// dielectric by albedo scaling (MaterialX's layer: what the dielectric
// reflects towards the viewer, its directional albedo, is taken from the
// diffuse). SetupBase fills it once per pixel; PrepareLayers (coat, sheen,
// fuzz, thin film) changes it; mesh.frag's Specular and BaseAlbedo
// evaluate it.
struct Base {
    vec3 metalF0, metalF90, metalA;  // metal Fresnel: Schlick F0..F90 minus Hoffman's a mu (1 - mu)^6 (F82)
    vec3 metalW;                     // its weight: metallic (and what the layers above leave)
    vec3 dielF0;                     // dielectric Fresnel: F0 (F90 white)
    float dielEta;                   // > 0: the exact Fresnel of this relative index, else Schlick's
    vec3 dielW;                      // its weight times tint: 1 - metallic, specular colour, layers above
    float msK;                       // multiple scattering (Turquin 2019): (1 - Ess) / Ess, 0: single
    uint model;                      // vkr::Material::Model
    uint diffuse;                    // 0 Lambert, 1 Oren-Nayar (standard_surface), 2 energy-compensated (OpenPBR)
    float diffuseRoughness;
    float dielThroughput;            // what passes the dielectric lobe to the diffuse beneath (towards the viewer)
    vec3 albedo;                     // the diffuse colour before the lobes (OpenPBR's coat darkening)
    float metallic;
    float specularWeight;
};
Base base;
// The coat's share of the last LayerLight (times the light's specular
// energy as the rest) and of LayerAmbient: the light splits' coat.
float gCoatLight = 0.0;
vec3 gCoatAmbient = vec3(0.0);

// Unpolarized Fresnel reflectance of a dielectric of relative index eta at
// cos theta c (MaterialX's mx_fresnel_dielectric); total internal
// reflection gives 1.
float FresnelDielectric(float c, float eta)
{
    float g2 = eta * eta + c * c - 1.0;
    if (g2 < 0.0) {
        return 1.0;
    }
    float g = sqrt(g2);
    float a = (g - c) / (g + c), b = ((g + c) * c - 1.0) / ((g - c) * c + 1.0);
    return 0.5 * a * a * (1.0 + b * b);
}

// Unpolarized Fresnel reflectance of a conductor (n, k) at cos theta c
// (mx_fresnel_conductor).
vec3 FresnelConductor(float c, vec3 n, vec3 k)
{
    float c2 = c * c, s2 = 1.0 - c2;
    vec3 n2 = n * n, k2 = k * k;
    vec3 t0 = n2 - k2 - s2;
    vec3 ab = sqrt(t0 * t0 + 4.0 * n2 * k2);
    vec3 t1 = ab + c2;
    vec3 t2 = 2.0 * sqrt(max(0.5 * (ab + t0), 0.0)) * c;
    vec3 rs = (t1 - t2) / (t1 + t2);
    vec3 t3 = c2 * ab + s2 * s2, t4 = t2 * s2;
    return 0.5 * (rs + rs * (t3 - t4) / (t3 + t4));
}

// Hoffman's a for F0, F90 and the colour at cos 1/7 (82 degrees) relative
// to Schlick's there (mx_fresnel_hoffman_schlick).
const float kCos82 = 1.0 / 7.0;
vec3 HoffmanA(vec3 f0, vec3 f90, vec3 f82Tint)
{
    return mix(f0, f90, pow(1.0 - kCos82, 5.0)) * (1.0 - f82Tint) / (kCos82 * pow(1.0 - kCos82, 6.0));
}

vec3 MetalFresnel(float mu)
{
    float m = 1.0 - mu, m5 = m * m * m * m * m;
    return mix(base.metalF0, base.metalF90, m5) - base.metalA * (mu * m5 * m);
}

vec3 DielectricFresnel(float mu)
{
    if (base.dielEta > 0.0) {
        return vec3(FresnelDielectric(mu, base.dielEta));
    }
    float m = 1.0 - mu;
    return base.dielF0 + (1.0 - base.dielF0) * (m * m * m * m * m);
}

// The base's Fresnel at cos mu between the light (or view) and the half
// vector, with each lobe's multiple-scattering compensation 1 + F msK
// (MaterialX's mx_ggx_energy_compensation).
vec3 BaseFresnel(float mu)
{
    vec3 fm = MetalFresnel(mu), fd = DielectricFresnel(mu);
    return base.metalW * fm * (1.0 + fm * base.msK) + base.dielW * fd * (1.0 + fd * base.msK);
}

// The dielectric's and the whole base's directional albedo towards the
// viewer (split sum: dfg = brdfLut's A, B, C at NoV and the roughness;
// for the dielectric Schlick's albedo at its F0, as MaterialX).
vec3 DielectricAlbedo(float NoV, vec3 dfg)
{
    return (base.dielF0 * dfg.x + dfg.y) * (1.0 + DielectricFresnel(NoV) * base.msK);
}

vec3 BaseAlbedo(float NoV, vec3 dfg)
{
    vec3 em = (base.metalF0 * dfg.x + base.metalF90 * dfg.y - base.metalA * dfg.z) *
              (1.0 + MetalFresnel(NoV) * base.msK);
    return base.metalW * max(em, 0.0) + base.dielW * DielectricAlbedo(NoV, dfg);
}

// The diffuse lobe as a colour to multiply by NoL / pi: Lambert,
// standard_surface's Oren-Nayar (MaterialX's qualitative form,
// mx_oren_nayar_diffuse) or OpenPBR's energy-compensated Oren-Nayar
// (Fujii's, mx_oren_nayar_compensated_diffuse).
const float kFujii1 = 0.5 - 2.0 / (3.0 * 3.14159265), kFujii2 = 2.0 / 3.0 - 28.0 / (15.0 * 3.14159265);

float FujiiAlbedo(float c, float r)
{
    float A = 1.0 / (1.0 + kFujii1 * r), s = sqrt(max(1.0 - c * c, 0.0));
    float G = s * (acos(clamp(c, -1.0, 1.0)) - s * c) + 2.0 * ((s / max(c, 1e-4)) * (1.0 - s * s * s) - s) / 3.0;
    return A + r * A * G * 0.31830989;
}

vec3 FujiiMultiple(vec3 color, float r)
{
    float avg = (1.0 + kFujii2 * r) / (1.0 + kFujii1 * r);
    return color * color * avg / (1.0 - color * max(1.0 - avg, 0.0));
}

vec3 DiffuseLobe(vec3 color, float NoL, float NoV, float LoV)
{
    if (base.diffuse == 0u) {
        return color;
    }
    float r = base.diffuseRoughness, s = LoV - NoL * NoV;
    if (base.diffuse == 1u) {
        float s2 = r * r;
        return color * (1.0 - 0.5 * s2 / (s2 + 0.33) + 0.45 * s2 / (s2 + 0.09) * (s > 0.0 ? s / max(NoL, NoV) : 0.0));
    }
    float A = 1.0 / (1.0 + kFujii1 * r), avg = A * (1.0 + kFujii2 * r);
    return color * A * (1.0 + r * (s > 0.0 ? s / max(NoL, NoV) : s)) +
           FujiiMultiple(color, r) * max(1.0 - FujiiAlbedo(NoV, r), 1e-6) * max(1.0 - FujiiAlbedo(NoL, r), 1e-6) /
               max(1.0 - avg, 1e-6);
}

// Its directional albedo towards the viewer (for the dome's light).
vec3 DiffuseAlbedo(vec3 color, float NoV)
{
    if (base.diffuse == 0u) {
        return color;
    }
    float r = base.diffuseRoughness;
    if (base.diffuse == 1u) {
        vec2 q = vec2(1.0) + vec2(-0.4297, -0.6076) * r + vec2(-0.7632, -0.4993) * NoV * r +
                 vec2(1.4385, 2.0315) * r * r;  // mx_oren_nayar_diffuse_dir_albedo_analytic
        return color * clamp(q.x / q.y, 0.0, 1.0);
    }
    return mix(FujiiMultiple(color, r), color, FujiiAlbedo(NoV, r));
}

// Before the lobes: the base of the pixel's material by its surface model
// (UsdPreviewSurface for none), returning its diffuse colour. `response`
// scales the specular lobes (the subsurface pass's 0); dfg: brdfLut at NoV
// and the base's roughness.
// UsdPreviewSurface (its spec): Schlick from F0 = ((1 - ior) / (1 + ior))^2
// to white, metals from albedo to albedo, the specular workflow from
// specularColor to white; single scattering, as Storm and Karma.
// standard_surface: an exact dielectric of specular_IOR weighted by
// specular, tinted by specular_color; the metal is MaterialX's conductor of
// artistic_ior(base_color, specular_color) in Hoffman's F82 form (exact at
// 0, 82 and 90 degrees); multiple scattering.
// OpenPBR: the dielectric's F0 scaled by specular_weight through its index
// (relative to the coat's under a coat), tinted by specular_color; the
// metal is F82-tint Schlick (specular_color) times specular_weight;
// multiple scattering.
vec3 SetupBase(vec3 baseColor, float metallic, vec3 response, float NoV, vec3 dfg)
{
    base.model = 0u;
    base.diffuse = 0u;
    base.diffuseRoughness = 0.0;
    vec3 specColor = vec3(1.0), specularF0 = vec3(0.0);
    float specWeight = 1.0, ior = 1.5, coat = 0.0, coatIor = 1.5, affect = 0.0;
    bool specularWorkflow = false;
    if (inMaterial != kNoMaterial) {
        Material m = ShadingMaterial();
        base.model = uint(m.surface.x + 0.5);
        base.diffuseRoughness = clamp(m.surface.y, 0.0, 1.0);
        specColor = max(m.specular.rgb, 0.0);
        specWeight = max(m.specular.w, 0.0);
        ior = m.refraction.x;
        specularWorkflow = m.specularColor.w > 0.0 && (frame.features.x & kFeatureSpecularWorkflow) != 0u;
        specularF0 = max(m.specularColor.rgb, 0.0);
        if (m.coat.x > 0.0 && (frame.features.x & kFeatureClearcoat) != 0u) {
            coat = clamp(m.coat.x, 0.0, 1.0);
            float s = sqrt(clamp(m.coat.z, 0.0, 0.99));
            coatIor = (1.0 + s) / (1.0 - s);
            affect = m.surface.z;
        }
    }
    bool multi = (frame.features.x & kFeatureMultiScatter) != 0u && base.model != 0u;
    float Ess = dfg.x + dfg.y;
    base.msK = multi ? (1.0 - Ess) / max(Ess, 1e-4) : 0.0;
    base.metalF90 = vec3(1.0);
    base.metalA = vec3(0.0);
    base.dielEta = 0.0;
    float throughputWeight = 1.0;
    vec3 diffuse = baseColor;
    if (base.model == 0u) {
        float r = (ior - 1.0) / (ior + 1.0);
        base.dielF0 = specularWorkflow ? specularF0 : vec3(r * r);
        metallic = specularWorkflow ? 0.0 : metallic;
        base.metalF0 = baseColor;
        base.metalF90 = baseColor;
        base.metalW = vec3(metallic);
        base.dielW = vec3(1.0 - metallic);
    } else if (base.model == 1u) {
        base.diffuse = base.diffuseRoughness > 0.0 ? 1u : 0u;
        float r = (ior - 1.0) / (ior + 1.0);
        base.dielEta = ior;
        base.dielF0 = vec3(r * r);
        base.dielW = (1.0 - metallic) * specWeight * specColor;
        throughputWeight = specWeight;
        // artistic_ior (Gulbrandsen 2014), and its conductor's Fresnel at 82 degrees.
        vec3 refl = clamp(baseColor, 0.0, 0.99), edge = specWeight * specColor, rs = sqrt(refl);
        vec3 n = mix((1.0 + rs) / (1.0 - rs), (1.0 - refl) / (1.0 + refl), edge);
        vec3 k = sqrt(max(((n + 1.0) * (n + 1.0) * refl - (n - 1.0) * (n - 1.0)) / (1.0 - refl), 0.0));
        base.metalF0 = refl;
        vec3 schlick82 = mix(refl, vec3(1.0), pow(1.0 - kCos82, 5.0));
        base.metalA = HoffmanA(refl, vec3(1.0), FresnelConductor(kCos82, n, k) / schlick82);
        base.metalW = vec3(metallic);
        diffuse = pow(max(baseColor, 0.0), vec3(1.0 + coat * affect));  // coat_affect_color
    } else {
        base.diffuse = base.diffuseRoughness > 0.0 ? 2u : 0u;
        // Under the coat the dielectric's index is relative to the coat's
        // (inverted where under 1, OpenPBR's TIR fix), mixed by coat weight.
        float ratio = ior / coatIor;
        float eta = mix(ior, ratio > 1.0 ? ratio : 1.0 / max(ratio, 1e-4), coat);
        float r = (eta - 1.0) / (eta + 1.0);
        float f0 = clamp(specWeight * r * r, 0.0, 0.99999);
        float e = sign(eta - 1.0) * sqrt(f0);
        base.dielEta = (1.0 + e) / (1.0 - e);
        base.dielF0 = vec3(f0);
        base.dielW = (1.0 - metallic) * specColor;
        base.metalF0 = baseColor;
        base.metalA = HoffmanA(baseColor, vec3(1.0), specColor);
        base.metalW = vec3(metallic * specWeight);
    }
    base.dielThroughput = 1.0 - throughputWeight * dot(DielectricAlbedo(NoV, dfg), vec3(1.0 / 3.0));
    base.metalW *= response;
    base.dielW *= response;
    base.albedo = baseColor;
    base.metallic = metallic;
    base.specularWeight = specWeight;
    return diffuse * (1.0 - metallic) * base.dielThroughput;
}

// The base's colour for toon bands (ToonShade): the diffuse colour before
// the specular takes its share (which varies over the surface) plus F0.
vec3 BaseToonAlbedo()
{
    return base.albedo * (1.0 - base.metallic) + base.metalW * base.metalF0 + base.dielW * base.dielF0;
}

#ifdef LAYERS

// mesh.frag's GGX pieces, shared by the lobes.
mat3 CotangentFrame(vec3 n, vec3 p, vec2 uv);
float D_GGX(float NoH, float a);
float V_SmithGGXCorrelated(float NoV, float NoL, float a);
vec3 F_Schlick(vec3 f0, float VoH);

// The pixel's lobes on top of the base (PrepareLayers).
struct Layers {
    float coat;            // weight; 0: no coat
    float coatPerceptual;  // roughness, clamped as the base's
    float coatF0;
    float coatAlbedo;      // what the coat reflects towards the viewer (split-sum DFG)
    float coatEta;         // > 0: its exact Fresnel (standard_surface, OpenPBR), else Schlick's from coatF0
    float coatMsK;         // its multiple scattering, as Base::msK
    vec3 sheen;            // colour times weight; 0: no sheen
    float sheenRoughness;
    float sheenAlbedo;     // directional albedo towards the viewer
    bool fuzz;             // OpenPBR's fuzz (Zeltner's LTC sheen, over the coat), else Imageworks' sheen
    vec3 under;            // what the layers above (coat, fuzz) let through to the base: its transmission too
    float anisotropy;      // 0: isotropic base specular
    bool openPbrAnisotropy;  // OpenPBR's roughness mapping, else standard_surface's
    vec3 tangent;          // the stretch direction, with the bitangent across it
    vec3 bitangent;
    float toonBands;       // the material's own (0: the frame's)
    float toonSoftness;
    vec3 albedo;           // the base's, before the lobes change it (toon bands)
    bool hair;             // strands along the bitangent (curves)
    vec3 hairTint;         // the hair's colour: the diffuse colour
};
Layers layers;

// Sheen as MaterialX's sheen_bsdf (standard_surface): the Imageworks
// microfiber distribution (Estevez & Kulla 2017) with Neubelt's smooth
// visibility term, and its directional albedo by MaterialX's rational fit
// (mx_microfacet_sheen.glsl).
float D_Sheen(float NoH, float roughness)
{
    float inv = 1.0 / roughness;
    return (2.0 + inv) * pow(max(1.0 - NoH * NoH, 1e-7), inv * 0.5) / (2.0 * PI);
}

float SheenAlbedo(float NoV, float roughness)
{
    vec2 r = vec2(13.67300, 1.0) + vec2(-68.78018, 61.57746) * NoV + vec2(799.08825, 442.78211) * roughness +
             vec2(-905.00061, 2597.49308) * NoV * roughness + vec2(60.28956, 121.81241) * NoV * NoV +
             vec2(1086.96473, 3045.55075) * roughness * roughness;
    return clamp(r.x / r.y, 0.0, 1.0);
}

// OpenPBR's fuzz: Zeltner et al. 2022's LTC sheen (MaterialX's
// mx_zeltner_sheen_*): its directional albedo (a Gaussian fit), and the
// lobe for light from l normalized to 1 (times cos).
float ZeltnerAlbedo(float x, float y)
{
    float s = y * (0.0206607 + 1.58491 * y) / (0.0379424 + y * (1.32227 + y));
    float m = y * (-0.193854 + y * (-1.14885 + y * (1.7932 - 0.95943 * y * y))) / (0.046391 + y);
    float o = y * (0.000654023 + (-0.0207818 + 0.119681 * y) * y) / (1.26264 + y * (-1.92021 + y));
    return exp(-0.5 * (x - m) * (x - m) / (s * s)) / (s * sqrt(2.0 * PI)) + o;
}

float ZeltnerLobe(vec3 n, vec3 v, vec3 l, float NoV, float r)
{
    vec3 X = v - n * NoV;
    X = dot(X, X) > 1e-12 ? normalize(X) : (abs(n.z) < 0.999 ? normalize(cross(n, vec3(0.0, 0.0, 1.0)))
                                                              : vec3(1.0, 0.0, 0.0));
    vec3 Y = cross(n, X);
    vec3 w = vec3(dot(l, X), dot(l, Y), dot(l, n));
    float aInv = (2.58126 * NoV + 0.813703 * r) * r / (1.0 + 0.310327 * NoV * NoV + 2.60994 * NoV * r);
    float bInv = sqrt(1.0 - NoV) * (r - 1.0) * r * r * r /
                 (0.0000254053 + 1.71228 * NoV - 1.71506 * NoV * r + 1.34174 * r * r);
    vec3 wo = vec3(aInv * w.x + bInv * w.z, aInv * w.y, w.z);
    float len2 = dot(wo, wo);
    return max(wo.z, 0.0) / PI * (aInv / len2) * (aInv / len2);
}

// The coat's Fresnel: exact (standard_surface's and OpenPBR's
// dielectric_bsdf), or Schlick's (UsdPreviewSurface's generalized_schlick).
float CoatFresnel(float mu)
{
    if (layers.coatEta > 0.0) {
        return FresnelDielectric(mu, layers.coatEta);
    }
    float m = 1.0 - mu;
    return layers.coatF0 + (1.0 - layers.coatF0) * (m * m * m * m * m);
}

// Thin-film iridescence (Belcour & Barla 2017, without polarization, as
// glTF's KHR_materials_iridescence): the Airy reflectance of a film of
// index `eta2` and `thickness` nm between air and a base of F0 `f0`, at
// cosTheta1 from the normal. The phase terms are integrated against the
// CIE sensitivities in closed form (their Gaussian fit), then taken from
// XYZ to linear Rec.709.
vec3 FilmSensitivity(float opd, vec3 shift)
{
    float phase = 2.0 * PI * opd * 1.0e-9;
    const vec3 val = vec3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    const vec3 pos = vec3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    const vec3 var = vec3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    vec3 xyz = val * sqrt(2.0 * PI * var) * cos(pos * phase + shift) * exp(-phase * phase * var);
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09) * cos(2.2399e+06 * phase + shift.x) *
             exp(-4.5282e+09 * phase * phase);
    const mat3 xyzToRec709 = mat3(3.2404542, -0.9692660, 0.0556434, -1.5371385, 1.8760108, -0.2040259, -0.4985314,
                                  0.0415560, 1.0572252);
    return xyzToRec709 * (xyz / 1.0685e-7);
}

vec3 Iridescence(float cosTheta1, float thickness, float eta2, vec3 f0)
{
    eta2 = mix(1.0, eta2, smoothstep(0.0, 30.0, thickness));  // thinner films fade to the bare base
    float cosTheta2 = sqrt(max(1.0 - (1.0 - cosTheta1 * cosTheta1) / (eta2 * eta2), 0.0));
    // Air to film, then film to the base (its index from F0).
    float r12 = (eta2 - 1.0) / (eta2 + 1.0);
    float R12 = F_Schlick(vec3(r12 * r12), cosTheta1).x;
    float T121 = 1.0 - R12;
    vec3 s = sqrt(clamp(f0, 0.0, 0.9999));
    vec3 eta3 = (1.0 + s) / (1.0 - s);
    vec3 r23 = (eta3 - eta2) / (eta3 + eta2);
    vec3 R23 = F_Schlick(r23 * r23, cosTheta2);
    // Reflections off a denser medium shift the phase by pi.
    vec3 phi = (eta2 < 1.0 ? 0.0 : PI) + mix(vec3(0.0), vec3(PI), lessThan(eta3, vec3(eta2)));
    float opd = 2.0 * eta2 * thickness * cosTheta2;
    vec3 R123 = clamp(R12 * R23, 1e-5, 0.9999);
    vec3 r123 = sqrt(R123);
    vec3 Rs = T121 * T121 * R23 / (1.0 - R123);
    vec3 I = R12 + Rs;
    vec3 Cm = Rs - T121;
    for (int m = 1; m <= 2; ++m) {
        Cm *= r123;
        I += Cm * 2.0 * FilmSensitivity(float(m) * opd, float(m) * phi);
    }
    return max(I, vec3(0.0));
}

// The uv set's u direction on the surface (from screen-space derivatives,
// the frame normal maps use), unit length; without uvs around the world's
// up axis. Call in uniform control flow.
vec3 UvTangent(vec3 n)
{
    vec3 t = CotangentFrame(n, inWorldPos, inUv)[0];
    t -= n * dot(n, t);
    if (dot(t, t) < 1e-12) {
        t = cross(abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n);
    }
    return normalize(t);
}

// The base's specular roughness under a coat: standard_surface's
// coat_affect_roughness (towards 1 by coat times it times coat_roughness),
// OpenPBR's roughening (min(1, 2 coat_roughness^4 + roughness^4)^(1/4),
// mixed by coat weight). Before SetupBase.
void CoatRoughening(inout float roughness)
{
    if (inMaterial == kNoMaterial) {
        return;
    }
    Material m = ShadingMaterial();
    if (m.coat.x <= 0.0 || (frame.features.x & kFeatureClearcoat) == 0u) {
        return;
    }
    float coat = clamp(m.coat.x, 0.0, 1.0), coatRoughness = clamp(m.coat.y, 0.0, 1.0);
    if (m.surface.x > 1.5) {
        float c2 = coatRoughness * coatRoughness, r2 = roughness * roughness;
        roughness = mix(roughness, pow(min(1.0, 2.0 * c2 * c2 + r2 * r2), 0.25), coat);
    } else if (m.surface.x > 0.5) {
        roughness = mix(roughness, 1.0, coat * m.surface.w * coatRoughness);
    }
}

// Before the lights: the pixel's lobes, and the base (SetupBase) and its
// diffuse colour and emission as they change them.
void PrepareLayers(vec3 n, float NoV, inout vec3 diffuseColor, inout vec3 emissive)
{
    layers = Layers(0.0, 1.0, 0.0, 0.0, 0.0, 0.0, vec3(0.0), 1.0, 0.0, false, vec3(1.0), 0.0, false, vec3(0.0),
                    vec3(0.0), 0.0, 0.0, BaseToonAlbedo(), false, diffuseColor);
    // Hair (curves, whose uvs run along each curve, unless their
    // primvars:eldr:hair is false; hydra/curves.cpp): strands along the
    // curve, highlights stretched around it, also without a material.
    if (draws[instances[inSlot].draw].hair != 0u && (frame.styles.x & kStyleHair) != 0u) {
        vec3 t = UvTangent(n);
        layers.hair = true;
        layers.anisotropy = 1.0;  // BentNormal: reflections around the strand
        layers.tangent = cross(t, n);
        layers.bitangent = t;
    }
    if (inMaterial == kNoMaterial) {
        return;
    }
    Material m = ShadingMaterial();
    layers.toonBands = m.toon.x;
    layers.toonSoftness = m.toon.y;
    // A thin film on the base's specular (dielectric and metal): its
    // reflectance towards the viewer stands in for F0 (Schlick's from it).
    // ponytail: the film sees air above it, also under a coat; MaterialX
    // evaluates Airy per light with polarization.
    if (m.thinFilm.x > 0.0 && (frame.features.x & kFeatureThinFilm) != 0u) {
        base.dielF0 = mix(base.dielF0, Iridescence(NoV, m.thinFilm.y, m.thinFilm.z, base.dielF0), m.thinFilm.x);
        base.dielEta = 0.0;
        base.metalF0 = mix(base.metalF0, Iridescence(NoV, m.thinFilm.y, m.thinFilm.z, base.metalF0), m.thinFilm.x);
    }
    // standard_surface's sheen layer (Imageworks): over the diffuse, under
    // the dielectric's specular (so through its throughput, and only where
    // not metal); the diffuse loses the sheen's albedo.
    if (m.sheen.x > 0.0 && (frame.features.x & kFeatureSheen) != 0u && base.model != 2u) {
        layers.sheen = m.sheenColor.rgb * m.sheen.x * (1.0 - base.metallic) * base.dielThroughput;
        layers.sheenRoughness = clamp(m.sheen.y, 0.005, 1.0);
        layers.sheenAlbedo = SheenAlbedo(NoV, m.sheen.y);
        diffuseColor *= 1.0 - m.sheen.x * layers.sheenAlbedo;
    }
    // The coat: everything beneath is tinted by the coat colour and loses
    // the coat's directional albedo (MaterialX's layer); OpenPBR's base also
    // darkens by the light the coat reflects back into it (coat_darkening).
    // Emission under it (standard_surface, OpenPBR; UsdPreviewSurface's
    // isn't layered) is tinted and loses the coat's Fresnel towards the
    // viewer.
    bool multi = (frame.features.x & kFeatureMultiScatter) != 0u && base.model != 0u;
    if (m.coat.x > 0.0 && (frame.features.x & kFeatureClearcoat) != 0u) {
        layers.coat = clamp(m.coat.x, 0.0, 1.0);
        layers.coatPerceptual = clamp(m.coat.y, 0.045, 1.0);
        layers.coatF0 = clamp(m.coat.z, 0.0, 0.99);
        float s = sqrt(layers.coatF0), coatIor = (1.0 + s) / (1.0 - s);
        layers.coatEta = base.model != 0u ? coatIor : 0.0;
        vec2 ab = texture(brdfLut, vec2(NoV, layers.coatPerceptual)).rg;
        layers.coatMsK = multi ? (1.0 - ab.x - ab.y) / max(ab.x + ab.y, 1e-4) : 0.0;
        layers.coatAlbedo = (layers.coatF0 * ab.x + ab.y) * (1.0 + CoatFresnel(NoV) * layers.coatMsK);
        vec3 under = mix(vec3(1.0), m.coatColor.rgb, layers.coat) * (1.0 - layers.coat * layers.coatAlbedo);
        if (base.model == 2u) {
            float K = 1.0 - (1.0 - layers.coatF0) / (coatIor * coatIor);
            vec3 Ebase = base.albedo * mix(1.0, base.specularWeight, base.metallic);
            under *= mix(vec3(1.0), (1.0 - K) / (1.0 - Ebase * K), layers.coat * m.surface.z);
        }
        diffuseColor *= under;
        layers.under *= under;
        base.metalW *= under;
        base.dielW *= under;
        layers.sheen *= under;
        if (base.model != 0u) {
            float c = 1.0 - NoV;
            emissive *= mix(vec3(1.0), m.coatColor.rgb * (1.0 - layers.coatF0) * (1.0 - c * c * c * c * c),
                            layers.coat);
        }
    }
    // OpenPBR's fuzz (Zeltner's LTC sheen) over everything, the coat too.
    if (m.sheen.x > 0.0 && (frame.features.x & kFeatureSheen) != 0u && base.model == 2u) {
        layers.fuzz = true;
        layers.sheen = m.sheenColor.rgb * m.sheen.x;
        layers.sheenRoughness = clamp(m.sheen.y, 0.01, 1.0);
        layers.sheenAlbedo = ZeltnerAlbedo(NoV, layers.sheenRoughness);
        float through = 1.0 - m.sheen.x * layers.sheenAlbedo;
        diffuseColor *= through;
        layers.under *= through;
        base.metalW *= through;
        base.dielW *= through;
        layers.coat *= through;
    }
    // Anisotropic specular: stretched along the uv set's u direction (as
    // MaterialX's default tangent), rotated about the normal; without uvs
    // around the world's up axis.
    // ponytail: no tangent primvars; a "tangents" primvar would replace the
    // derivative frame.
    if (m.anisotropy.x > 0.0 && (frame.features.x & kFeatureAnisotropy) != 0u && !layers.hair) {
        layers.anisotropy = clamp(m.anisotropy.x, 0.0, 1.0);
        layers.openPbrAnisotropy = m.anisotropy.z > 0.0;
        vec3 t = UvTangent(n);
        float angle = 2.0 * PI * m.anisotropy.y;
        layers.tangent = cos(angle) * t + sin(angle) * cross(n, t);
        layers.bitangent = cross(n, layers.tangent);
    }
}

// The base's GGX with roughness `a` stretched along the tangent (Heitz
// 2014's anisotropic distribution and height-correlated visibility, in
// Filament's form), times cos, for light from direction l.
vec3 AnisotropicSpecular(vec3 n, vec3 v, vec3 l, float a, vec3 f0, float NoV)
{
    float at, ab;
    if (layers.openPbrAnisotropy) {
        // OpenPBR: alpha_t = a sqrt(2 / (1 + (1 - anisotropy)^2)), alpha_b = (1 - anisotropy) alpha_t.
        float k = 1.0 - layers.anisotropy;
        at = a * sqrt(2.0 / (1.0 + k * k));
        ab = max(k * at, 1e-4);
    } else {
        // standard_surface (mx_roughness_anisotropy).
        float aspect = sqrt(1.0 - min(layers.anisotropy, 0.98));
        at = min(a / aspect, 1.0);
        ab = a * aspect;
    }
    vec3 t = layers.tangent, b = layers.bitangent, h = normalize(v + l);
    float NoL = clamp(dot(n, l), 0.0, 1.0);
    float a2 = at * ab;
    vec3 d = vec3(ab * dot(t, h), at * dot(b, h), a2 * max(dot(n, h), 0.0));
    float b2 = a2 / dot(d, d);
    float D = a2 * b2 * b2 / PI;
    float lambdaV = NoL * length(vec3(at * dot(t, v), ab * dot(b, v), NoV));
    float lambdaL = NoV * length(vec3(at * dot(t, l), ab * dot(b, l), NoL));
    return D * 0.5 / (lambdaV + lambdaL) * BaseFresnel(max(dot(l, h), 0.0)) * NoL;
}

// Hair: Kajiya-Kay's strand highlights in Scheuermann's two-lobe form, a
// primary reflection (Fresnel F0) shifted towards the root and a wider
// secondary through the fibre (tinted by the hair's colour) shifted
// towards the tip. Each lobe is Blinn-Phong-normalized with the exponent
// as wide as GGX at roughness `a`, times 1/4 for visibility, times cos
// (the strands are opaque tubes).
// ponytail: no transmission (TT) lobe, so backlit hair gets no glow; a
// Marschner model (Karis 2016) would add it.
float Strand(vec3 t, vec3 h, float p)
{
    float th = dot(t, h);
    return (p + 2.0) / (2.0 * PI) * pow(max(1.0 - th * th, 0.0), 0.5 * p) * smoothstep(-1.0, 0.0, th);
}

vec3 HairSpecular(vec3 n, vec3 v, vec3 l, float a, vec3 f0)
{
    vec3 h = normalize(v + l), t = layers.bitangent;
    a = max(a, 0.01);
    float p = 2.0 / (a * a) - 2.0;
    float primary = Strand(normalize(t - 0.1 * n), h, p);
    float secondary = Strand(normalize(t + 0.15 * n), h, 0.25 * p);
    return (F_Schlick(f0, max(dot(l, h), 0.0)) * primary + 0.5 * layers.hairTint * secondary) * 0.25 *
           clamp(dot(n, l), 0.0, 1.0);
}

// The base's specular where it has a tangent frame (mesh.frag's
// Specular): hair, or anisotropic GGX.
vec3 TangentSpecular(vec3 n, vec3 v, vec3 l, float a, vec3 f0, float NoV)
{
    return layers.hair ? HairSpecular(n, v, l, a, f0) : AnisotropicSpecular(n, v, l, a, f0, NoV);
}

// The normal the dome's reflection is looked up with: anisotropic
// highlights bend it towards the stretch (Filament's bent normal).
vec3 BentNormal(vec3 n, vec3 v, float perceptual)
{
    if (layers.anisotropy <= 0.0) {
        return n;
    }
    vec3 across = cross(layers.bitangent, v);
    vec3 stretched = cross(across, layers.bitangent);
    return normalize(mix(n, stretched, layers.anisotropy * clamp(5.0 * perceptual, 0.0, 1.0)));
}

// The lobes' reflection of light from direction l, times cos, per unit of
// the light's specular radiance. `regionArea`: the light's area in
// half-vector space / pi (mesh.frag's LightRegion); the coat normalizes its
// own lobe for it as for a disc of that area.
// ponytail: the coat shares the base's representative point.
vec3 LayerLight(vec3 n, vec3 v, vec3 l, float NoV, float regionArea)
{
    vec3 c = vec3(0.0);
    float NoL = clamp(dot(n, l), 0.0, 1.0);
    vec3 h = normalize(v + l);
    float NoH = max(dot(n, h), 0.0);
    gCoatLight = 0.0;
    if (layers.coat > 0.0) {
        float a = layers.coatPerceptual * layers.coatPerceptual;
        float F = CoatFresnel(max(dot(l, h), 0.0));
        gCoatLight = layers.coat * D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, max(NoL, 1e-4), a) * F *
                     (1.0 + F * layers.coatMsK) * NoL * (a * a) / (a * a + regionArea);
        c += gCoatLight;
    }
    if (layers.sheen != vec3(0.0)) {
        c += layers.sheen * (layers.fuzz ? layers.sheenAlbedo * ZeltnerLobe(n, v, l, NoV, layers.sheenRoughness)
                                         : D_Sheen(NoH, layers.sheenRoughness) / (4.0 * (NoL + NoV - NoL * NoV)) * NoL);
    }
    return c;
}

// The lobes' share of the dome's light (mesh.frag's IBL, Lavg its average
// radiance over the normal's hemisphere): the coat reflects the
// prefiltered environment at its own roughness, sheen the average.
// ponytail: no screen-space reflections in the coat (only the base traces
// them); a second trace for smooth coats over rough bases would add them.
vec3 LayerAmbient(vec3 n, vec3 v, vec3 Lavg, float diffuseAO, float specularAO)
{
    vec3 c = vec3(0.0);
    if (frame.envRadiance.w <= 0.0) {
        return c;
    }
    if (layers.coat > 0.0) {
        vec3 r = normalize(mat3(frame.envWorldToLocal) * reflect(-v, n));
        float lod = layers.coatPerceptual * float(frame.counts.z - 1u);
        gCoatAmbient = layers.coat * layers.coatAlbedo * textureLod(envCube, r, lod).rgb * frame.envRadiance.rgb * specularAO;
        c += gCoatAmbient;
    }
    c += layers.sheen * layers.sheenAlbedo * Lavg * diffuseAO;
    if (layers.hair) {
        // The secondary strand lobe: the dome around the strand (bent
        // normal), blurrier than the primary, tinted.
        vec3 r = normalize(mat3(frame.envWorldToLocal) * reflect(-v, BentNormal(n, v, 1.0)));
        c += 0.25 * layers.hairTint * textureLod(envCube, r, 0.6 * float(frame.counts.z - 1u)).rgb *
             frame.envRadiance.rgb * specularAO;
    }
    return c;
}

#else

void PrepareLayers(vec3 n, float NoV, inout vec3 diffuseColor, inout vec3 emissive)
{
}

void CoatRoughening(inout float roughness)
{
}

vec3 BentNormal(vec3 n, vec3 v, float perceptual)
{
    return n;
}

vec3 LayerLight(vec3 n, vec3 v, vec3 l, float NoV, float regionArea)
{
    return vec3(0.0);
}

vec3 LayerAmbient(vec3 n, vec3 v, vec3 Lavg, float diffuseAO, float specularAO)
{
    return vec3(0.0);
}

#endif

// NPR toon shading, as EEVEE's Shader to RGB into a constant colour ramp:
// the lit result's intensity relative to the surface's albedo (so textures
// keep their detail) falls into `bands` flat steps per unit, each shown at
// its middle (highlights past white keep stepping); edges are `softness`
// of a band wide, at least a pixel. Emission stays as it is.
vec3 Toon(vec3 color, vec3 emissive, vec3 albedo, float bands, float softness)
{
    const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
    vec3 lit = color - emissive;
    float intensity = dot(lit, luma) / max(dot(albedo, luma), 1e-4);
    float x = intensity * bands;
    float edge = clamp(max(softness, fwidth(x)), 1e-4, 1.0);
    float level = (floor(x) + smoothstep(1.0 - edge, 1.0, fract(x)) + 0.5) / bands;
    return emissive + lit * (level / max(intensity, 1e-4));
}

// Last, before cutouts discard (fwidth needs the whole quad): toon bands
// from the material's eldr:toonBands (hydra/material.cpp), else the
// frame's for every material. `albedo`: diffuse colour plus F0.
vec3 ToonShade(vec3 color, vec3 emissive, vec3 albedo)
{
    float bands = Feature(kFeatureToon) ? float(frame.styles.y) : 0.0, softness = 0.05;
#ifdef LAYERS
    albedo = layers.albedo;  // sheen and coats would make bands uneven
    if (layers.toonBands > 0.0 && (frame.styles.x & kStyleToon) != 0u) {
        bands = layers.toonBands;
        softness = layers.toonSoftness;
    }
#endif
    return bands > 0.0 ? Toon(color, emissive, albedo, bands, softness) : color;
}
