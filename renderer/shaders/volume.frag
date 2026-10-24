#version 450
#extension GL_GOOGLE_include_directive : require

// Heterogeneous volumes (NanoVDB grids) raymarched per pixel over the shaded
// image, nearest volume first, up to the opaque depth. Single scattering:
// the distant lights through a shadow march across the grid (and the sun's
// cascades for opaque occluders), sphere/rect/disk lights from the
// clusters (unshadowed), the dome's SH convolved with the phase function
// and attenuated by the optical depth up towards the dome's pole;
// blackbody emission from temperature, emitted radiance from emission and
// the material's per unit of density. The medium is the volume's material
// (VolumeShading: scattering and absorption per unit of density, phase,
// emission), the settings multiply it. Henyey-Greenstein phase. At scale 2
// each invocation marches a 2 x 2 block's middle into a half resolution
// target (volume_upsample.frag). Multiple scattering of the distant lights is
// approximated by octaves of ever weaker, less attenuated and less
// directional single scattering (Wrenninge 2015). Each pixel starts at a
// different fraction of a step every frame, which TAA averages. Steps skip
// tiles (and the background) of zero density.
// ponytail: volumes are composited one after another along the ray (in
// entry order, not interleaved where they overlap) and don't shadow each
// other (surfaces only from the sun: volume_shadow.frag); local lights are unshadowed inside them, the
// dome's light only comes down from above (half its optical depth, as
// light also arrives from the sides).

#include "frame.glsl"
#include "lights.glsl"
#include "fog.glsl"

layout(set = 0, binding = 4) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 63) uniform sampler2D sceneDepth;  // window [0, 1], prepass or not
#include "volume.glsl"
// The nearest translucent surfaces' window depth (VolumePass::Part).
layout(set = 0, binding = 67) uniform sampler2D translucentDepth;

layout(push_constant) uniform Params {
    float stepSize;     // density voxels per step
    uint maxSteps;      // per pixel
    uint shadowSteps;
    float density;      // extinction multiplier
    float emission;     // temperature and emission multiplier
    float anisotropy;   // Henyey-Greenstein g of volumes whose material has none
    uint count;         // volumes[]
    uint scale;         // 1: every pixel; 2: half resolution, each pixel a 2 x 2 block
    uint part;          // VolumePass::Part: 0 all, 1 behind the nearest translucent surface, 2 in front
} params;

layout(location = 0) out vec4 outColor;  // premultiplied: radiance, 1 - transmittance

const uint kMaxVolumes = 8u;     // per pixel, the nearest
const float kOpaque = 0.01;      // transmittance at which a ray stops

// Where on the frame this invocation's ray goes, in full resolution pixels
// (at half resolution the middle of its 2 x 2 block).
vec2 fragCoord;

vec3 Unproject(vec3 ndc)
{
    vec4 p = frame.invViewProj * vec4(ndc, 1.0);
    return p.xyz / p.w;
}

// Henyey-Greenstein: cosTheta between the light's and the eye ray's
// directions of travel.
float Phase(float cosTheta, float g)
{
    float d = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 - g * g) / (4.0 * PI * d * sqrt(d));
}

// The dome's radiance scattered towards the eye along -d: its SH (stored
// as irradiance, so divided by the cosine lobe's band factors) convolved
// with the phase function's (g^l per band).
vec3 DomeScatter(vec3 d, float g)
{
    vec3 n = normalize(mat3(frame.envWorldToLocal) * d);
    float b1 = g * 3.0 / (2.0 * PI), b2 = g * g * 4.0 / PI;
    vec3 sh = frame.envSh[0].rgb * 0.282095 / PI
            + b1 * 0.488603 * (frame.envSh[1].rgb * n.y + frame.envSh[2].rgb * n.z + frame.envSh[3].rgb * n.x)
            + b2 * (frame.envSh[4].rgb * 1.092548 * n.x * n.y + frame.envSh[5].rgb * 1.092548 * n.y * n.z
                    + frame.envSh[6].rgb * 0.315392 * (3.0 * n.z * n.z - 1.0)
                    + frame.envSh[7].rgb * 1.092548 * n.x * n.z
                    + frame.envSh[8].rgb * 0.546274 * (n.x * n.x - n.y * n.y));
    return max(sh, 0.0) * frame.envRadiance.rgb;
}

