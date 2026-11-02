// The volumes' grids and table (VolumePass), shared by volume.frag and
// volume_shadow.frag.

layout(std430, set = 0, binding = 64) readonly buffer Grids { uint pnanovdb_buf_data[]; };
#define PNANOVDB_GLSL
#include "PNanoVDB.h"

// std430 mirror of GpuVolume in volume.h.
struct Volume {
    vec4 worldToIndex[3];  // density index = rows . (world, 1)
    vec4 indexMin;         // density index-space box around the active voxels
    vec4 indexMax;
    uvec4 grids;           // byte offsets: density, temperature, color, emission (kNone: none)
    uvec4 types;           // PNANOVDB_GRID_TYPE_*
    vec4 toGrid[9];        // density index -> temperature, color, emission index (3 rows each)
    vec4 params;           // x: temperature to Kelvin, y: blackbody intensity, z: 1 = times (K / 1500)^4
    vec4 scattering;       // rgb: sigma_s per unit of density; w: Henyey-Greenstein g
    vec4 absorption;       // rgb: sigma_a per unit of density
    vec4 medium;           // x: mean extinction per unit of density, y: 1 = scattering.w is the g,
                           // z: 1 = no volume material, w: Volume Limit (below 0: params.volumeLimit)
    vec4 emission;         // rgb: radiance per unit length per unit of density
    vec4 fieldEmission;    // rgb: times the emission field
    uvec4 factorMasks;     // which factors multiply scattering, absorption, emission (bit k: factor k,
                           // 8: the density); w: roles read (1 blackbody, 2 colour tint, 4 emission field)
    uvec4 lutOffsets;      // factors' tables (words in the grid pool, kNone: none); w: the extinction's
    uvec4 lutSlots;        // the grid each factor reads (as grids; 4: none, it reads 0)
    vec4 lutLow;           // table index = (value - low) * scale
    vec4 lutScale;
    uvec4 velocity;        // velocity grids (Vec3f: x; float components: xyz); w: 0 none, 1 Vec3f, 3 floats
    vec4 toVelocity[3];    // density index -> velocity index
    vec4 advect[3];        // velocity -> density index offset per frame
};
layout(std430, set = 0, binding = 65) readonly buffer Volumes { Volume volumes[]; };

pnanovdb_buf_t buf;

const uint kVolumeLut = 1024u;  // entries per table (volume.h)

// A factor's table at field value v: linear between entries, clamped.
vec3 Lut(uint offset, float low, float scale, float v)
{
    float f = clamp((v - low) * scale, 0.0, float(kVolumeLut - 1u));
    uint i = min(uint(f), kVolumeLut - 2u);
    uint a = offset + i * 4u;
    vec3 x = uintBitsToFloat(uvec3(pnanovdb_buf_data[a], pnanovdb_buf_data[a + 1u], pnanovdb_buf_data[a + 2u]));
    vec3 y = uintBitsToFloat(uvec3(pnanovdb_buf_data[a + 4u], pnanovdb_buf_data[a + 5u], pnanovdb_buf_data[a + 6u]));
    return mix(x, y, f - float(i));
}

// A volume's extinction table (lutOffsets.w: kVolumeLut entries of 4
// floats) at density rho, the nearest entry: rgb the extinction per channel
// there, w their mean (before the settings' multiplier). Without a table
// that is rho times scattering + absorption, and medium.x.
uint ExtinctionEntry(uint table, float low, float scale, float rho)
{
    return table + 4u * uint(clamp((rho - low) * scale + 0.5, 0.0, float(kVolumeLut - 1u)));
}
float ExtinctionTable(uint table, float low, float scale, float rho)
{
    return uintBitsToFloat(pnanovdb_buf_data[ExtinctionEntry(table, low, scale, rho) + 3u]);
}
vec3 ExtinctionTableRgb(uint table, float low, float scale, float rho)
{
    uint a = ExtinctionEntry(table, low, scale, rho);
    return uintBitsToFloat(uvec3(pnanovdb_buf_data[a], pnanovdb_buf_data[a + 1u], pnanovdb_buf_data[a + 2u]));
}

vec3 Row3(vec4 r0, vec4 r1, vec4 r2, vec3 p, float w)
{
    vec4 q = vec4(p, w);
    return vec3(dot(r0, q), dot(r1, q), dot(r2, q));
}

