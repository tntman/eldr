// Translucent surfaces in mesh.frag (renderer/transparency.h); include
// after mesh.frag's outputs.

// The pass a shading pipeline draws (the Renderer specializes it): 0 the
// opaque pass or sorted translucency (colour premultiplied by coverage,
// blended over), 1 weighted blended OIT accumulation, 2 the subsurface
// pass (subsurface.glsl), 3 refracting back faces (TransmissionEnd).
layout(constant_id = 12) const uint kShadingPass = 0u;

// frame.features.x: refraction samples the opaque colour behind (bound
// below); holdouts are cut out (PassOutput).
const uint kFeatureRefraction = 4096u, kFeatureHoldouts = 32768u;

// The opaque colour (premultiplied) with mips, copied after the opaque
// pass (Transparency::RecordBackground).
layout(set = 0, binding = 42) uniform sampler2D refractionBackground;
// Traced refraction (rt_refraction.rgen; frame.rtRefraction.x): per pixel
// the light along the refracted ray of the nearest refracting surface,
// that surface's distance in a (negative: the app's background).
layout(set = 0, binding = 86, rgba16f) uniform readonly image2D rtRefractionImage;

// Transmission, split by TransmissionBegin: the fraction of the pixel
// seen straight through the surface (opacity's complement, blended over
// what's behind it) and the fraction it refracts (sampled from the
// background by TransmissionEnd).
float gStraight = 0.0, gRefracted = 0.0;
// Opacity presence (UsdPreviewSurface opacityMode): the light is shaded at
// full opacity and scaled by this at the end, highlights too.
float gPresence = 1.0;
// The light TransmissionEnd added from the background (the light splits'
// indirect glossy: Karma counts refraction with the specular lobes).
vec3 gRefractedLight = vec3(0.0);

// Before shading: `alpha` (opacity) becomes the fraction that reflects
// diffusely and emits.
void TransmissionBegin(inout float alpha)
{
    gStraight = 1.0 - alpha;
    gRefracted = 0.0;
    if (inMaterial == kNoMaterial) {
        return;
    }
    Material m = ShadingMaterial();
    if (m.subsurfaceRadius.w > 0.0 && frame.rtRefraction.y != 0u) {
        gPresence = alpha;  // alpha is out coverage (gStraight); no refraction
        alpha = 1.0;
        return;
    }
    gRefracted = alpha * m.transmission.w;
    alpha *= 1.0 - m.transmission.w;
    if (m.refraction.w > 0.0) {
        gRefracted += gStraight;  // UsdPreviewSurface with an ior
        gStraight = 0.0;
    }
}

