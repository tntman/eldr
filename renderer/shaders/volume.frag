#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

// Heterogeneous volumes (NanoVDB grids) raymarched per pixel over the shaded
// image, nearest volume first, up to the opaque depth. Single scattering
// (as Karma's default volume limit 0):
// the distant lights through a shadow march across the grid (and the sun's
// cascades for opaque occluders), sphere/rect/disk lights from the
// clusters (through their atlas tiles), the dome's SH convolved with the phase function
// and attenuated by the optical depth up towards the dome's pole;
// blackbody emission from temperature, emitted radiance from emission and
// the material's per unit of density. The medium is the volume's material
// (VolumeShading: scattering and absorption per unit of density, phase,
// emission), the settings multiply it. Henyey-Greenstein phase. At scale 2
// each invocation marches a 2 x 2 block's middle into a half resolution
// target (volume_upsample.frag). With params.octaves above 1, multiple
// scattering of the distant lights and the dome is approximated by octaves
// of ever weaker and less attenuated single scattering (Wrenninge 2015),
// fitted to Karma's Volume Limit (VolumePass::Settings::volumeLimit). Each pixel starts at a
// different fraction of a step every frame, which TAA averages. Steps skip
// tiles (and the background) of zero density.
// Volumes shadow each other (params.mutual): the light towards each step
// also marches the other volumes' boxes on its way (OthersDepth).
// ponytail: volumes are composited one after another along the ray (in
// entry order, not interleaved where they overlap); local lights get one atlas tap per step, the
// dome's light only comes down from above (half its optical depth, as
// light also arrives from the sides), through the other volumes too.

#include "frame.glsl"
#include "lights.glsl"
#include "fog.glsl"

layout(set = 0, binding = 4) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 63) uniform sampler2D sceneDepth;  // window [0, 1], prepass or not
#include "volume.glsl"
// The camera's translucent layers, nearest first (VolumePass::Part): the
// colour each lets through, its distance along the ray (a < 0: none).
layout(set = 0, binding = 77) uniform sampler2DArray translucentLayers;
// Local lights' shadow atlas, as in mesh.frag.
const float kSpotShadowCone = 1.3962634;
struct ShadowTile {
    mat4 viewProj;
    vec4 rect;
    vec4 params;
    vec4 origin;
};
layout(set = 0, binding = 17) uniform sampler2DShadow shadowAtlas;
layout(std430, set = 0, binding = 18) readonly buffer LightShadows { uint lightShadow[]; };
layout(std430, set = 0, binding = 19) readonly buffer ShadowTiles { ShadowTile shadowTiles[]; };

// The light splits (aov.h) behind a volume are seen through it, as in
// Karma's LPEs; its light is layer 9's. As volume_upsample.frag's.
layout(set = 0, binding = 67, rgba16f) uniform image2DArray lightSplits;
void VolumeSplits(vec3 radiance, vec3 transmittance)
{
    int layers = imageSize(lightSplits).z;
    if (frame.aovs.x == 0u || layers <= 4) {
        return;
    }
    ivec2 q = ivec2(gl_FragCoord.xy);
    for (int i = 0; i < layers; ++i) {
        vec4 s = imageLoad(lightSplits, ivec3(q, i));
        imageStore(lightSplits, ivec3(q, i), vec4(s.rgb * transmittance + (i == 9 ? radiance : vec3(0.0)), s.a));
    }
}

layout(push_constant) uniform Params {
    float stepSize;     // density voxels per step
    uint maxSteps;      // per pixel
    uint shadowSteps;
    float density;      // extinction multiplier
    float emission;     // temperature and emission multiplier
    float anisotropy;   // Henyey-Greenstein g of volumes whose material has none
    uint count;         // volumes[]
    uint scale;         // 1: every pixel; 2: half resolution, each pixel a 2 x 2 block
    uint part;          // VolumePass::Part: 0 all, 1 behind the translucent layers, 2 in front, 3 all through them
    uint cascade, mapSize, originX, originY;  // volume_shadow.frag's
    uint mutual;        // 1: volumes shadow each other
    uint defaultFields; // 1: volumes without a material read temperature, colour and emission fields
    uint octaves;       // multiple scattering: octaves of distant lights and the dome (1: single scattering)
    float octaveLight, octaveDepth;  // each octave's light and optical depth times the last's
} params;

