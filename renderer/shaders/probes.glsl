// Reflection probe and planar reflections (Renderer's Reflections,
// probes.h); include after frame.glsl.
layout(set = 0, binding = 50) uniform samplerCube probeCubes[8];  // world directions; level 0 alpha: distance (0: sky)
layout(set = 0, binding = 51) uniform sampler2D planarReflection;  // the scene mirrored, premultiplied

// Probe i's prefiltered light along the reflection ray p + r t, parallax
// corrected: the direction from its capture point to where the ray meets
// the captured surroundings. First guess where the ray leaves the probe's
// box, then two fixed-point steps on the captured distances. Where the
// probe sees sky, the ray's own direction if the sky is there too (it is
// infinitely far).
vec3 ProbeLookup(uint i, vec3 p, vec3 r, float perceptual)
{
    vec3 o = p - frame.probeCenter[i].xyz;
    vec3 safe = mix(vec3(1e-8), r, greaterThan(abs(r), vec3(1e-8)));
    vec3 exits = (mix(frame.probeMin[i].xyz, frame.probeMax[i].xyz, step(0.0, r)) - p) / safe;
    vec3 dir = o + r * max(min(min(exits.x, exits.y), exits.z), 0.0);
    for (int k = 0; k < 2; ++k) {
        float distance = textureLod(probeCubes[nonuniformEXT(i)], dir, 0.0).a;
        if (distance <= 0.0) {
            dir = textureLod(probeCubes[nonuniformEXT(i)], r, 0.0).a <= 0.0 ? r : dir;
            break;
        }
        float b = dot(o, r), disc = b * b - dot(o, o) + distance * distance;
        if (disc < 0.0) {
            break;  // the ray passes that surface by: keep the guess
        }
        dir = o + r * max(-b + sqrt(disc), 0.0);
    }
    return textureLod(probeCubes[nonuniformEXT(i)], dir, perceptual * frame.probeCenter[i].w).rgb;
}

const uint kProbeNormalize = 1u;  // frame.reflections.z

// Probe i's ambient light (irradiance / pi) towards n, from its SH L0, L1.
vec3 ProbeAmbient(uint i, vec3 n)
{
    uint b = i * 4u;
    vec3 e = frame.probeSh[b].rgb * 0.282095 + 0.488603 * (frame.probeSh[b + 1u].rgb * n.y +
                                                           frame.probeSh[b + 2u].rgb * n.z +
                                                           frame.probeSh[b + 3u].rgb * n.x);
    return max(e, vec3(0.0)) / PI;
}

// The probes' light over `dome`: smallest box first, each weighted by how
// far p is inside its box (fading in over its blend distance) times what
// the smaller ones left; the dome takes the rest. With kProbeNormalize
// each is scaled by the local ambient light (`local`, irradiance / pi from
// GI or SSGI) over the ambient light the probe saw where it was captured
// (EEVEE's normalization): a probe captured in daylight doesn't light a
// dark corner's reflections. ponytail: by luminance, at most 4x.
vec3 ProbeSpecular(vec3 p, vec3 r, float perceptual, vec3 dome, vec3 n, vec3 local)
{
    const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
    bool normalized = (frame.reflections.z & kProbeNormalize) != 0u;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < frame.reflections.x && weight < 0.999; ++i) {
        vec3 inside = min(p - frame.probeMin[i].xyz, frame.probeMax[i].xyz - p);
        float blend = frame.probeMin[i].w;
        float w = blend > 0.0 ? clamp(min(inside.x, min(inside.y, inside.z)) / blend, 0.0, 1.0) : 1.0;
        if (w > 0.0) {
            float scale = normalized ? clamp(dot(local, kLuma) / max(dot(ProbeAmbient(i, n), kLuma), 1e-6), 0.0, 4.0)
                                    : 1.0;
            sum += ProbeLookup(i, p, r, perceptual) * (w * (1.0 - weight) * scale);
            weight += w * (1.0 - weight);
        }
    }
    return sum + dome * (1.0 - weight);
}

// Whether p of mesh slot `draw` is on the mirror.
bool OnMirror(uint draw, vec3 p)
{
    return Feature(kFeaturePlanar) && draw == frame.planarInfo.x &&
           abs(dot(frame.planarPlane.xyz, p) + frame.planarPlane.w) <= 1e-3 * length(p - frame.cameraPos.xyz);
}

// The mirrored scene at this pixel, shifted by the normal's tilt from the
// plane (normal maps ripple it), over `specular` where it shows nothing;
// faded out from roughness 0.3 to 0.6 like SSR.
// ponytail: one sharp reflection, no blur for rougher mirrors.
vec3 PlanarSpecular(vec3 specular, vec3 p, vec3 n, float perceptual)
{
    vec3 tilt = n - frame.planarPlane.xyz * dot(n, frame.planarPlane.xyz);
    vec4 a = frame.viewProj * vec4(p, 1.0);
    vec4 b = frame.viewProj * vec4(p + tilt * (0.1 * length(p - frame.cameraPos.xyz)), 1.0);
    vec2 uv = gl_FragCoord.xy / frame.viewport.xy + (b.xy / b.w - a.xy / a.w) * 0.5;
    vec4 hit = textureLod(planarReflection, uv, 0.0);
    return mix(specular, hit.rgb + (1.0 - hit.a) * specular, 1.0 - smoothstep(0.3, 0.6, perceptual));
}
