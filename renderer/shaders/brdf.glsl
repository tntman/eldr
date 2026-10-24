// Shading beyond mesh.frag's metallic-roughness base (Lambert diffuse, GGX
// specular), from the material's constants (material.glsl): the specular
// workflow, a clearcoat, sheen, thin-film iridescence, anisotropy, hair
// (curves) and toon bands. mesh.frag includes it after its inputs and calls
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
// have them. Call after RunProgram.
Material ShadingMaterial()
{
    Material m = materials[inMaterial];
#if defined(GRAPH) && defined(LAYERS)
    uint lobes = (frame.styles.x & kStyleTexturedLobes) != 0u ? programLobes : 0u;
    if (lobes != 0u) {
        m.specularColor.rgb = (lobes & 1u) != 0u ? programLobe[0].rgb : m.specularColor.rgb;
        m.coat.x = (lobes & 2u) != 0u ? programLobe[1].x : m.coat.x;
        m.coat.y = (lobes & 4u) != 0u ? programLobe[2].x : m.coat.y;
        m.coatColor.rgb = (lobes & 8u) != 0u ? programLobe[3].rgb : m.coatColor.rgb;
        m.sheen.x = (lobes & 16u) != 0u ? programLobe[4].x : m.sheen.x;
        m.sheenColor.rgb = (lobes & 32u) != 0u ? programLobe[5].rgb : m.sheenColor.rgb;
        m.sheen.y = (lobes & 64u) != 0u ? programLobe[6].x : m.sheen.y;
        m.anisotropy.x = (lobes & 128u) != 0u ? programLobe[7].x : m.anisotropy.x;
        m.thinFilm.y = (lobes & 256u) != 0u ? programLobe[8].x : m.thinFilm.y;
        float r = (programLobe[9].x - 1.0) / (programLobe[9].x + 1.0);  // the coat's F0 from its IOR
        m.coat.z = (lobes & 512u) != 0u ? r * r : m.coat.z;
        m.thinFilm.x = (lobes & 1024u) != 0u ? programLobe[10].x : m.thinFilm.x;
        m.thinFilm.z = (lobes & 2048u) != 0u ? programLobe[11].x : m.thinFilm.z;
        m.anisotropy.y = (lobes & 4096u) != 0u ? programLobe[12].x : m.anisotropy.y;
        m.refraction.x = (lobes & 8192u) != 0u ? programLobe[13].x : m.refraction.x;
    }
    // Transmission and subsurface (transparency.glsl, subsurface.glsl):
    // the radius multiplies the constant (its scale).
    uint passes = (frame.styles.x & kStyleTexturedTransmission) != 0u ? programLobes : 0u;
    if ((passes & 507904u) != 0u) {
        m.transmission.w = (passes & 16384u) != 0u ? programLobe[14].x : m.transmission.w;
        m.transmission.rgb = (passes & 32768u) != 0u ? programLobe[15].rgb : m.transmission.rgb;
        m.subsurface.w = (passes & 65536u) != 0u ? programLobe[16].x : m.subsurface.w;
        m.subsurface.rgb = (passes & 131072u) != 0u ? programLobe[17].rgb : m.subsurface.rgb;
        m.subsurfaceRadius.xyz *= (passes & 262144u) != 0u ? programLobe[18].xyz : vec3(1.0);
    }
#endif
    return m;
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
    vec3 sheen;            // colour times weight; 0: no sheen
    float sheenRoughness;
    float sheenAlbedo;     // directional albedo towards the viewer
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
    return r.x / r.y;
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

// Before the lights: the pixel's lobes, and the base's F0 and colours as
// they change them.
void PrepareLayers(vec3 n, float NoV, inout vec3 f0, inout vec3 diffuseColor, inout vec3 tint, inout vec3 emissive)
{
    layers = Layers(0.0, 1.0, 0.0, 0.0, vec3(0.0), 1.0, 0.0, 0.0, false, vec3(0.0), vec3(0.0), 0.0, 0.0,
                    diffuseColor + f0, false, diffuseColor);
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
    // UsdPreviewSurface's specular workflow: F0 is specularColor (F90
    // white); hydra/material.cpp already dropped metallic.
    if (m.specularColor.w > 0.0 && (frame.features.x & kFeatureSpecularWorkflow) != 0u) {
        f0 = m.specularColor.rgb;
        layers.albedo = diffuseColor + f0;
    }
    // A thin film on the base's specular (dielectric and metal): its
    // reflectance towards the viewer stands in for F0.
    // ponytail: the film sees air above it, also under a coat.
    if (m.thinFilm.x > 0.0 && (frame.features.x & kFeatureThinFilm) != 0u) {
        f0 = mix(f0, Iridescence(NoV, m.thinFilm.y, m.thinFilm.z, f0), m.thinFilm.x);
    }
    // Sheen over the diffuse (standard_surface's sheen layer): the diffuse
    // loses the sheen's albedo.
    if (m.sheen.x > 0.0 && (frame.features.x & kFeatureSheen) != 0u) {
        layers.sheen = m.sheenColor.rgb * m.sheen.x;
        layers.sheenRoughness = clamp(m.sheen.y, 0.005, 1.0);
        layers.sheenAlbedo = SheenAlbedo(NoV, layers.sheenRoughness);
        diffuseColor *= 1.0 - m.sheen.x * layers.sheenAlbedo;
    }
    // Clearcoat (standard_surface's coat layer): everything beneath is
    // tinted by the coat colour and loses the coat's directional albedo,
    // emission included.
    if (m.coat.x > 0.0 && (frame.features.x & kFeatureClearcoat) != 0u) {
        layers.coat = m.coat.x;
        layers.coatPerceptual = clamp(m.coat.y, 0.045, 1.0);
        layers.coatF0 = m.coat.z;
        vec2 ab = texture(brdfLut, vec2(NoV, layers.coatPerceptual)).rg;
        layers.coatAlbedo = layers.coatF0 * ab.x + ab.y;
        vec3 base = mix(vec3(1.0), m.coatColor.rgb, layers.coat) * (1.0 - layers.coat * layers.coatAlbedo);
        diffuseColor *= base;
        tint *= base;
        emissive *= base;
        layers.sheen *= base;
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
    return D * 0.5 / (lambdaV + lambdaL) * F_Schlick(f0, max(dot(l, h), 0.0)) * NoL;
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
// the light's specular radiance. `widen`: what a local light's size adds
// to roughness (Karis' representative point, normalized as the base).
vec3 LayerLight(vec3 n, vec3 v, vec3 l, float NoV, float widen)
{
    vec3 c = vec3(0.0);
    float NoL = clamp(dot(n, l), 0.0, 1.0);
    vec3 h = normalize(v + l);
    float NoH = max(dot(n, h), 0.0);
    if (layers.coat > 0.0) {
        float a = layers.coatPerceptual * layers.coatPerceptual, aWide = clamp(a + widen, 0.0, 1.0);
        float F = F_Schlick(vec3(layers.coatF0), max(dot(l, h), 0.0)).x;
        c += layers.coat * D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, max(NoL, 1e-4), a) * F * NoL *
             (a * a) / (aWide * aWide);
    }
    if (layers.sheen != vec3(0.0)) {
        c += layers.sheen * D_Sheen(NoH, layers.sheenRoughness) / (4.0 * (NoL + NoV - NoL * NoV)) * NoL;
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
        c += layers.coat * layers.coatAlbedo * textureLod(envCube, r, lod).rgb * frame.envRadiance.rgb * specularAO;
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

void PrepareLayers(vec3 n, float NoV, inout vec3 f0, inout vec3 diffuseColor, inout vec3 tint, inout vec3 emissive)
{
}

vec3 BentNormal(vec3 n, vec3 v, float perceptual)
{
    return n;
}

vec3 LayerLight(vec3 n, vec3 v, vec3 l, float NoV, float widen)
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