#ifdef HALF
// Half resolution (built with HALF, scale 2): radiance and the coloured
// transmittance as six half floats, volume_upsample.frag blends them.
layout(location = 0) out uvec4 outPacked;
#else
// Premultiplied: radiance, 1 - the mean transmittance; the image is
// multiplied by the coloured transmittance (dual-source blend).
layout(location = 0, index = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outTransmittance;
#endif

void Output(vec3 radiance, vec3 transmittance)
{
#ifdef HALF
    outPacked = uvec4(packHalf2x16(radiance.rg), packHalf2x16(vec2(radiance.b, transmittance.r)),
                      packHalf2x16(transmittance.gb), 0u);
#else
    float mean = dot(transmittance, vec3(1.0 / 3.0));
    outColor = vec4(radiance, 1.0 - mean);
    outTransmittance = vec4(transmittance, mean);
#endif
}

const uint kMaxVolumes = 8u;     // per pixel, the nearest
// Volumes read factor tables (VolumePass::Record's `factors`); without, the
// march keeps the registers they'd take.
layout(constant_id = 0) const bool kFactors = true;
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

// A local light's atlas tiles at a point in the medium, as fog_inject.comp
// looks them up: one tap, TAA's jitter smooths it. Traced lights keep tiles
// while volumes are in the scene (Renderer::_PlanLocalShadows).
float LocalShadow(uint index, vec3 x)
{
    uint tile = lightShadow[index];
    if (tile == kNone) {
        return 1.0;
    }
    vec3 fromLight = x - shadowTiles[tile].origin.xyz;
    if (lights[index].shaping.y > kSpotShadowCone) {
        vec3 a = abs(fromLight);
        tile += a.x >= a.y && a.x >= a.z ? (fromLight.x > 0.0 ? 0u : 1u)
              : a.y >= a.z               ? (fromLight.y > 0.0 ? 2u : 3u)
                                         : (fromLight.z > 0.0 ? 4u : 5u);
    }
    ShadowTile t = shadowTiles[tile];
    float dist = length(fromLight);
    vec4 clip = t.viewProj * vec4(x - fromLight / max(dist, 1e-8) * (t.params.x * dist * 2.0), 1.0);
    vec3 ndc = clip.xyz / clip.w;
    if (clip.w <= 0.0 || any(greaterThan(abs(ndc.xy), vec2(1.0)))) {
        return 1.0;  // outside a spot's tile: its cone doesn't light it anyway
    }
    return texture(shadowAtlas, vec3(t.rect.xy + (ndc.xy * 0.5 + 0.5) * t.rect.zw, ndc.z * 0.5 + 0.5));
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
    vec3 radiance = light.color.rgb * LightTint(light, x) * window * window * light.halfHeight.w;
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

float Max3(vec3 v)
{
    return max(max(v.x, v.y), v.z);
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

// The density (without a table) or the extinction table's value
// (ExtinctionTable) integrated from density index p towards a distant
// light (index-space direction dl per world unit) to the edge of the box,
// in `steps` steps.
float ShadowMarch(uint table, float low, float scale, inout pnanovdb_readaccessor_t acc, vec3 p, vec3 dl, vec3 lo,
                  vec3 hi, float noise, uint steps)
{
    float far = Slabs(p, dl, lo, hi).y;
    if (!(far > 0.0) || steps == 0u) {
        return 0.0;
    }
    float dt = far / float(steps), tau = 0.0;
    for (uint k = 0u; k < steps; ++k) {
        ivec3 ijk = ivec3(floor(p + dl * ((float(k) + noise) * dt) + 0.5));
        float rho = max(ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, acc, ijk), 0.0);
        tau += table == kNone ? rho : ExtinctionTable(table, low, scale, rho);
    }
    return tau * dt;
}

// Optical depth per channel of the volumes other than `self` along
// x + dir * s, s in [0, 1] (as VolumeOpticalDepth): shadowSteps steps across each box crossed, offset by noise. The
// self march (ShadowMarch) covers the volume's own density, so it is left
// out here. No tint maps: they hold all volumes' transmittance together,
// the lit volume's own included, with one depth per texel, so they can't
// tell another volume in front from this one.
// Steps landing in an empty tile go on to the first step past it; the
// march stops once nothing is left to dim (kShadowOpaque).
// ponytail: fixed steps per box; cost grows with the volumes crossed.
vec3 OthersDepth(uint self, vec3 x, vec3 dir, float noise)
{
    vec3 tau = vec3(0.0);
    for (uint j = 0u; j < params.count && min(min(tau.x, tau.y), tau.z) < kShadowOpaque; ++j) {
        if (j == self) {
            continue;
        }
        vec4 w0 = volumes[j].worldToIndex[0], w1 = volumes[j].worldToIndex[1], w2 = volumes[j].worldToIndex[2];
        vec3 oi = Row3(w0, w1, w2, x, 1.0), di = Row3(w0, w1, w2, dir, 0.0);
        vec2 span = Slabs(oi, di, volumes[j].indexMin.xyz, volumes[j].indexMax.xyz);
        span = vec2(max(span.x, 0.0), min(span.y, 1.0));
        if (!(span.x < span.y)) {
            continue;
        }
        float ds = (span.y - span.x) / float(params.shadowSteps);
        uint table = kFactors ? volumes[j].lutOffsets.w : kNone;
        float low = volumes[j].lutLow.w, tableScale = volumes[j].lutScale.w;
        vec3 scale = ds * length(dir) * params.density *
                     (table == kNone ? volumes[j].scattering.rgb + volumes[j].absorption.rgb : vec3(1.0));
        vec3 safe = SafeDir(di);
        pnanovdb_readaccessor_t acc = Accessor(volumes[j].grids.x);
        float k = 0.0;
        while (k < float(params.shadowSteps) && min(min(tau.x, tau.y), tau.z) < kShadowOpaque) {
            vec3 q = oi + di * (span.x + (k + noise) * ds) + 0.5;  // nearest voxel: floor(q)
            uint level;
            pnanovdb_address_t a = pnanovdb_readaccessor_get_value_address_and_level(PNANOVDB_GRID_TYPE_FLOAT, buf, acc,
                                                                                     ivec3(floor(q)), level);
            float v = pnanovdb_read_float(buf, a);
            if (level > 0u && v <= 0.0) {
                // An empty tile (or the background): on to the first step past its far side.
                float dim = level == 1u ? 8.0 : level == 2u ? 128.0 : 4096.0;
                vec3 exits = (floor(q / dim) * dim + step(0.0, di) * dim - 0.5 - oi) / safe;
                k = max(k + 1.0, ceil((min(min(exits.x, exits.y), exits.z) - span.x) / ds - noise));
                continue;
            }
            tau += (table == kNone ? vec3(max(v, 0.0)) : ExtinctionTableRgb(table, low, tableScale, max(v, 0.0))) * scale;
            k += 1.0;
        }
    }
    return tau;
}

// Volume i's factor k at density index p (rho: the density there): its
// table at its field's value (nearest voxel; 0 without the field).
vec3 Factor(uint i, uint k, vec3 p, float rho, uvec4 types, inout pnanovdb_readaccessor_t temperature,
            inout pnanovdb_readaccessor_t color, inout pnanovdb_readaccessor_t emission)
{
    uint slot = volumes[i].lutSlots[k];
    float v = rho;
    if (slot != 0u) {
        uint r = (min(slot, 3u) - 1u) * 3u;
        ivec3 q = ivec3(floor(Row3(volumes[i].toGrid[r], volumes[i].toGrid[r + 1u], volumes[i].toGrid[r + 2u], p, 1.0)
                              + 0.5));
        v = slot == 1u ? ReadRgb(types.y, temperature, q).x
          : slot == 2u ? ReadRgb(types.z, color, q).x
          : slot == 3u ? ReadRgb(types.w, emission, q).x : 0.0;
    }
    return Lut(volumes[i].lutOffsets[k], volumes[i].lutLow[k], volumes[i].lutScale[k], v);
}

// Light through optical depth `depth` with multiple scattering: octave o
// weighs params.octaveLight^o and sees the depth times params.octaveDepth^o
// (Wrenninge 2015, the phase function kept); one octave: exp(-depth).
vec3 Octaves(vec3 depth)
{
    vec3 sum = vec3(0.0);
    float a = 1.0, b = 1.0;
    for (uint o = 0u; o < params.octaves; ++o) {
        sum += a * exp(-b * depth);
        a *= params.octaveLight;
        b *= params.octaveDepth;
    }
    return sum;
}

// The density (bit 3 of mask m) times the factors (bit k: fk) it names.
vec3 Scale(uint m, float rho, vec3 f0, vec3 f1, vec3 f2)
{
    return max(((m & 8u) != 0u ? rho : 1.0) * ((m & 1u) != 0u ? f0 : vec3(1.0)) * ((m & 2u) != 0u ? f1 : vec3(1.0)) *
                   ((m & 4u) != 0u ? f2 : vec3(1.0)),
               0.0);
}

// Marches volume i over [t0, t1] of the world ray, adding to radiance and
// attenuating transmittance; steps come out of stepsLeft.
void March(uint i, vec3 origin, vec3 dir, float t0, float t1, float noise, vec3 up, inout uint stepsLeft,
           inout vec3 radiance, inout vec3 transmittance)
{
    vec4 w0 = volumes[i].worldToIndex[0], w1 = volumes[i].worldToIndex[1], w2 = volumes[i].worldToIndex[2];
    vec3 lo = volumes[i].indexMin.xyz, hi = volumes[i].indexMax.xyz;
    uvec4 grids = volumes[i].grids, types = volumes[i].types;
    uvec4 masks = volumes[i].factorMasks;
    if (volumes[i].medium.z > 0.0 && params.defaultFields == 0u) {
        masks.w = 0u;  // no volume material: density only (VolumePass::Settings::defaultFields)
    }
    uint factors = (masks.x | masks.y | masks.z) & 7u;
    const bool plain = !kFactors || (factors == 0u && all(equal(masks.xyz, uvec3(8u))));  // per unit of density
    uint table = kFactors ? volumes[i].lutOffsets.w : kNone;
    float tableLow = volumes[i].lutLow.w, tableScale = volumes[i].lutScale.w;
    vec3 oi = Row3(w0, w1, w2, origin, 1.0), di = Row3(w0, w1, w2, dir, 0.0);
    vec3 safe = SafeDir(di);
    // The material (or the defaults): scattering and absorption per unit of
    // density, per channel, the phase function, the dome's light scattered
    // with it.
    vec3 scattering = volumes[i].scattering.rgb * params.density;
    vec3 absorption = volumes[i].absorption.rgb * params.density;
    float g = volumes[i].medium.y > 0.0 ? volumes[i].scattering.w : params.anisotropy;
    vec3 dome = frame.envRadiance.w > 0.0 ? DomeScatter(dir, g) : vec3(0.0);

    // Steps of stepSize voxels; a box that would need more than maxSteps
    // gets longer ones.
    float dt = max(params.stepSize / length(di), (t1 - t0) / float(max(params.maxSteps, 1u)));
    pnanovdb_readaccessor_t density = Accessor(grids.x), shadow = Accessor(grids.x);
    // Absent grids (never read) get the density's.
    pnanovdb_readaccessor_t temperature = Accessor(grids.y != kNone ? grids.y : grids.x);
    pnanovdb_readaccessor_t color = Accessor(grids.z != kNone ? grids.z : grids.x);
    pnanovdb_readaccessor_t emission = Accessor(grids.w != kNone ? grids.w : grids.x);
    const bool mutual = params.mutual != 0u && params.count > 1u && params.shadowSteps > 0u;
    float t = t0 + noise * dt;
    while (t < t1 && stepsLeft > 0u && Max3(transmittance) > kOpaque) {
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
        // The other volumes' shadow steps start at a different fraction each
        // step (golden ratio), so they average over the march, not per frame.
        float shadowNoise = fract(noise + 0.6180340 * float(stepsLeft));
        // The factors (the material's ramps and remaps of a field each),
        // then which multiply what: the density itself (bit 3) or factor k.
        vec3 scaleS = vec3(rho), scaleA = vec3(rho), scaleE = vec3(rho);
        if (!plain) {
            vec3 f0 = (factors & 1u) != 0u ? Factor(i, 0u, p, rho, types, temperature, color, emission) : vec3(1.0);
            vec3 f1 = (factors & 2u) != 0u ? Factor(i, 1u, p, rho, types, temperature, color, emission) : vec3(1.0);
            vec3 f2 = (factors & 4u) != 0u ? Factor(i, 2u, p, rho, types, temperature, color, emission) : vec3(1.0);
            scaleS = Scale(masks.x, rho, f0, f1, f2);
            scaleA = Scale(masks.y, rho, f0, f1, f2);
            scaleE = Scale(masks.z, rho, f0, f1, f2);
        }
        // The colour field scales the scattering (MaterialX: a colour
        // geompropvalue times it), so it colours what shows through too.
        vec3 sigmaS = scaleS * scattering;
        if ((masks.w & 2u) != 0u) {
            vec3 q = Row3(volumes[i].toGrid[3], volumes[i].toGrid[4], volumes[i].toGrid[5], p, 1.0);
            sigmaS *= clamp(ReadRgb(types.z, color, ivec3(floor(q + 0.5))), 0.0, 1.0);
        }
        vec3 sigma = scaleA * absorption + sigmaS;
        // Towards the lights: the optical depth of the density (or of the
        // extinction table) times this point's extinction per unit of it.
        vec3 extinction = sigma / max(table == kNone ? rho : ExtinctionTable(table, tableLow, tableScale, rho), 1e-20);
        vec3 x = origin + t * dir;
        vec3 emitted = vec3(0.0);
        if ((masks.w & 1u) != 0u) {
            vec3 q = Row3(volumes[i].toGrid[0], volumes[i].toGrid[1], volumes[i].toGrid[2], p, 1.0);
            float kelvin = ReadFloat(types.y, temperature, ivec3(floor(q + 0.5))) * volumes[i].params.x;
            // Physical: brighter as T^4. MaterialX's blackbody is luminance 1
            // down to 800 K's colour; it fades out below that here (the
            // Draper point: nothing visible glows colder).
            float physical = volumes[i].params.z > 0.0 ? pow(max(kelvin, 0.0) / 1500.0, 4.0)
                                                       : smoothstep(500.0, 900.0, kelvin);
            emitted += Blackbody(kelvin) * volumes[i].params.y * physical;
        }
        if ((masks.w & 4u) != 0u) {
            vec3 q = Row3(volumes[i].toGrid[6], volumes[i].toGrid[7], volumes[i].toGrid[8], p, 1.0);
            emitted += max(ReadRgb(types.w, emission, ivec3(floor(q + 0.5))), 0.0) * volumes[i].fieldEmission.rgb;
        }
        emitted += volumes[i].emission.rgb * scaleE;
        radiance += transmittance * emitted * params.emission * dt;
        if (Max3(sigma) > 0.0) {
            vec3 albedo = mix(vec3(0.0), sigmaS / max(sigma, 1e-20), greaterThan(sigma, vec3(0.0)));
            vec3 light = vec3(0.0);
            if (frame.envRadiance.w > 0.0) {
                vec3 depth = extinction * ShadowMarch(table, tableLow, tableScale, shadow, p, Row3(w0, w1, w2, up, 0.0), lo, hi, noise,
                                                      (params.shadowSteps + 1u) / 2u);
                if (mutual) {
                    depth += OthersDepth(i, x, up * 1e4, shadowNoise);  // the other volumes above, as its own
                }
                light = dome * Octaves(0.5 * depth);
            }
            for (uint k = 0u; k < frame.counts.x; ++k) {
                Light sun = lights[k];
                vec3 l = sun.position.xyz;
                vec3 depth = extinction * ShadowMarch(table, tableLow, tableScale, shadow, p, Row3(w0, w1, w2, l, 0.0), lo, hi, noise,
                                                      params.shadowSteps);
                if (mutual) {
                    // ponytail: a sun's ray marches 1e4 units (the boxes clip it), as rt_shade.glsl's.
                    depth += OthersDepth(i, x, l * 1e4, shadowNoise);
                }
                vec3 scattered = Octaves(depth) * Phase(dot(dir, l), g);
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
                        uint index = clusterLights[base + 1u + k];
                        vec3 e = LocalIrradiance(index, x, l);
                        e = any(greaterThan(e, vec3(0.0))) ? e * LocalShadow(index, x) : e;
                        e = mutual && any(greaterThan(e, vec3(0.0)))
                                ? e * exp(-OthersDepth(i, x, lights[index].position.xyz - x, shadowNoise)) : e;
                        light += e * Phase(dot(dir, l), g);
                    }
                } else {
                    for (uint k = frame.counts.x; k < frame.counts.x + frame.localLights.x; ++k) {
                        vec3 e = LocalIrradiance(k, x, l);
                        e = any(greaterThan(e, vec3(0.0))) ? e * LocalShadow(k, x) : e;
                        e = mutual && any(greaterThan(e, vec3(0.0)))
                                ? e * exp(-OthersDepth(i, x, lights[k].position.xyz - x, shadowNoise)) : e;
                        light += e * Phase(dot(dir, l), g);
                    }
                }
            }
            // In-scattering integrated over the step (Hillaire 2015).
            vec3 stepT = exp(-sigma * dt);
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
    // the farthest translucent layer (marched before it); the part in front
    // goes over that after it, each stretch between layers seen through the
    // layers in front of it (their colours times its light). kThrough: all
    // of the ray so. Without a layer, all of the ray is behind.
    const uint kMaxLayers = 4u;  // VolumePass::kMaxLayers
    float layerT[kMaxLayers];
    vec3 layerTint[kMaxLayers];
    uint layers = 0u;
    float tStart = 0.0;
    if (params.part != 0u) {
        ivec3 size = textureSize(translucentLayers, 0);
        ivec2 q = min(ivec2(fragCoord), size.xy - 1);
        for (int k = 0; k < min(size.z, int(kMaxLayers)); ++k) {
            vec4 l = texelFetch(translucentLayers, ivec3(q, k), 0);
            if (l.a < 0.0 || l.a >= tEnd) {
                break;
            }
            layerT[layers] = l.a;
            layerTint[layers] = l.rgb;
            ++layers;
        }
        float split = layers > 0u ? layerT[layers - 1u] : 0.0;
        if (params.part == 1u) {
            tStart = split;
            layers = 0u;  // the translucency pass dims it
        } else if (params.part == 2u) {
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
        Output(vec3(0.0), vec3(1.0));  // the half resolution target isn't cleared
        return;
    }

    float noise = SampleNoise(kDimVolume, gl_FragCoord.xy, float(frame.localLights.y));
    // The dome's pole (its latlong's +y) in world space.
    mat3 toDome = mat3(frame.envWorldToLocal);
    vec3 up = normalize(vec3(toDome[0][1], toDome[1][1], toDome[2][1]));
    vec3 radiance = vec3(0.0);
    vec3 transmittance = vec3(1.0);
    uint stepsLeft = params.maxSteps;
    for (uint k = 0u; k < n && Max3(transmittance) > kOpaque; ++k) {
        vec3 before = radiance;
        vec3 transmittanceBefore = transmittance;
        // Stretches between translucent layers, each one's light through
        // the layers in front of it.
        float a = spans[k].x;
        vec3 tint = vec3(1.0);
        uint l = 0u;
        for (uint stretch = 0u; stretch <= layers && a < spans[k].y; ++stretch) {
            for (; l < layers && layerT[l] <= a; ++l) {
                tint *= layerTint[l];
            }
            float b = l < layers ? min(layerT[l], spans[k].y) : spans[k].y;
            vec3 stretchBefore = radiance;
            March(ids[k], origin, dir, a, b, noise, up, stepsLeft, radiance, transmittance);
            radiance = stretchBefore + (radiance - stretchBefore) * tint;
            a = b;
        }
        // Volumetric fog (fog.glsl): the image below carries it all the way
        // to the surface behind. The volume's light reaches the eye through
        // the fog in front of it, and that fog's own light isn't hidden by
        // the volume: add what the blend's attenuation takes from it.
        // ponytail: fog inside a volume's box counts as behind it.
        vec4 fog = Fog(fragCoord, dot(origin + spans[k].x * dir - frame.cameraPos.xyz, frame.cameraForward.xyz));
        radiance = before + (radiance - before) * fog.a + (transmittanceBefore - transmittance) * fog.rgb;
    }
    Output(radiance, transmittance);
#ifndef HALF
    VolumeSplits(radiance, transmittance);
#endif
}
