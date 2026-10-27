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
                           // z: 1 = no volume material
    vec4 emission;         // rgb: radiance per unit length per unit of density
    vec4 fieldEmission;    // rgb: times the emission field
    uvec4 factorMasks;     // which factors multiply scattering, absorption, emission (bit k: factor k,
                           // 8: the density); w: roles read (1 blackbody, 2 colour tint, 4 emission field)
    uvec4 lutOffsets;      // factors' tables (words in the grid pool, kNone: none); w: the extinction's
    uvec4 lutSlots;        // the grid each factor reads (as grids; 4: none, it reads 0)
    vec4 lutLow;           // table index = (value - low) * scale
    vec4 lutScale;
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

// A volume's extinction table (lutOffsets.w: kVolumeLut floats) at density
// rho, the nearest entry: the mean extinction there (before the settings'
// multiplier). Without a table that is rho times medium.x.
float ExtinctionTable(uint table, float low, float scale, float rho)
{
    return uintBitsToFloat(pnanovdb_buf_data[table + uint(clamp((rho - low) * scale + 0.5, 0.0,
                                                                 float(kVolumeLut - 1u)))]);
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

const uint kShadowSteps = 64u;     // per volume and shadow ray, at most
const float kShadowOpaque = 6.9;  // optical depth of 1e-3 transmittance: nothing is left to dim

// Optical depth of the first `count` volumes along origin + dir * s, s in
// [0, 1] (volume_shadow.frag's texels, traced shadow rays), stepping
// 2 x stepSize density voxels (VolumeParams); `weighted`, `weighted2`: its
// integral times s and times s^2. Segments outside a volume's box are skipped and the march stops
// at kShadowOpaque, but empty tiles inside it are still stepped.
float VolumeOpticalDepth(vec3 origin, vec3 dir, uint count, float stepSize, float density, out float weighted,
                         out float weighted2)
{
    float len = length(dir);
    float tau = 0.0;
    weighted = 0.0;
    weighted2 = 0.0;
    for (uint i = 0u; i < count && tau < kShadowOpaque; ++i) {
        vec4 w0 = volumes[i].worldToIndex[0], w1 = volumes[i].worldToIndex[1], w2 = volumes[i].worldToIndex[2];
        vec3 oi = Row3(w0, w1, w2, origin, 1.0), di = Row3(w0, w1, w2, dir, 0.0);
        vec2 span = Slabs(oi, di, volumes[i].indexMin.xyz, volumes[i].indexMax.xyz);
        span = vec2(max(span.x, 0.0), min(span.y, 1.0));
        if (!(span.x < span.y)) {
            continue;
        }
        uint steps = uint(clamp(ceil((span.y - span.x) * length(di) / (2.0 * stepSize)), 1.0, float(kShadowSteps)));
        float ds = (span.y - span.x) / float(steps);
        uint table = volumes[i].lutOffsets.w;
        float low = volumes[i].lutLow.w, scale = volumes[i].lutScale.w;
        float sigma = density * ds * len * (table == 0xFFFFFFFFu ? volumes[i].medium.x : 1.0);
        pnanovdb_readaccessor_t acc = Accessor(volumes[i].grids.x);
        for (uint k = 0u; k < steps && tau < kShadowOpaque; ++k) {
            float s = span.x + (float(k) + 0.5) * ds;
            float d = max(ReadFloat(PNANOVDB_GRID_TYPE_FLOAT, acc, ivec3(floor(oi + di * s + 0.5))), 0.0);
            d = (table == 0xFFFFFFFFu ? d : ExtinctionTable(table, low, scale, d)) * sigma;
            tau += d;
            weighted += d * s;
            weighted2 += d * s * s;
        }
    }
    return tau;
}