// The opaque colour (and what refracting layers behind added, see
// Renderer::_RenderTranslucency) along direction `dir` from the surface,
// after `distance`: where that point lands on screen, blurred over mips and
// frame.translucency.x rotated taps by the roughness spreading the rays
// over `spread`.
vec4 RefractedBackground(vec3 dir, float distance, float spread)
{
    vec3 exitP = inWorldPos + dir * distance;
    vec4 clip = frame.viewProj * vec4(exitP, 1.0);
    vec2 uv = clip.w > 0.0 ? clamp(clip.xy / clip.w * 0.5 + 0.5, 0.0, 1.0) : gl_FragCoord.xy / frame.viewport.xy;
    vec3 v = ViewDirection(inWorldPos);
    vec3 side = normalize(cross(v, abs(v.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec4 clipSide = frame.viewProj * vec4(exitP + side * spread, 1.0);
    float pixels = clip.w > 0.0 && clipSide.w > 0.0
        ? length((clipSide.xy / clipSide.w - clip.xy / clip.w) * 0.5 * frame.viewport.xy) : 0.0;
    int taps = clamp(int(frame.translucency.x), 1, 16);
    float lod = clamp(log2(max(pixels * inversesqrt(float(taps)), 1e-3)), 0.0, frame.translucency.y - 1.0);
    float angle = 6.2831853 * SampleNoise(kDimRefraction, gl_FragCoord.xy, float(frame.localLights.y));
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    vec4 background = vec4(0.0);
    for (int i = 0; i < taps; ++i) {
        vec2 offset = taps > 1 ? rotation * kPoisson[i] * pixels / frame.viewport.xy : vec2(0.0);
        background += textureLod(refractionBackground, clamp(uv + offset, 0.0, 1.0), lod);
    }
    return background / float(taps);
}

// The dome's radiance along dir at mip `lod` (0 without a dome).
vec3 DomeAlong(vec3 dir, float lod)
{
    if (frame.envRadiance.w <= 0.0) {
        return vec3(0.0);
    }
    return textureLod(envCube, mat3(frame.envWorldToLocal) * dir, lod).rgb * frame.envRadiance.rgb;
}

// After shading (`color` premultiplied, highlights whole): adds what the
// surface refracts and returns the coverage to blend over what's behind
// with. The refracted ray (thin walls: the view ray) leaves the surface
// after a thickness estimate, the distance to the opaque surface behind
// but at most the mesh's size, and reads the background where that exit
// point lands (RefractedBackground). The transmission colour absorbs at
// the surface, or over the path inside at its transmission depth
// (Beer-Lambert); Fresnel (Schlick at the ior) splits reflection from
// transmission. Where it refracts past every drawn surface and sky, the
// dome lights it; without a dome coverage drops instead (the app's background).
// Closed refractive meshes: the shading passes draw their front faces and
// discard the back ones (by the authored normal); kShadingPass 3 draws
// only those back faces, before the front ones (Renderer's refraction
// layers): the surface seen from inside the medium (water from below,
// the far side of a glass ball), with the inverted ior, so beyond the
// critical angle it reflects the inside totally (sampled along the
// mirrored ray) instead of letting light out.
// With traced refraction (frame.rtRefraction.x) the front surface the
// ray tracing pass found at this pixel takes its light from there instead
// (the path through the glass, absorption inside included), and back
// faces there draw nothing; other refracting fragments stay screen space.
// ponytail: screen space: what isn't on screen isn't refracted; one
// interface per layer, the thickness an estimate.
// How far along e (object space, scaled so the ellipsoid is the unit ball;
// the distance in e's units) o is from leaving the unit ball.
float EllipsoidExit(vec3 o, vec3 e)
{
    float a = max(dot(e, e), 1e-12), b = dot(o, e), c = dot(o, o) - 1.0;
    float disc = b * b - a * c;
    return disc > 0.0 ? max((-b + sqrt(disc)) / a, 0.0) : 0.0;
}

float TransmissionEnd(inout vec3 color, vec3 n, vec3 v, float perceptual)
{
    if (gRefracted <= 0.0) {
        if (kShadingPass == 3u) {
            discard;  // refracts nothing: drawn by the shading passes
        }
        return 1.0 - gStraight;
    }
    Material m = ShadingMaterial();
    float ior = max(m.refraction.x, 1e-3);
    bool thin = m.refraction.y > 0.0;
    bool inside = !thin && m.refraction.x != 1.0 && dot(inNormal, v) < 0.0;
    if (inside != (kShadingPass == 3u)) {
        discard;
    }
    float f0 = (ior - 1.0) / (ior + 1.0);
    f0 *= f0;
    vec3 r = thin ? -v : refract(-v, n, inside ? ior : 1.0 / ior);
    bool total = inside && dot(r, r) == 0.0;  // total internal reflection
    r = dot(r, r) > 0.0 ? r : -v;  // (ior < 1 seen from outside: straight on, as before)
    // Schlick's cosine is the one on the thinner medium's side.
    float cosine = inside ? (total ? 0.0 : clamp(-dot(r, n), 0.0, 1.0)) : clamp(dot(n, v), 0.0, 1.0);
    float F = total ? 1.0 : f0 + (1.0 - f0) * pow(1.0 - cosine, 5.0);
    // The coat (and OpenPBR's fuzz) lie over the transmission as over the
    // rest of the base: their colour and what they reflect (brdf.glsl).
    vec3 over = vec3(1.0);
#ifdef LAYERS
    over = layers.under;
#endif
    // A thin film reflects what its specular reflects (brdf.glsl's
    // Iridescence, at this cosine): that much less gets through, in the
    // complementary colour (not the film's reflection on top of all the
    // light: a soap bubble came out brighter than the light around it).
    vec3 reflectance = vec3(F);
#ifdef LAYERS
    if (m.thinFilm.x > 0.0 && (frame.features.x & kFeatureThinFilm) != 0u && !total) {
        reflectance = mix(reflectance, min(Iridescence(cosine, m.thinFilm.y, m.thinFilm.z, vec3(f0)), vec3(1.0)),
                          m.thinFilm.x);
    }
#endif
    vec3 through = gRefracted * (1.0 - reflectance);
    if (!Feature(kFeatureRefraction)) {
        return 1.0 - gStraight - dot(through, vec3(1.0 / 3.0));  // no background: seen straight through, untinted
    }
    // Only where no opaque surface lies straight behind does light that
    // refracts out of the scene (no dome) give coverage back to the app's
    // background; elsewhere that surface would show unrefracted through the
    // glass (caustics.usda's ball showed the lit floor behind it, Karma
    // black below its middle), so that light is black.
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float open = depth < 1.0 ? 0.0 : 1.0;
    if (frame.rtRefraction.x != 0u) {
        vec4 traced = imageLoad(rtRefractionImage, ivec2(gl_FragCoord.xy));
        if (kShadingPass == 3u && traced.a != 0.0) {
            discard;  // the traced path went through this back face
        }
        float camera = length(inWorldPos - frame.cameraPos.xyz);
        if (!inside && abs(abs(traced.a) - camera) <= 0.01 * camera) {
            vec3 transmitted = through * over * (m.refraction.z > 0.0 && !thin ? vec3(1.0) : m.transmission.rgb);
            color += transmitted * traced.rgb;
            gRefractedLight += transmitted * traced.rgb;  // indirect glossy (light splits)
            return 1.0 - gStraight - (traced.a < 0.0 ? open * dot(transmitted, vec3(1.0 / 3.0)) : 0.0);
        }
    }

    vec4 behindP = frame.invViewProj * vec4(gl_FragCoord.xy / frame.viewport.xy * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    float camera = length(inWorldPos - frame.cameraPos.xyz);
    float behind = depth < 1.0 ? length(behindP.xyz / behindP.w - inWorldPos) : 1e30;
    Instance inst = instances[inSlot];
    DrawInfo d = draws[inst.draw];
    float size = length(mat3(InstanceTransform(inst)) * (d.boundsMax.xyz - d.boundsMin.xyz));
    float thickness = thin ? 0.0 : min(behind, size);
    float spread = perceptual * perceptual * min(behind, camera);
    vec3 tint = m.transmission.rgb;
    vec3 tintInside = tint;  // along the inside reflection
    if (m.refraction.z > 0.0 && !thin) {
        // The path inside: through the ellipsoid that fills the mesh's
        // bounds (exact for a ball), not the bounds' diagonal. A back face
        // seen from outside adds none (its front face counted the path),
        // from inside the medium the way to the camera. Flat meshes (water
        // sheets): the distance to what is behind, at most the mesh's size.
        mat4 xf = InstanceTransform(inst);
        mat3 toObject = inverse(mat3(xf));
        vec3 extent = 0.5 * (d.boundsMax.xyz - d.boundsMin.xyz);
        vec3 centre = 0.5 * (d.boundsMax.xyz + d.boundsMin.xyz);
        float path = thickness, pathInside = thickness;
        if (min(extent.x, min(extent.y, extent.z)) > 1e-3 * max(extent.x, max(extent.y, extent.z))) {
            vec3 o = (toObject * (inWorldPos - xf[3].xyz) - centre) / extent;
            vec3 eye = (toObject * (frame.cameraPos.xyz - xf[3].xyz) - centre) / extent;
            path = inside ? (dot(eye, eye) < 1.0 ? camera : 0.0) : min(behind, EllipsoidExit(o, toObject * r / extent));
            pathInside = EllipsoidExit(o, toObject * reflect(-v, n) / extent);
        }
        tint = pow(max(tint, vec3(1e-4)), vec3(path / m.refraction.z));
        tintInside = pow(max(tintInside, vec3(1e-4)), vec3(pathInside / m.refraction.z));
    }
    float lost = 0.0;  // coverage the app's background takes back
    // Where the lookup finds no drawn surface or sky (a dome hidden from the
    // camera), the dome lights it along the ray, as traced refraction and
    // Karma see it; only without a dome does the app's background show.
    bool dome = frame.envRadiance.w > 0.0;
    float domeLod = perceptual * float(frame.counts.z - 1u);
    if (!total) {
        vec4 background = RefractedBackground(r, thickness, spread);
        vec3 transmitted = through * over * tint;
        vec3 light = background.rgb + (1.0 - background.a) * DomeAlong(r, domeLod);
        color += transmitted * light;
        gRefractedLight += transmitted * light;
        lost += dome ? 0.0 : dot(transmitted, vec3(1.0 / 3.0)) * (1.0 - background.a);
    }
    if (inside) {
        // The inside's reflection, all of it past the critical angle.
        vec3 inward = reflect(-v, n);
        vec4 mirrored = RefractedBackground(inward, min(camera, size), spread);
        vec3 reflected = gRefracted * F * over * tintInside;
        vec3 light = mirrored.rgb + (1.0 - mirrored.a) * DomeAlong(inward, domeLod);
        color += reflected * light;
        gRefractedLight += reflected * light;
        lost += dome ? 0.0 : dot(reflected, vec3(1.0 / 3.0)) * (1.0 - mirrored.a);
    }
    return 1.0 - gStraight - open * lost;
}

// frame.features.x: translucent casters tint the sun's and local lights'
// shadows (Transparency's tint maps, below).
const uint kFeatureTranslucentShadows = 8192u;

// Per sun cascade (layer) and for the local lights' atlas: the colour
// translucent casters let through and the depth of the nearest of them.
layout(set = 0, binding = 43) uniform sampler2DArray sunTint;
layout(set = 0, binding = 44) uniform sampler2DArray sunTintDepth;
layout(set = 0, binding = 45) uniform sampler2D atlasTint;
layout(set = 0, binding = 46) uniform sampler2D atlasTintDepth;

// Bilinear over the 2 x 2 texels around uv (in texels of `size`), each
// tinting only receivers behind its caster (window depth ref).
vec3 TintLookup(sampler2DArray tint, sampler2DArray tintDepth, sampler2D atlas, sampler2D atlasDepth, bool sun,
                vec2 uv, float layer, float ref, vec2 lo, vec2 hi)
{
    float size = float(sun ? textureSize(tint, 0).x : textureSize(atlas, 0).x);
    vec2 t = uv * size - 0.5;
    vec2 f = fract(t);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        ivec2 o = ivec2(i & 1, i >> 1);
        ivec2 q = ivec2(clamp(floor(t) + vec2(o), lo * size, hi * size - 1.0));
        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float d = sun ? texelFetch(tintDepth, ivec3(q, int(layer)), 0).r : texelFetch(atlasDepth, q, 0).r;
        vec3 c = sun ? texelFetch(tint, ivec3(q, int(layer)), 0).rgb : texelFetch(atlas, q, 0).rgb;
        sum += w * (ref > d ? c : vec3(1.0));
    }
    return sum;
}

// The colour translucent casters let through to p from the shadow-casting
// distant light (direction l), looked up as Shadow() does in mesh.frag.
vec3 ShadowTint(vec3 p, vec3 ng, vec3 l)
{
    vec3 traced;
    if (RtShadowTint(frame.counts.y, p, ng, traced)) {
        return traced;
    }
    if (!Feature(kFeatureTranslucentShadows)) {
        return vec3(1.0);
    }
    float depth = dot(p - frame.cameraPos.xyz, frame.cameraForward.xyz);
    int c = 0;
    while (c < 4 && depth > frame.cascadeSplits[c]) {
        ++c;
    }
    if (c == 4) {
        return vec3(1.0);
    }
    float texel = frame.cascadeTexel[c];
    float NoL = clamp(dot(ng, l), 0.0, 1.0);
    vec4 clip = frame.cascadeViewProj[c] * vec4(p + ng * texel * 1.5 * (1.0 - NoL) + l * texel * 0.5, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    return TintLookup(sunTint, sunTintDepth, atlasTint, atlasTintDepth, true, ndc.xy * 0.5 + 0.5, float(c),
                      ndc.z * 0.5 + 0.5, vec2(0.0), vec2(1.0));
}

// The same for local light `index`, looked up as LocalShadow() does.
vec3 LocalShadowTint(uint index, Light light, vec3 p, vec3 ng)
{
    vec3 traced;
    if (RtShadowTint(index, p, ng, traced)) {
        return traced;
    }
    uint tile = lightShadow[index];
    if (!Feature(kFeatureTranslucentShadows) || tile == kNone) {
        return vec3(1.0);
    }
    vec3 fromLight = p - shadowTiles[tile].origin.xyz;
    if (light.shaping.y > kSpotShadowCone) {
        vec3 a = abs(fromLight);
        tile += a.x >= a.y && a.x >= a.z ? (fromLight.x > 0.0 ? 0u : 1u)
              : a.y >= a.z               ? (fromLight.y > 0.0 ? 2u : 3u)
                                         : (fromLight.z > 0.0 ? 4u : 5u);
    }
    ShadowTile t = shadowTiles[tile];
    float dist = length(fromLight);
    float texel = t.params.x * dist;
    vec3 l = -fromLight / max(dist, 1e-8);
    float NoL = clamp(dot(ng, l), 0.0, 1.0);
    vec4 clip = t.viewProj * vec4(p + ng * texel * 1.5 * (1.0 - NoL) + l * texel * 0.5, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    return TintLookup(sunTint, sunTintDepth, atlasTint, atlasTintDepth, false,
                      t.rect.xy + (ndc.xy * 0.5 + 0.5) * t.rect.zw, 0.0, ndc.z * 0.5 + 0.5, t.rect.xy,
                      t.rect.xy + t.rect.zw);
}

// The outputs of the passes after the opaque one. OIT: premultiplied
// colour and coverage times a weight falling with the distance (relative
// to the nearest translucent mesh's, to the power frame.translucency.w)
// are summed in location 0; location 3, the revealage, is multiplied by
// 1 - coverage. Pure highlights (glass at opacity 0) keep a sliver of
// coverage so they still count. Subsurface: the light in location 0, its
// alpha the largest radius, location 1 the radii's proportions (10 bits
// each) and bit 30, so the blur keeps materials apart.
// Holdouts (frame.features.x & kFeatureHoldouts) write nothing but their
// depth and ids: black with zero alpha in the opaque pass, no light in the
// subsurface pass, and translucent ones leave what's behind them alone.
void PassOutput(vec3 color, float alpha)
{
    if (Feature(kFeatureHoldouts) && draws[instances[inSlot].draw].holdout != 0u) {
        outColor = vec4(0.0);
        if (kShadingPass == 1u) {
            outVelocity = vec2(0.0);  // revealage unchanged
        } else if (kShadingPass == 2u) {
            outPrimId = 0;  // not a subsurface material
        }
        return;
    }
    if (kShadingPass == 2u) {
        vec3 radius = inMaterial != kNoMaterial ? max(ShadingMaterial().subsurfaceRadius.xyz, vec3(1e-6))
                                                : vec3(1.0);
        float largest = max(radius.r, max(radius.g, radius.b));
        uvec3 q = uvec3(round(radius / largest * 1023.0));
        outColor = vec4(color, largest);
        outPrimId = int(q.r | (q.g << 10) | (q.b << 20) | (1u << 30));
        return;
    }
    if (kShadingPass != 1u) {
        return;
    }
    float a = clamp(alpha, 1.0 / 256.0, 1.0);
    float z = max(length(inWorldPos - frame.cameraPos.xyz) * frame.translucency.z, 1e-4);
    float w = a * clamp(pow(z, -frame.translucency.w), 1e-2, 1e3);
    outColor = vec4(color, a) * w;
    outVelocity = vec2(a);
}
