// Translucent surfaces in mesh.frag (renderer/transparency.h); include
// after mesh.frag's outputs.

// The pass a shading pipeline draws (the Renderer specializes it): 0 the
// opaque pass or sorted translucency (colour premultiplied by coverage,
// blended over), 1 weighted blended OIT accumulation.
layout(constant_id = 12) const uint kTranslucentPass = 0u;

// frame.features.x: refraction samples the opaque colour behind (bound below).
const uint kFeatureRefraction = 4096u;

// The opaque colour (premultiplied) with mips, copied after the opaque
// pass (Transparency::RecordBackground).
layout(set = 0, binding = 42) uniform sampler2D refractionBackground;

// Transmission, split by TransmissionBegin: the fraction of the pixel
// seen straight through the surface (opacity's complement, blended over
// what's behind it) and the fraction it refracts (sampled from the
// background by TransmissionEnd).
float gStraight = 0.0, gRefracted = 0.0;

// Before shading: `alpha` (opacity) becomes the fraction that reflects
// diffusely and emits.
void TransmissionBegin(inout float alpha)
{
    gStraight = 1.0 - alpha;
    gRefracted = 0.0;
    if (inMaterial == kNoMaterial) {
        return;
    }
    Material m = materials[inMaterial];
    gRefracted = alpha * m.transmission.w;
    alpha *= 1.0 - m.transmission.w;
    if (m.refraction.w > 0.0) {
        gRefracted += gStraight;  // UsdPreviewSurface with an ior
        gStraight = 0.0;
    }
}

// After shading (`color` premultiplied, highlights whole): adds what the
// surface refracts and returns the coverage to blend over what's behind
// with. The refracted ray (thin walls: the view ray) leaves the surface
// after a thickness estimate, the distance to the opaque surface behind
// but at most the mesh's size, and reads the background where that exit
// point lands, blurred over mips and frame.translucency.x rotated taps by
// the roughness spreading it over the distance to the background. The
// transmission colour absorbs at the surface, or over the path inside at
// its transmission depth (Beer-Lambert); Fresnel (Schlick at the ior)
// splits reflection from transmission. Where it refracts the app's
// background (clear, coverage 0) coverage drops instead. Closed refractive
// meshes show their front only (back faces, by the authored normal, are
// discarded), so the two refractions read as one.
// ponytail: one refraction (no exit interface), screen space only (what
// isn't on screen or is translucent itself isn't refracted).
float TransmissionEnd(inout vec3 color, vec3 n, vec3 v, float perceptual)
{
    if (gRefracted <= 0.0) {
        return 1.0 - gStraight;
    }
    Material m = materials[inMaterial];
    float ior = max(m.refraction.x, 1e-3);
    bool thin = m.refraction.y > 0.0;
    if (!thin && m.refraction.x != 1.0 && dot(inNormal, v) < 0.0) {
        discard;
    }
    float f0 = (ior - 1.0) / (ior + 1.0);
    f0 *= f0;
    float through = gRefracted * (1.0 - (f0 + (1.0 - f0) * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0)));
    if ((frame.features.x & kFeatureRefraction) == 0u) {
        return 1.0 - gStraight - through;  // no background: seen straight through, untinted
    }

    vec2 pixelUv = gl_FragCoord.xy / frame.viewport.xy;
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    vec4 behindP = frame.invViewProj * vec4(pixelUv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    float camera = length(inWorldPos - frame.cameraPos.xyz);
    float behind = depth < 1.0 ? length(behindP.xyz / behindP.w - inWorldPos) : 1e30;
    Instance inst = instances[inSlot];
    DrawInfo d = draws[inst.draw];
    float size = length(mat3(InstanceTransform(inst)) * (d.boundsMax.xyz - d.boundsMin.xyz));
    float thickness = thin ? 0.0 : min(behind, size);
    vec3 r = thin ? -v : refract(-v, n, 1.0 / ior);
    r = dot(r, r) > 0.0 ? r : -v;  // total internal reflection (ior < 1)
    vec3 exitP = inWorldPos + r * thickness;
    vec4 clip = frame.viewProj * vec4(exitP, 1.0);
    vec2 uv = clip.w > 0.0 ? clamp(clip.xy / clip.w * 0.5 + 0.5, 0.0, 1.0) : pixelUv;

    float spread = perceptual * perceptual * min(behind, camera);
    vec3 side = normalize(cross(v, abs(v.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec4 clipSide = frame.viewProj * vec4(exitP + side * spread, 1.0);
    float pixels = clip.w > 0.0 && clipSide.w > 0.0
        ? length((clipSide.xy / clipSide.w - clip.xy / clip.w) * 0.5 * frame.viewport.xy) : 0.0;
    int taps = clamp(int(frame.translucency.x), 1, 16);
    float lod = clamp(log2(max(pixels * inversesqrt(float(taps)), 1e-3)), 0.0, frame.translucency.y - 1.0);
    float angle = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * float(frame.localLights.y),
                                                           vec2(0.06711056, 0.00583715))));
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    vec4 background = vec4(0.0);
    for (int i = 0; i < taps; ++i) {
        vec2 offset = taps > 1 ? rotation * kPoisson[i] * pixels / frame.viewport.xy : vec2(0.0);
        background += textureLod(refractionBackground, clamp(uv + offset, 0.0, 1.0), lod);
    }
    background /= float(taps);

    vec3 tint = m.transmission.rgb;
    if (m.refraction.z > 0.0 && !thin) {
        tint = pow(max(tint, vec3(1e-4)), vec3(thickness / m.refraction.z));
    }
    vec3 transmitted = through * tint;
    color += transmitted * background.rgb;
    return 1.0 - gStraight - dot(transmitted, vec3(1.0 / 3.0)) * (1.0 - background.a);
}

// OIT: premultiplied colour and coverage times a weight falling with the
// distance (relative to the nearest translucent mesh's, to the power
// frame.translucency.w) are summed in location 0; location 3, the
// revealage, is multiplied by 1 - coverage. Pure highlights (glass at
// opacity 0) keep a sliver of coverage so they still count.
void TranslucentOutput(vec3 color, float alpha)
{
    if (kTranslucentPass != 1u) {
        return;
    }
    float a = clamp(alpha, 1.0 / 256.0, 1.0);
    float z = max(length(inWorldPos - frame.cameraPos.xyz) * frame.translucency.z, 1e-4);
    float w = a * clamp(pow(z, -frame.translucency.w), 1e-2, 1e3);
    outColor = vec4(color, a) * w;
    outVelocity = vec2(a);
}