// Planckian locus (Krystek 1985, 1000..15000 K) in linear Rec.709 with
// luminance 1.
vec3 Blackbody(float kelvin)
{
    float k = clamp(kelvin, 1000.0, 15000.0), k2 = k * k;
    float u = (0.860117757 + 1.54118254e-4 * k + 1.28641212e-7 * k2) / (1.0 + 8.42420235e-4 * k + 7.08145163e-7 * k2);
    float v = (0.317398726 + 4.22806245e-5 * k + 4.20481691e-8 * k2) / (1.0 - 2.89741816e-5 * k + 1.61456053e-7 * k2);
    float x = 3.0 * u / (2.0 * u - 8.0 * v + 4.0), y = 2.0 * v / (2.0 * u - 8.0 * v + 4.0);
    vec3 xyz = vec3(x / y, 1.0, (1.0 - x - y) / y);
    return max(mat3(3.2404542, -0.9692660, 0.0556434, -1.5371385, 1.8760108, -0.2040259,
                    -0.4985314, 0.0415560, 1.0572252) * xyz, 0.0);
}

// The sun's cascades at a point in the medium: one PCF tap, TAA's jitter
// smooths it.
float SunShadow(vec3 x, vec3 l)
{
    float depth = dot(x - frame.cameraPos.xyz, frame.cameraForward.xyz);
    int c = 0;
    while (c < 4 && depth > frame.cascadeSplits[c]) {
        ++c;
    }
    if (c == 4) {
        return 1.0;
    }
    vec4 clip = frame.cascadeViewProj[c] * vec4(x + l * frame.cascadeTexel[c] * 2.0, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    return texture(shadowMap, vec4(ndc.xy * 0.5 + 0.5, float(c), ndc.z * 0.5 + 0.5));
}

// Light of a sphere/rect/disk light arriving at x, as from a point (its
// solid angle times its radiance), with the cone and focus shaping of
// LocalLight in mesh.frag; l is the direction towards it.
vec3 LocalIrradiance(uint index, vec3 x, out vec3 l)
{
    Light light = lights[index];
    vec3 toLight = light.position.xyz - x;
    float d = length(toLight);
    l = toLight / max(d, 1e-8);
    float window = clamp(1.0 - pow(d / LightRange(light), 4.0), 0.0, 1.0);
    if (window <= 0.0 || d <= 0.0) {
        return vec3(0.0);
    }
    vec3 radiance = light.color.rgb * window * window * light.halfHeight.w;
    float cosOff = dot(-l, light.axis.xyz);
    if (light.shaping.z > 0.0) {
        radiance *= mix(light.focusTint.rgb, vec3(1.0), pow(abs(cosOff), light.shaping.z));
    }
    float theta = acos(clamp(cosOff, -1.0, 1.0));
    radiance *= light.shaping.y <= light.shaping.x ? (theta <= light.shaping.y ? 1.0 : 0.0)
                                                   : 1.0 - smoothstep(light.shaping.x, light.shaping.y, theta);
    if (uint(light.position.w) == kLightSphere) {
        float r2 = light.axis.w * light.axis.w;
        return radiance * PI * r2 / max(d * d, r2);
    }
    if (cosOff <= 0.0) {
        return vec3(0.0);  // behind the emitting side
    }
    float area = (uint(light.position.w) == kLightRect ? 4.0 : PI) *
                 length(cross(light.halfWidth.xyz, light.halfHeight.xyz));
    return radiance * area * cosOff / max(d * d, area);
}

// A float or Vec3f grid's value (floats splat to grey).
vec3 ReadRgb(uint type, inout pnanovdb_readaccessor_t acc, ivec3 ijk)
{
    pnanovdb_address_t a = pnanovdb_readaccessor_get_value_address(type, buf, acc, ijk);
    if (type == PNANOVDB_GRID_TYPE_VEC3F) {
        return vec3(pnanovdb_read_float(buf, a), pnanovdb_read_float(buf, pnanovdb_address_offset(a, 4u)),
                    pnanovdb_read_float(buf, pnanovdb_address_offset(a, 8u)));
    }
    return vec3(pnanovdb_read_float(buf, a));
}

// Density at index-space p (voxel values sit on integer coordinates),
// trilinear on average: one of the eight voxels around p, each as likely as
// its trilinear weight, picked by xi in [0, 1)^3 (TAA averages the rest).
float Density(inout pnanovdb_readaccessor_t acc, vec3 p, vec3 xi)
{
    vec3 base = floor(p);
    ivec3 ijk = ivec3(base) + ivec3(lessThan(xi, p - base));
    return max(ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, acc, ijk), 0.0);
}

