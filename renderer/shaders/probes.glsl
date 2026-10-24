// Reflection probe and planar reflections (Renderer's Reflections,
// probes.h); include after frame.glsl.
layout(set = 0, binding = 50) uniform samplerCube probeCube;      // world directions; level 0 alpha: distance (0: sky)
layout(set = 0, binding = 51) uniform sampler2D planarReflection;  // the scene mirrored, premultiplied

// The probe's prefiltered light along the reflection ray p + r t, parallax
// corrected: the direction from the capture point to where the ray meets
// the captured surroundings. First guess where it leaves the scene's
// bounds, then two fixed-point steps on the captured distances. Where the
// probe sees sky, the ray's own direction if the sky is there too (it is
// infinitely far).
vec3 ProbeSpecular(vec3 p, vec3 r, float perceptual)
{
    vec3 o = p - frame.probe.xyz;
    vec3 safe = mix(vec3(1e-8), r, greaterThan(abs(r), vec3(1e-8)));
    vec3 exits = (mix(frame.probeMin.xyz, frame.probeMax.xyz, step(0.0, r)) - p) / safe;
    vec3 dir = o + r * max(min(min(exits.x, exits.y), exits.z), 0.0);
    for (int i = 0; i < 2; ++i) {
        float distance = textureLod(probeCube, dir, 0.0).a;
        if (distance <= 0.0) {
            dir = textureLod(probeCube, r, 0.0).a <= 0.0 ? r : dir;
            break;
        }
        float b = dot(o, r), disc = b * b - dot(o, o) + distance * distance;
        if (disc < 0.0) {
            break;  // the ray passes that surface by: keep the guess
        }
        dir = o + r * max(-b + sqrt(disc), 0.0);
    }
    return textureLod(probeCube, dir, perceptual * frame.probe.w).rgb;
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
