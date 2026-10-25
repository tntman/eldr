// The ray generation shaders' tracing and shading (rt_pixels.rgen,
// rt_probes.rgen). Include after frame.glsl, lights.glsl, scene.glsl,
// material.glsl, rt.glsl and gi.glsl.

layout(set = 0, binding = 5) uniform samplerCube envCube;

layout(location = 0) rayPayloadEXT RtHit hit;
layout(location = 1) rayPayloadEXT float unoccluded;

// Closest surface along a ray, cone: its spread angle in radians.
void Trace(vec3 origin, vec3 dir, float cone)
{
    hit.t = cone;
    traceRayEXT(tlas, gl_RayFlagsNoneEXT, 0xFF, 0, 0, 0, origin, 0.0, dir, 1e30, 0);
}

// 1 if nothing is in the way up to tMax.
float TraceShadow(vec3 origin, vec3 dir, float tMax)
{
    unoccluded = 0.0;
    traceRayEXT(tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT, 0xFF, 1, 0, 1,
                origin, 0.0, dir, tMax, 1);
    return unoccluded;
}

// Ray origin just off a surface on the side of normal ng, robust at any
// scale (Waechter and Binder, Ray Tracing Gems ch. 6).
vec3 OffsetRay(vec3 p, vec3 ng)
{
    ivec3 o = ivec3(256.0 * ng);
    vec3 pi = intBitsToFloat(floatBitsToInt(p) + ivec3(p.x < 0.0 ? -o.x : o.x, p.y < 0.0 ? -o.y : o.y,
                                                       p.z < 0.0 ? -o.z : o.z));
    return vec3(abs(p.x) < 1.0 / 32.0 ? p.x + ng.x / 65536.0 : pi.x,
                abs(p.y) < 1.0 / 32.0 ? p.y + ng.y / 65536.0 : pi.y,
                abs(p.z) < 1.0 / 32.0 ? p.z + ng.z / 65536.0 : pi.z);
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
// visible cap, a rect or a disk.
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
            dist = d;
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

// Fraction of the light visible from p (normal ng) over `samples` rays:
// points u, then R2 steps from it, on the light's shape.
float LightVisibility(uint index, vec3 p, vec3 ng, vec2 u, uint samples)
{
    Light light = lights[index];
    vec3 origin = OffsetRay(p, ng);
    float sum = 0.0;
    for (uint s = 0u; s < samples; ++s) {
        float dist;
        vec3 dir = SampleLight(light, p, fract(u + vec2(0.7548776662, 0.5698402910) * float(s)), dist);
        // Short of the light: its own surface (a rect on a ceiling) is not a blocker.
        sum += dot(dir, ng) > 0.0 ? TraceShadow(origin, dir, dist * 0.999) : 0.0;
    }
    return sum / float(samples);
}

// The dome's radiance along dir, blurred to `lod`.
vec3 Dome(vec3 dir, float lod)
{
    if (frame.envRadiance.w <= 0.0) {
        return vec3(0.0);
    }
    return textureLod(envCube, mat3(frame.envWorldToLocal) * dir, lod).rgb * frame.envRadiance.rgb;
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
// direct light (rt.params.w's light shadowed by a ray, the rest not),
// diffuse indirect light (the probes with GI, else the dome) and the
// dome's reflection for the specular part.
// ponytail: local lights are unshadowed at hits and shaded as points, and
// hits have no specular highlights.
vec3 ShadeHit(vec3 dir, uvec3 seed, bool emission)
{
    vec3 p = hit.position, n = hit.normal;
    vec3 albedo = hit.baseColor * (1.0 - hit.metallic);
    vec3 f0 = mix(vec3(0.04), hit.baseColor, hit.metallic);
    vec3 irradiance = vec3(0.0);  // direct, cosine-weighted
    for (uint i = 0u; i < frame.counts.x; ++i) {
        Light light = lights[i];
        float NoL = dot(n, light.position.xyz);
        if (NoL > 0.0) {
            float shadow = i == rt.params.w ? LightVisibility(i, p, hit.geometric, Random2(seed + uvec3(0u, 0u, i * 64u), 0u), 1u) : 1.0;
            irradiance += light.color.rgb * (NoL * light.halfHeight.w * shadow);
        }
    }
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
        // Intensity towards p: radiance times projected area (a cylinder's
        // side: 2r x length, foreshortened along its axis).
        float h = length(light.halfWidth.xyz);
        float area = type == kLightSphere   ? PI * light.axis.w * light.axis.w
                   : type == kLightCylinder ? 4.0 * light.axis.w * h * length(cross(light.halfWidth.xyz / max(h, 1e-8), l))
                                            : (type == kLightRect ? 4.0 : PI) *
                                                  length(cross(light.halfWidth.xyz, light.halfHeight.xyz)) * cosLight;
        float window = clamp(1.0 - pow(sqrt(d2) / LightRange(light), 4.0), 0.0, 1.0);
        float theta = acos(clamp(cosLight, -1.0, 1.0));
        float cone = light.shaping.y <= light.shaping.x ? (theta <= light.shaping.y ? 1.0 : 0.0)
                                                         : 1.0 - smoothstep(light.shaping.x, light.shaping.y, theta);
        irradiance += light.color.rgb * (area * NoL / d2 * window * window * cone * light.halfHeight.w);
    }
    vec3 indirect = DomeDiffuse(n);
    if ((frame.features.x & kFeatureGi) != 0u) {
        vec4 gi = GiIrradiance(p, n, -dir);
        indirect = mix(indirect, gi.rgb, gi.a);
    }
    vec3 radiance = albedo * (irradiance / PI + indirect) + f0 * Dome(reflect(dir, n), 0.5 * float(frame.counts.z));
    return emission ? radiance + hit.emissive : radiance;
}