// Optical depth from density index p towards a distant light (index-space
// direction dl per world unit) to the edge of the box, in `steps` steps.
float ShadowMarch(inout pnanovdb_readaccessor_t acc, vec3 p, vec3 dl, vec3 lo, vec3 hi, float noise, uint steps,
                  float extinction)
{
    float far = Slabs(p, dl, lo, hi).y;
    if (!(far > 0.0) || steps == 0u) {
        return 0.0;
    }
    float dt = far / float(steps), tau = 0.0;
    for (uint k = 0u; k < steps; ++k) {
        ivec3 ijk = ivec3(floor(p + dl * ((float(k) + noise) * dt) + 0.5));
        tau += max(ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, acc, ijk), 0.0);
    }
    return tau * dt * extinction;
}

// Marches volume i over [t0, t1] of the world ray, adding to radiance and
// attenuating transmittance; steps come out of stepsLeft.
void March(uint i, vec3 origin, vec3 dir, float t0, float t1, float noise, vec3 up, inout uint stepsLeft,
           inout vec3 radiance, inout float transmittance)
{
    vec4 w0 = volumes[i].worldToIndex[0], w1 = volumes[i].worldToIndex[1], w2 = volumes[i].worldToIndex[2];
    vec3 lo = volumes[i].indexMin.xyz, hi = volumes[i].indexMax.xyz;
    uvec4 grids = volumes[i].grids, types = volumes[i].types;
    vec3 oi = Row3(w0, w1, w2, origin, 1.0), di = Row3(w0, w1, w2, dir, 0.0);
    vec3 safe = SafeDir(di);
    // The material (or the defaults): extinction per unit of density, the
    // phase function, the dome's light scattered with it.
    float extinction = volumes[i].medium.x * params.density;
    float g = volumes[i].medium.y > 0.0 ? volumes[i].albedo.w : params.anisotropy;
    vec3 dome = frame.envRadiance.w > 0.0 ? DomeScatter(dir, g) : vec3(0.0);

    // Steps of stepSize voxels; a box that would need more than maxSteps
    // gets longer ones.
    float dt = max(params.stepSize / length(di), (t1 - t0) / float(max(params.maxSteps, 1u)));
    pnanovdb_readaccessor_t density = Accessor(grids.x), shadow = Accessor(grids.x);
    // Absent grids (never read) get the density's.
    pnanovdb_readaccessor_t temperature = Accessor(grids.y != kNone ? grids.y : grids.x);
    pnanovdb_readaccessor_t color = Accessor(grids.z != kNone ? grids.z : grids.x);
    pnanovdb_readaccessor_t emission = Accessor(grids.w != kNone ? grids.w : grids.x);
    float t = t0 + noise * dt;
    while (t < t1 && stepsLeft > 0u && transmittance > kOpaque) {
        --stepsLeft;
        vec3 p = oi + t * di;
        ivec3 ijk = ivec3(floor(p));
        uint level;
        pnanovdb_address_t a = pnanovdb_readaccessor_get_value_address_and_level(PNANOVDB_GRID_TYPE_FLOAT, buf, density,
                                                                                 ijk, level);
        if (level > 0u && pnanovdb_read_float(buf, a) <= 0.0) {
            // An empty tile (or the background): on to the first step past
            // its far side, keeping this pixel's step offset.
            float dim = level == 1u ? 8.0 : level == 2u ? 128.0 : 4096.0;
            vec3 cell = floor(p / dim) * dim + step(0.0, di) * dim;
            vec3 exits = (cell - oi) / safe;
            float exit = min(min(exits.x, exits.y), exits.z);
            t = max(t + dt, t0 + (ceil((exit - t0) / dt - noise) + noise) * dt);
            continue;
        }
        float rho = Density(density, p, fract(noise + vec3(0.8191725, 0.6710436, 0.5497005) * float(stepsLeft)));
        float sigma = rho * extinction;
        vec3 x = origin + t * dir;
        vec3 emitted = vec3(0.0);
        if (grids.y != kNone) {
            vec3 q = Row3(volumes[i].toGrid[0], volumes[i].toGrid[1], volumes[i].toGrid[2], p, 1.0);
            float kelvin = ReadFloat(types.y, temperature, ivec3(floor(q + 0.5))) * volumes[i].params.x;
            // Physical: brighter as T^4. MaterialX's blackbody is luminance 1
            // down to 800 K's colour; it fades out below that here (the
            // Draper point: nothing visible glows colder).
            float physical = volumes[i].params.z > 0.0 ? pow(max(kelvin, 0.0) / 1500.0, 4.0)
                                                       : smoothstep(500.0, 900.0, kelvin);
            emitted += Blackbody(kelvin) * volumes[i].params.y * physical;
        }
        if (grids.w != kNone) {
            vec3 q = Row3(volumes[i].toGrid[6], volumes[i].toGrid[7], volumes[i].toGrid[8], p, 1.0);
            emitted += max(ReadRgb(types.w, emission, ivec3(floor(q + 0.5))), 0.0) * volumes[i].medium.w;
        }
        emitted += volumes[i].emission.rgb * rho;
        radiance += transmittance * emitted * params.emission * dt;
        if (sigma > 0.0) {
            vec3 albedo = volumes[i].albedo.rgb;
            if (grids.z != kNone) {
                vec3 q = Row3(volumes[i].toGrid[3], volumes[i].toGrid[4], volumes[i].toGrid[5], p, 1.0);
                albedo *= clamp(ReadRgb(types.z, color, ivec3(floor(q + 0.5))), 0.0, 1.0);
            }
            vec3 light = vec3(0.0);
            if (frame.envRadiance.w > 0.0) {
                float depth = ShadowMarch(shadow, p, Row3(w0, w1, w2, up, 0.0), lo, hi, noise,
                                          (params.shadowSteps + 1u) / 2u, extinction);
                light = dome * exp(-0.5 * depth);
            }
            for (uint k = 0u; k < frame.counts.x; ++k) {
                Light sun = lights[k];
                vec3 l = sun.position.xyz;
                float depth = ShadowMarch(shadow, p, Row3(w0, w1, w2, l, 0.0), lo, hi, noise, params.shadowSteps,
                                          extinction);
                // Octave i: light, optical depth and anisotropy times 1/2^i.
                float scattered = 0.0;
                for (float a = 1.0; a > 0.2; a *= 0.5) {
                    scattered += a * exp(-depth * a) * Phase(dot(dir, l), g * a);
                }
                if (k == frame.counts.y) {
                    scattered *= SunShadow(x, l);
                }
                light += sun.color.rgb * sun.halfHeight.w * scattered;
            }
            if (frame.localLights.x > 0u) {
                vec3 l;
                if ((frame.features.x & kFeatureClusters) != 0u) {
                    float viewDepth = dot(x - frame.cameraPos.xyz, frame.cameraForward.xyz);
                    uint base = ClusterIndex(fragCoord, viewDepth) * (kMaxClusterLights + 1u);
                    for (uint k = 0u; k < clusterLights[base]; ++k) {
                        vec3 e = LocalIrradiance(clusterLights[base + 1u + k], x, l);
                        light += e * Phase(dot(dir, l), g);
                    }
                } else {
                    for (uint k = frame.counts.x; k < frame.counts.x + frame.localLights.x; ++k) {
                        vec3 e = LocalIrradiance(k, x, l);
                        light += e * Phase(dot(dir, l), g);
                    }
                }
            }
            // In-scattering integrated over the step (Hillaire 2015).
            float stepT = exp(-sigma * dt);
            radiance += transmittance * albedo * light * (1.0 - stepT);
            transmittance *= stepT;
        }
        t += dt;
    }
}

