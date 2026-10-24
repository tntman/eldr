// Subsurface scattering in mesh.frag (renderer/subsurface.h); include
// after transparency.glsl.

// frame.features.x: the subsurface pass and blur run, and light shines
// through thin parts of subsurface materials.
const uint kFeatureSubsurface = 16384u;

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
    if (kShadingPass == 2u) {
        baseColor = m.subsurface.rgb * m.subsurface.w * (1.0 - clamp(metallic, 0.0, 1.0));
        emissive = vec3(0.0);
        metallic = 0.0;
        specularTint = vec3(0.0);
    } else {
        baseColor = mix(baseColor, m.subsurface.rgb, m.subsurface.w);
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
// channel by exp(-distance / radius). Thin walls let it through whole.
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
    if (m.subsurface.w <= 0.0 || back <= 0.0) {
        return vec3(0.0);
    }
    vec3 radius = max(m.subsurfaceRadius.xyz, vec3(1e-6));
    float largest = max(radius.r, max(radius.g, radius.b));
    float distance = 0.0;
    if (m.refraction.y == 0.0) {  // not thin walled
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
        distance = 8.0 * largest;
        for (int k = 2; k >= -1; --k) {
            float d = largest * exp2(float(k));
            if (texture(shadowMap, vec4(uv, float(c), ref - d * perUnit)) > 0.5) {
                distance = d;
            }
        }
    }
    return m.subsurface.rgb * m.subsurface.w * light.color.rgb * light.halfHeight.w * back / PI *
           exp(-distance / radius);
}