// Avoids 0 * inf in the slab tests.
vec3 SafeDir(vec3 d)
{
    return mix(d, vec3(1e-12), lessThan(abs(d), vec3(1e-12)));
}

// Ray parameters [t0, t1] inside box [lo, hi] (t0 > t1: missed).
vec2 Slabs(vec3 o, vec3 d, vec3 lo, vec3 hi)
{
    vec3 inv = 1.0 / SafeDir(d);
    vec3 a = (lo - o) * inv, b = (hi - o) * inv;
    vec3 near = min(a, b), far = max(a, b);
    return vec2(max(max(near.x, near.y), near.z), min(min(far.x, far.y), far.z));
}

float ReadFloat(uint type, inout pnanovdb_readaccessor_t acc, ivec3 ijk)
{
    return pnanovdb_read_float(buf, pnanovdb_readaccessor_get_value_address(type, buf, acc, ijk));
}

pnanovdb_readaccessor_t Accessor(uint offset)
{
    pnanovdb_grid_handle_t grid;
    grid.address.byte_offset = offset;
    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(acc, pnanovdb_tree_get_root(buf, pnanovdb_grid_get_tree(buf, grid)));
    return acc;
}

// Velocity blur (Karma's Lookup Offset): in a motion blur time sample
// (frame.deform.z, its offset from this frame frame.deform.x in frames)
// volume i's fields at density index p are read at p - vel(p) * t, the
// velocity the nearest voxel's.
// VOLUME_STATIC (volume_grid.comp): every volume as at this frame.
bool Moving(uint i)
{
#ifdef VOLUME_STATIC
    return false;
#else
    return volumes[i].velocity.w != 0u && frame.deform.z > 0.0 && frame.deform.x != 0.0;
#endif
}
vec3 Advect(uint i, vec3 p)
{
    if (!Moving(i)) {
        return p;
    }
    ivec3 q = ivec3(floor(Row3(volumes[i].toVelocity[0], volumes[i].toVelocity[1], volumes[i].toVelocity[2], p, 1.0)
                          + 0.5));
    uvec4 grids = volumes[i].velocity;
    vec3 v;
    if (grids.w == 1u) {
        pnanovdb_readaccessor_t acc = Accessor(grids.x);
        pnanovdb_address_t a = pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_VEC3F, buf, acc, q);
        v = vec3(pnanovdb_read_float(buf, a), pnanovdb_read_float(buf, pnanovdb_address_offset(a, 4u)),
                 pnanovdb_read_float(buf, pnanovdb_address_offset(a, 8u)));
    } else {
        pnanovdb_readaccessor_t x = Accessor(grids.x), y = Accessor(grids.y), z = Accessor(grids.z);
        v = vec3(ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, x, q), ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, y, q),
                 ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, z, q));
    }
    return p - Row3(volumes[i].advect[0], volumes[i].advect[1], volumes[i].advect[2], v, 0.0) * frame.deform.x;
}

const uint kShadowSteps = 64u;     // per volume and shadow ray, at most
const float kShadowOpaque = 6.9;  // optical depth of 1e-3 transmittance: nothing is left to dim

