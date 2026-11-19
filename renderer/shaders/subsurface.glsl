// Subsurface scattering in mesh.frag (renderer/subsurface.h); include
// after transparency.glsl.

// frame.features.x: the subsurface pass and blur run, and light shines
// through thin parts of subsurface materials.
const uint kFeatureSubsurface = 16384u;

// The subsurface lobe's share of the diffuse light, per channel (the light
// splits' subsurface).
vec3 gSubsurfaceShare = vec3(0.0);

// Before shading: the diffuse albedo mixes in the subsurface colour by
// its weight (standard_surface, OpenPBR). The subsurface pass
// (kShadingPass 2) keeps only that lobe, without highlights or emission,
// for Subsurface to blur.
void SubsurfaceBegin(inout vec3 baseColor, inout vec3 emissive, inout float metallic, inout vec3 specularTint)
{
    if (inMaterial == kNoMaterial) {
        return;
    }
    Material m = ShadingMaterial();
    if (m.refraction.y != 0.0 && Feature(kFeatureSubsurface)) {
        // Thin walled (a leaf): the subsurface lobe transmits only
        // (SubsurfaceTransmission), it reflects nothing (standard_surface's
        // translucent_bsdf).
        baseColor = kShadingPass == 2u ? vec3(0.0) : baseColor * (1.0 - m.subsurface.w);
        if (kShadingPass == 2u) {
            emissive = vec3(0.0);
            metallic = 0.0;
            specularTint = vec3(0.0);
        }
        return;
    }
    if (kShadingPass == 2u) {
        baseColor = m.subsurface.rgb * m.subsurface.w * (1.0 - clamp(metallic, 0.0, 1.0));
        emissive = vec3(0.0);
        metallic = 0.0;
        specularTint = vec3(0.0);
    } else {
        vec3 mixed = mix(baseColor, m.subsurface.rgb, m.subsurface.w);
        gSubsurfaceShare = clamp(m.subsurface.rgb * m.subsurface.w / max(mixed, vec3(1e-6)), 0.0, 1.0);
        baseColor = mixed;
    }
}

// After PrepareLayers (brdf.glsl): the subsurface pass keeps what a coat
// and sheen take from the diffuse light, but none of their own reflection.
void SubsurfaceLayers()
{
#ifdef LAYERS
    if (kShadingPass == 2u) {
        layers.coat = 0.0;
        layers.sheen = vec3(0.0);
    }
#endif
}

// Light from the shadow-casting sun through thin parts (ears, leaves) to
// the side facing away from it: the distance it travelled inside, from
// where the shadow map first saw the surface, is bracketed by comparing at
// half, one, two and four times the largest radius, and attenuates each
// channel by exp(-distance / radius). Thin walls let it through whole
// where the sun reaches their other side (other casters shadow it).
// Added in the shading and the subsurface pass, so the blur spreads it.
// ponytail: the sun only; local lights don't shine through.
vec3 SubsurfaceTransmission(vec3 p, vec3 n)
{
    if (!Feature(kFeatureSubsurface) || inMaterial == kNoMaterial || frame.counts.y == kNone) {
        return vec3(0.0);
    }
    Material m = ShadingMaterial();
    Light light = lights[frame.counts.y];
    vec3 l = light.position.xyz;
    float back = dot(-n, l);
    if (m.subsurface.w <= 0.0 || back <= 0.0 || !LightLinked(light, inLinkMask)) {
        return vec3(0.0);
    }
    vec3 radius = max(m.subsurfaceRadius.xyz, vec3(1e-6));
    float largest = max(radius.r, max(radius.g, radius.b));
    float depth = dot(p - frame.cameraPos.xyz, frame.cameraForward.xyz);
    int c = 0;
    while (c < 4 && depth > frame.cascadeSplits[c]) {
        ++c;
    }
    if (c == 4) {
        return vec3(0.0);
    }
    vec4 clip = frame.cascadeViewProj[c] * vec4(p, 1.0);
    vec4 towards = frame.cascadeViewProj[c] * vec4(p + l, 1.0);
    float ref = clip.z / clip.w * 0.5 + 0.5;
    float perUnit = abs(towards.z / towards.w - clip.z / clip.w) * 0.5;  // window depth per unit
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    float distance = 0.0, lit = 1.0;
    if (m.refraction.y == 0.0) {  // not thin walled
        distance = 8.0 * largest;
        for (int k = 2; k >= -1; --k) {
            float d = largest * exp2(float(k));
            if (texture(shadowMap, vec4(uv, float(c), ref - d * perUnit)) > 0.5) {
                distance = d;
            }
        }
    } else {
        // A thin wall: lit where the sun reaches its other side, i.e. where
        // nothing but the wall itself (within a few texels) is nearer the
        // sun; 3x3 PCF as Shadow().
        float bias = 3.0 * frame.cascadeTexel[c] * perUnit;
        vec2 ts = 1.0 / vec2(textureSize(shadowMap, 0).xy);
        lit = 0.0;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                lit += texture(shadowMap, vec4(uv + vec2(x, y) * ts, float(c), ref - bias));
            }
        }
        lit /= 9.0;
    }
    return m.subsurface.rgb * m.subsurface.w * light.color.rgb * light.halfHeight.w * back / PI * lit *
           exp(-distance / radius);
}
