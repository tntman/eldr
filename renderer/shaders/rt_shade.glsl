// The ray generation shaders' tracing and shading (rt_pixels.rgen,
// rt_probes.rgen). Include after frame.glsl, lights.glsl, scene.glsl,
// material.glsl, rt.glsl, gi.glsl and volume.glsl.

layout(set = 0, binding = 5) uniform samplerCube envCube;
#include "caustics.glsl"

layout(location = 0) rayPayloadEXT RtHit hit;
layout(location = 1) rayPayloadEXT vec4 shadowRay;  // rt_shadow.rmiss, rt_shadow.rahit

// Closest surface along a ray, cone: its spread angle in radians; of the
// instances in `mask` (rt.glsl's kMask*).
void TraceMask(vec3 origin, vec3 dir, float cone, uint mask)
{
    hit.t = cone;
    traceRayEXT(tlas, gl_RayFlagsNoneEXT, mask, 0, 0, 0, origin, 0.0, dir, 1e30, 0);
}

void Trace(vec3 origin, vec3 dir, float cone)
{
    TraceMask(origin, dir, cone, kRtMaskSurfaces);
}

// What reaches origin from tMax along dir: 0 behind an opaque surface,
// else what the translucent casters (rt.shadowInfo.x) and the volumes
// (rt.shadowInfo.y) on the way let through.
vec3 TraceShadow(vec3 origin, vec3 dir, float tMax)
{
    shadowRay = vec4(1.0, 1.0, 1.0, 0.0);
    traceRayEXT(tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, rt.shadowInfo.x, 1, 0, 1,
                origin, 0.0, dir, tMax, 1);
    vec3 through = shadowRay.rgb * shadowRay.a;
    if (rt.shadowInfo.y > 0u && shadowRay.a > 0.0) {
        // ponytail: a sun's ray marches 1e4 units (the volumes' boxes clip it).
        float weighted, weighted2;
        through *= exp(-VolumeOpticalDepth(origin, dir * min(tMax, 1e4), rt.shadowInfo.y, rt.shadowVolume.x,
                                           rt.shadowVolume.y, weighted, weighted2));
    }
    return through;
}

// Ray origin just off a surface on the side of normal ng, robust at any
// scale (Waechter and Binder, Ray Tracing Gems ch. 6).
vec3 OffsetRay(vec3 p, vec3 ng)
{
    ivec3 o = ivec3(256.0 * ng);
    vec3 pi = intBitsToFloat(floatBitsToInt(p) + ivec3(p.x < 0.0 ? -o.x : o.x, p.y < 0.0 ? -o.y : o.y,
                                                       p.z < 0.0 ? -o.z : o.z));
    // Plus 256 ulps of the largest coordinate along the normal: per
    // component alone, Kitchen_set's floor (z = 0.04 at x, y up to 300) got
    // 1e-6, under both the hit point's error (rt.rchit's origin + t * dir)
    // and the intersector's from x and y, and shadowed itself: 47% of its
    // sunlit pixels lost over 10% of the sun.
    // ponytail: a hit near the origin seen from far away (t >> |p|) can
    // still fall short; then compute rt.rchit's position from the corners.
    float scale = max(max(abs(p.x), abs(p.y)), abs(p.z));
    return vec3(abs(p.x) < 1.0 / 32.0 ? p.x + ng.x / 65536.0 : pi.x,
                abs(p.y) < 1.0 / 32.0 ? p.y + ng.y / 65536.0 : pi.y,
                abs(p.z) < 1.0 / 32.0 ? p.z + ng.z / 65536.0 : pi.z) + ng * (scale * 3.05e-5);
}