void main()
{
    // Row 0 is NDC y = -1, as in sky.frag; the rays follow the TAA jitter.
    // At half resolution the ray through the middle of a 2 x 2 block stops
    // at the farthest of its depths (volume_upsample.frag weighs that).
    float depth;
    if (params.scale == 1u) {
        fragCoord = gl_FragCoord.xy;
        depth = texelFetch(sceneDepth, ivec2(fragCoord), 0).r;
    } else {
        ivec2 block = ivec2(gl_FragCoord.xy) * 2, last = textureSize(sceneDepth, 0) - 1;
        fragCoord = vec2(block) + 1.0;
        depth = max(max(texelFetch(sceneDepth, min(block, last), 0).r,
                        texelFetch(sceneDepth, min(block + ivec2(1, 0), last), 0).r),
                    max(texelFetch(sceneDepth, min(block + ivec2(0, 1), last), 0).r,
                        texelFetch(sceneDepth, min(block + ivec2(1, 1), last), 0).r));
    }
    vec2 ndc = fragCoord / frame.viewport.xy * 2.0 - 1.0;
    vec3 origin = Unproject(vec3(ndc, -1.0));
    vec3 dir = normalize(Unproject(vec3(ndc, 1.0)) - origin);
    float tEnd = depth < 1.0 ? length(Unproject(vec3(ndc, depth * 2.0 - 1.0)) - origin) : 3.0e38;
    // Under transparency: the translucency pass composites the part behind
    // the nearest translucent surface (marched before it) and the part in
    // front (after it) goes over that. Without a translucent surface nearer
    // than the opaque one, all of the ray is behind.
    float tStart = 0.0;
    if (params.part != 0u) {
        float glass;
        if (params.scale == 1u) {
            glass = texelFetch(translucentDepth, ivec2(fragCoord), 0).r;
        } else {
            ivec2 block = ivec2(gl_FragCoord.xy) * 2, last = textureSize(translucentDepth, 0) - 1;
            glass = min(min(texelFetch(translucentDepth, min(block, last), 0).r,
                            texelFetch(translucentDepth, min(block + ivec2(1, 0), last), 0).r),
                        min(texelFetch(translucentDepth, min(block + ivec2(0, 1), last), 0).r,
                            texelFetch(translucentDepth, min(block + ivec2(1, 1), last), 0).r));
        }
        float split = glass < depth ? length(Unproject(vec3(ndc, glass * 2.0 - 1.0)) - origin) : 0.0;
        if (params.part == 1u) {
            tStart = split;
        } else {
            tEnd = split;
        }
    }

    // The volumes this ray crosses before the opaque surface, nearest first.
    uint ids[kMaxVolumes];
    vec2 spans[kMaxVolumes];
    uint n = 0u;
    for (uint i = 0u; i < params.count; ++i) {
        vec4 w0 = volumes[i].worldToIndex[0], w1 = volumes[i].worldToIndex[1], w2 = volumes[i].worldToIndex[2];
        vec2 span = Slabs(Row3(w0, w1, w2, origin, 1.0), Row3(w0, w1, w2, dir, 0.0), volumes[i].indexMin.xyz,
                          volumes[i].indexMax.xyz);
        span.x = max(span.x, tStart);
        span.y = min(span.y, tEnd);
        if (!(span.x < span.y) || (n == kMaxVolumes && span.x >= spans[n - 1u].x)) {
            continue;
        }
        uint k = min(n, kMaxVolumes - 1u);
        for (; k > 0u && spans[k - 1u].x > span.x; --k) {
            ids[k] = ids[k - 1u];
            spans[k] = spans[k - 1u];
        }
        ids[k] = i;
        spans[k] = span;
        n = min(n + 1u, kMaxVolumes);
    }
    if (n == 0u) {
        if (params.scale == 1u) {
            discard;  // blending nothing over the image
        }
        outColor = vec4(0.0);  // the half resolution target isn't cleared
        return;
    }

    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy + 5.588238 * float(frame.localLights.y),
                                               vec2(0.06711056, 0.00583715))));
    // The dome's pole (its latlong's +y) in world space.
    mat3 toDome = mat3(frame.envWorldToLocal);
    vec3 up = normalize(vec3(toDome[0][1], toDome[1][1], toDome[2][1]));
    vec3 radiance = vec3(0.0);
    float transmittance = 1.0;
    uint stepsLeft = params.maxSteps;
    for (uint k = 0u; k < n && transmittance > kOpaque; ++k) {
        vec3 before = radiance;
        float transmittanceBefore = transmittance;
        March(ids[k], origin, dir, spans[k].x, spans[k].y, noise, up, stepsLeft, radiance, transmittance);
        // Volumetric fog (fog.glsl): the image below carries it all the way
        // to the surface behind. The volume's light reaches the eye through
        // the fog in front of it, and that fog's own light isn't hidden by
        // the volume: add what the blend's attenuation takes from it.
        // ponytail: fog inside a volume's box counts as behind it.
        vec4 fog = Fog(fragCoord, dot(origin + spans[k].x * dir - frame.cameraPos.xyz, frame.cameraForward.xyz));
        radiance = before + (radiance - before) * fog.a + (transmittanceBefore - transmittance) * fog.rgb;
    }
    outColor = vec4(radiance, 1.0 - transmittance);
}