// Optical depth per channel of the first `count` volumes along
// origin + dir * s, s in [0, 1] (volume_shadow.frag's texels, traced shadow
// rays), stepping 2 x stepSize density voxels (VolumeParams): coloured
// smoke casts coloured shadows. `weighted`, `weighted2`: the integral of the
// channels' mean times s and times s^2. Segments outside a volume's box are
// skipped and the march stops once every channel reaches kShadowOpaque, but
// empty tiles inside it are still stepped. shadowLink: the light's shadow
// link bit (-1: every volume casts), as the volumes' link masks hold it.
// ponytail: per channel from the material's coefficients (or the extinction
// table of the density); a colour field (Cd) tinting the scattering isn't read.
vec3 VolumeOpticalDepth(vec3 origin, vec3 dir, uint count, float stepSize, float density, int shadowLink,
                        out float weighted, out float weighted2)
{
    float len = length(dir);
    vec3 tau = vec3(0.0);
    weighted = 0.0;
    weighted2 = 0.0;
    for (uint i = 0u; i < count && min(min(tau.x, tau.y), tau.z) < kShadowOpaque; ++i) {
        vec4 w0 = volumes[i].worldToIndex[0], w1 = volumes[i].worldToIndex[1], w2 = volumes[i].worldToIndex[2];
        vec3 oi = Row3(w0, w1, w2, origin, 1.0), di = Row3(w0, w1, w2, dir, 0.0);
        vec2 span = Slabs(oi, di, volumes[i].indexMin.xyz, volumes[i].indexMax.xyz);
        span = vec2(max(span.x, 0.0), min(span.y, 1.0));
        bool linked = shadowLink < 0 || (floatBitsToUint(volumes[i].indexMin.w) & (1u << uint(shadowLink))) != 0u;
        if (!(span.x < span.y) || !linked) {
            continue;
        }
        uint steps = uint(clamp(ceil((span.y - span.x) * length(di) / (2.0 * stepSize)), 1.0, float(kShadowSteps)));
        float ds = (span.y - span.x) / float(steps);
        uint table = volumes[i].lutOffsets.w;
        float low = volumes[i].lutLow.w, scale = volumes[i].lutScale.w;
        vec3 sigma = density * ds * len *
                     (table == 0xFFFFFFFFu ? volumes[i].scattering.rgb + volumes[i].absorption.rgb : vec3(1.0));
        pnanovdb_readaccessor_t acc = Accessor(volumes[i].grids.x);
        for (uint k = 0u; k < steps && min(min(tau.x, tau.y), tau.z) < kShadowOpaque; ++k) {
            float s = span.x + (float(k) + 0.5) * ds;
            float d = max(ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, acc, ivec3(floor(Advect(i, oi + di * s) + 0.5))), 0.0);
            vec3 e = (table == 0xFFFFFFFFu ? vec3(d) : ExtinctionTableRgb(table, low, scale, d)) * sigma;
            float m = dot(e, vec3(1.0 / 3.0));
            tau += e;
            weighted += m * s;
            weighted2 += m * s * s;
        }
    }
    return tau;
}

// The density (without a table) or the extinction table's value
// (ExtinctionTable) integrated from density index p towards a distant
// light (index-space direction dl per world unit) to the edge of the box,
// in `steps` steps (volume.frag's march, volume_grid.comp's cells).
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

// Optical depth per channel of the first `count` volumes but `self` along
// x + dir * s, s in [0, 1] (as VolumeOpticalDepth): `steps` steps across
// each box crossed (0: one per voxel, at most 256), offset by noise;
// `density`: the extinction multiplier; `tables`: read extinction tables
// (volume.frag's kFactors). The self march (ShadowMarch) covers the
// volume's own density, so it is left out here. No tint maps: they hold
// all volumes' transmittance together, the lit volume's own included, with
// one depth per texel, so they can't tell another volume in front from
// this one.
// Steps landing in an empty tile go on to the first step past it; the
// march stops once nothing is left to dim (kShadowOpaque).
// ponytail: fixed steps per box; cost grows with the volumes crossed.
vec3 OthersDepth(uint self, vec3 x, vec3 dir, float noise, uint count, uint steps, float density, bool tables)
{
    vec3 tau = vec3(0.0);
    for (uint j = 0u; j < count && min(min(tau.x, tau.y), tau.z) < kShadowOpaque; ++j) {
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
        float n = steps > 0u ? float(steps) : clamp(ceil((span.y - span.x) * length(di)), 1.0, 256.0);
        float ds = (span.y - span.x) / n;
        uint table = tables ? volumes[j].lutOffsets.w : kNone;
        float low = volumes[j].lutLow.w, tableScale = volumes[j].lutScale.w;
        vec3 scale = ds * length(dir) * density *
                     (table == kNone ? volumes[j].scattering.rgb + volumes[j].absorption.rgb : vec3(1.0));
        vec3 safe = SafeDir(di);
        pnanovdb_readaccessor_t acc = Accessor(volumes[j].grids.x);
        const bool moving = Moving(j);  // velocity blur: no tile skips (the fields move)
        float k = 0.0;
        while (k < n && min(min(tau.x, tau.y), tau.z) < kShadowOpaque) {
            vec3 q = Advect(j, oi + di * (span.x + (k + noise) * ds)) + 0.5;  // nearest voxel: floor(q)
            uint level;
            pnanovdb_address_t a = pnanovdb_readaccessor_get_value_address_and_level(PNANOVDB_GRID_TYPE_FLOAT, buf, acc,
                                                                                     ivec3(floor(q)), level);
            float v = pnanovdb_read_float(buf, a);
            if (level > 0u && v <= 0.0 && !moving) {
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
