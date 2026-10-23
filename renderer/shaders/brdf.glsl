// Shading beyond mesh.frag's metallic-roughness base (Lambert diffuse, GGX
// specular), from the material's constants (material.glsl): the specular
// workflow, a clearcoat and sheen. mesh.frag includes it after its inputs
// and calls PrepareLayers once per pixel (before the lights), LayerLight
// per light and LayerAmbient for the dome. Only materials with lobes
// (vkr::Material::Layered) shade with the LAYERS build of mesh.frag; the
// plain build gets the empty versions at the end, so plain materials pay
// nothing. Within LAYERS each lobe costs a branch unless the material uses
// it and its FrameData.features.x flag is on.

// frame.features.x bits (renderer.cpp's Frame switches).
const uint kFeatureSpecularWorkflow = 8u, kFeatureClearcoat = 16u, kFeatureSheen = 32u;

#ifdef LAYERS

// mesh.frag's GGX pieces, shared by the lobes.
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

// Before the lights: the pixel's lobes, and the base's F0 and colours as
// they change them.
void PrepareLayers(float NoV, inout vec3 f0, inout vec3 diffuseColor, inout vec3 tint, inout vec3 emissive)
{
    layers = Layers(0.0, 1.0, 0.0, 0.0, vec3(0.0), 1.0, 0.0);
    if (inMaterial == kNoMaterial) {
        return;
    }
    Material m = materials[inMaterial];
    // UsdPreviewSurface's specular workflow: F0 is specularColor (F90
    // white); hydra/material.cpp already dropped metallic.
    if (m.specularColor.w > 0.0 && (frame.features.x & kFeatureSpecularWorkflow) != 0u) {
        f0 = m.specularColor.rgb;
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
    return c;
}

#else

void PrepareLayers(float NoV, inout vec3 f0, inout vec3 diffuseColor, inout vec3 tint, inout vec3 emissive)
{
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