// Random numbers: a hash (PCG3D, Jarzynski and Olano 2020) per pixel and
// frame, and an R2 low-discrepancy step per sample.
uvec3 Pcg3d(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

vec2 Random2(uvec3 seed, uint sampleIndex)
{
    vec2 u = vec2(Pcg3d(seed).xy) * (1.0 / 4294967296.0);
    return fract(u + vec2(0.7548776662, 0.5698402910) * float(sampleIndex));
}

// Orthonormal basis around n (Duff et al. 2017).
void Basis(vec3 n, out vec3 t, out vec3 b)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

// Uniform direction within cosMax of axis.
vec3 ConeSample(vec3 axis, float cosMax, vec2 u)
{
    float cosT = mix(1.0, cosMax, u.x);
    float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
    float phi = 2.0 * PI * u.y;
    vec3 t, b;
    Basis(axis, t, b);
    return normalize((t * cos(phi) + b * sin(phi)) * sinT + axis * cosT);
}

// Direction from p to a random point of the light (u in [0, 1)^2) and how
// far that point is: the sun's disk (half angle in axis.w), a sphere's
// visible cap, a rect or a disk. Inside a sphere dist is 0: nothing to trace
// (an emissive mesh's light bounds it, and a ray to its center would hit the
// mesh; the shadow maps' near plane leaves the inside lit too).
vec3 SampleLight(Light light, vec3 p, vec2 u, out float dist)
{
    uint type = uint(light.position.w);
    if (type == 0u) {
        dist = 1e30;
        return ConeSample(light.position.xyz, cos(light.axis.w), u);
    }
    vec3 toCenter = light.position.xyz - p;
    float d = length(toCenter);
    if (type == kLightSphere) {
        float r = light.halfWidth.w;  // 0: shaded as a point
        if (r <= 0.0 || d <= r) {
            dist = r > 0.0 ? 0.0 : d;
            return toCenter / max(d, 1e-20);
        }
        vec3 dir = ConeSample(toCenter / d, sqrt(1.0 - r * r / (d * d)), u);
        float along = dot(toCenter, dir);
        dist = along - sqrt(max(along * along - d * d + r * r, 0.0));
        return dir;
    }
    // Cylinders: a point of the axis (their radius is small beside it).
    vec3 q = type == kLightCylinder ? light.position.xyz + (2.0 * u.x - 1.0) * light.halfWidth.xyz
           : type == kLightRect     ? light.position.xyz + (2.0 * u.x - 1.0) * light.halfWidth.xyz +
                                          (2.0 * u.y - 1.0) * light.halfHeight.xyz
                                    : light.position.xyz + sqrt(u.x) * (cos(2.0 * PI * u.y) * light.halfWidth.xyz +
                                                                        sin(2.0 * PI * u.y) * light.halfHeight.xyz);
    vec3 toPoint = q - p;
    dist = length(toPoint);
    return toPoint / max(dist, 1e-20);
}

// Fraction of the light reaching p (normal ng) over `samples` rays (its
// colour through translucent casters and volumes): points u, then R2 steps
// from it, on the light's shape. Only of the part above p's horizon: the
// shading clips the light there already (a sphere light resting on the
// floor, half under it, would be dimmed twice). Points under the horizon
// are skipped for the next R2 points, up to 8 tries; a light wholly under
// it is dark (a bumped surface's terminator).
vec3 LightVisibility(uint index, vec3 p, vec3 ng, vec2 u, uint samples)
{
    Light light = lights[index];
    vec3 origin = OffsetRay(p, ng);
    vec3 sum = vec3(0.0);
    uint taken = 0u;
    for (uint s = 0u; s < samples; ++s) {
        for (uint k = 0u; k < 8u; ++k) {
            float dist;
            vec3 dir = SampleLight(light, p, fract(u + vec2(0.7548776662, 0.5698402910) * float(s + k * samples)), dist);
            if (dist <= 0.0) {  // inside a sphere light
                sum += vec3(1.0);
            } else if (dot(dir, ng) > 0.0) {
                // Short of the light: its own surface (a rect on a ceiling) is not a blocker.
                sum += TraceShadow(origin, dir, dist * 0.999);
            } else {
                continue;
            }
            ++taken;
            break;
        }
    }
    return taken > 0u ? sum / float(taken) : vec3(0.0);
}

// The dome's radiance along dir, blurred to `lod`.
vec3 Dome(vec3 dir, float lod)
{
    if (frame.envRadiance.w <= 0.0) {
        return vec3(0.0);
    }
    return textureLod(envCube, mat3(frame.envWorldToLocal) * dir, lod).rgb * frame.envRadiance.rgb;
}

// The suns' discs along dir: each distant light with an angle shows as a
// disc of radiance irradiance / solid angle (normalize is in the
// irradiance already, hydra/light.cpp), times its specular multiplier.
// Only for rays whose end no light sampling covers (traced refraction):
// reflections, the final gather and the probes leave the sun to
// mesh.frag's and ShadeHit's direct light, or it would count twice.
// Karma's renderlightgeo hides lights from the camera only, so it isn't
// read here. ponytail: hard-edged disc, no limb darkening.
vec3 SunDiscs(vec3 dir)
{
    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < frame.counts.x; ++i) {
        Light light = lights[i];
        float s = sin(0.5 * light.axis.w);  // 1 - cos(half angle) = 2 s^2, exact for a 0.5 degree sun
        if (light.axis.w > 0.0 && dot(dir, light.position.xyz) >= cos(light.axis.w)) {
            sum += light.color.rgb * (light.shaping.w / (4.0 * PI * s * s));
        }
    }
    return BrightnessClamp(sum, frame.lightClamp.x);
}

// The dome's irradiance / pi, unoccluded (as mesh.frag's Irradiance).
vec3 DomeDiffuse(vec3 n)
{
    if (frame.envRadiance.w <= 0.0) {
        return vec3(0.0);
    }
    vec3 d = normalize(mat3(frame.envWorldToLocal) * n);
    vec3 e = frame.envSh[0].rgb * 0.282095 + frame.envSh[1].rgb * 0.488603 * d.y +
             frame.envSh[2].rgb * 0.488603 * d.z + frame.envSh[3].rgb * 0.488603 * d.x +
             frame.envSh[4].rgb * 1.092548 * d.x * d.y + frame.envSh[5].rgb * 1.092548 * d.y * d.z +
             frame.envSh[6].rgb * 0.315392 * (3.0 * d.z * d.z - 1.0) + frame.envSh[7].rgb * 1.092548 * d.x * d.z +
             frame.envSh[8].rgb * 0.546274 * (d.x * d.x - d.y * d.y);
    return max(e, 0.0) * frame.envRadiance.rgb / PI;
}

// Radiance leaving the hit towards the ray's origin: emission, diffuse
// direct light, diffuse indirect light (the probes with GI, else the dome)
// and the dome's reflection for the specular part. The reflected light is
// bounce light, limited by eldr:indirectClamp; the emission is the
// emitter's direct light, limited by eldr:directClamp (off by default):
// through the indirect clamp of 10, emissive.usda's panel of 40 lit its
// room by GI at a third of Karma's. rt.params.w's light is
// shadowed by a ray; with `shadows` (eldr:rtHitShadows) every distant light
// and the first kMaxHitShadows local lights reaching p are too, one ray
// each, the rest unshadowed.
// ponytail: local lights are shaded as points (textured ones by one tap, LightTint) and hits have no specular
// highlights; lights past the cap stay unshadowed (most important first if
// scenes with many lights at one hit show it).
const uint kMaxHitShadows = 16u;
vec3 ShadeHit(vec3 dir, uvec3 seed, bool emission, bool shadows)
{
    vec3 p = hit.position, n = hit.normal;
    vec3 albedo = hit.baseColor * (1.0 - hit.metallic);
    vec3 f0 = mix(vec3(0.04), hit.baseColor, hit.metallic);
    vec3 irradiance = vec3(0.0);  // direct, cosine-weighted
    for (uint i = 0u; i < frame.counts.x; ++i) {
        Light light = lights[i];
        float NoL = dot(n, light.position.xyz);
        if (NoL > 0.0) {
            vec3 shadow = i == rt.params.w || (shadows && (uint(light.color.w) & 2u) != 0u) ? LightVisibility(i, p, hit.geometric, Random2(seed + uvec3(0u, 0u, i * 64u), 0u), 1u) : vec3(1.0);
            irradiance += light.color.rgb * (NoL * light.halfHeight.w * shadow);
        }
    }
    uint traced = 0u;
    for (uint i = frame.counts.x; i < frame.counts.x + frame.localLights.x; ++i) {
        Light light = lights[i];
        vec3 toLight = light.position.xyz - p;
        float d2 = max(dot(toLight, toLight), 1e-12);
        vec3 l = toLight * inversesqrt(d2);
        float NoL = dot(n, l);
        float cosLight = -dot(l, light.axis.xyz);
        uint type = uint(light.position.w);
        if (NoL <= 0.0 || (type != kLightSphere && type != kLightCylinder && cosLight <= 0.0)) {
            continue;
        }
        // Irradiance / radiance: a rect's exact form factor (clipped to the
        // horizon, as mesh.frag); else intensity towards p, radiance times
        // projected area (a cylinder's side: 2r x length, foreshortened
        // along its axis), over d^2, a disk's over d^2 + r^2 (exact on its
        // axis). As points, emissive.usda's panel edges (rects 2 cm tall
        // under the ceiling) lit it 3 cm beside them 8 times over, 10 cm
        // beside 3 times, in every traced bounce.
        float h = length(light.halfWidth.xyz);
        float r2 = length(cross(light.halfWidth.xyz, light.halfHeight.xyz));
        float area = type == kLightSphere   ? PI * light.axis.w * light.axis.w
                   : type == kLightCylinder ? 4.0 * light.axis.w * h * length(cross(light.halfWidth.xyz / max(h, 1e-8), l))
                                            : PI * r2 * cosLight;
        float formFactor = type == kLightRect ? PI * RectFormFactorClipped(toLight, light.halfWidth.xyz, light.halfHeight.xyz, n)
                                              : area * NoL / (type == kLightDisk ? d2 + r2 : d2);
        float window = clamp(1.0 - pow(sqrt(d2) / LightRange(light), 4.0), 0.0, 1.0);
        float theta = acos(clamp(cosLight, -1.0, 1.0));
        float cone = light.shaping.y <= light.shaping.x ? (theta <= light.shaping.y ? 1.0 : 0.0)
                                                         : 1.0 - smoothstep(light.shaping.x, light.shaping.y, theta);
        vec3 e = light.color.rgb * LightTint(light, p) * (formFactor * window * window * cone * light.halfHeight.w);
        if (shadows && traced < kMaxHitShadows && (uint(light.color.w) & 2u) != 0u && max(e.r, max(e.g, e.b)) > 0.0) {
            e *= LightVisibility(i, p, hit.geometric, Random2(seed + uvec3(0u, 0u, i * 64u), 0u), 1u);
            ++traced;
        }
        irradiance += e;
    }
    vec3 indirect = DomeDiffuse(n);
    if ((frame.features.x & kFeatureGi) != 0u) {
        vec4 gi = GiIrradiance(p, n, -dir);
        indirect = mix(indirect, gi.rgb, gi.a);
    }
    if (Feature(kFeatureCaustics) && (draws[hit.slot].caustics & kCausticsReceive) != 0u) {
        indirect += CausticDiffuse(p, n);  // seen in reflections, through glass (a pool's floor), by GI
    }
    vec3 radiance = albedo * (irradiance / PI + indirect) + f0 * Dome(reflect(dir, n), 0.5 * float(frame.counts.z));
    radiance = BrightnessClamp(radiance, frame.lightClamp.y);
    return emission ? radiance + BrightnessClamp(hit.emissive, frame.lightClamp.y) : radiance;
}
