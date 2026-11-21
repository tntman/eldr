// Caustic photons (RayTracing::Work::caustics): rt_refraction.rgen built
// with PHOTONS (rt_photons.glsl) stores them every sample, mesh.frag and
// traced hits (rt_shade.glsl's ShadeHit) gather them. Include after
// gi.glsl; CAUSTICS_WRITE: the storing side.
// One buffer: [0] the photons stored, [4, 4 + cells) the hash grids' heads
// (the last photon in that cell, kNone: empty), then 9 words per photon:
// position, the next photon in its fine cell, flux (rgb), its direction of
// travel (octahedral, snorm16 x 2), the next photon in its coarse cell.
// Two grids share the heads: fine cells twice the gather radius
// (rt.caustics.y), so a gather reads 2 x 2 x 2 of them, and coarse ones
// kCausticCoarse times that, for sparse photons (CausticDiffuse).
#ifndef CAUSTICS_GLSL
#define CAUSTICS_GLSL
#ifdef CAUSTICS_WRITE
layout(std430, set = 0, binding = 92) buffer CausticPhotons { uint photons[]; };
#else
layout(std430, set = 0, binding = 92) readonly buffer CausticPhotons { uint photons[]; };
#endif
// DrawInfo::caustics.
const uint kCausticsCast = 1u, kCausticsReceive = 2u;
// The coarse grid's radius over the gather radius, and the photons a
// gather within the gather radius wants before it trusts that alone.
const float kCausticCoarse = 8.0, kCausticEnough = 16.0;

uint CausticCell(ivec3 c, uint level)
{
    return ((uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ (uint(c.z) * 83492791u) ^ (level * 2654435761u)) &
           (rt.causticInfo.z - 1u);
}

uint PhotonWord(uint photon)
{
    return 4u + rt.causticInfo.z + 9u * photon;
}

#ifdef CAUSTICS_WRITE
void StorePhoton(vec3 p, vec3 flux, vec3 dir)
{
    uint i = atomicAdd(photons[0], 1u);
    if (i >= rt.causticInfo.w) {
        return;
    }
    uint w = PhotonWord(i);
    photons[w] = floatBitsToUint(p.x);
    photons[w + 1u] = floatBitsToUint(p.y);
    photons[w + 2u] = floatBitsToUint(p.z);
    photons[w + 4u] = floatBitsToUint(flux.r);
    photons[w + 5u] = floatBitsToUint(flux.g);
    photons[w + 6u] = floatBitsToUint(flux.b);
    photons[w + 7u] = packSnorm2x16(OctEncode(dir));
    photons[w + 3u] = atomicExchange(photons[4u + CausticCell(ivec3(floor(p / rt.caustics.y)), 0u)], i);
    photons[w + 8u] =
        atomicExchange(photons[4u + CausticCell(ivec3(floor(p / (kCausticCoarse * rt.caustics.y))), 1u)], i);
}
#endif
// The photons of one grid within r of p (a flat disc around p: at most a
// quarter radius off its plane) that arrive on n's side: their flux (rgb)
// and count (a), stopping once `enough` count. level 0: the fine grid, 1
// the coarse one (r its radius).
// ponytail: at most kMaxGathered photons a pixel (a hang guard for a far too
// large radius); raise the photon count's share of the radius instead.
const uint kMaxGathered = 65536u;  // 4096 cut caustics.usda's focus to half
vec4 CausticGather(vec3 p, vec3 n, float r, uint level, float enough)
{
    float r2 = r * r;
    ivec3 lo = ivec3(floor((p - r) / (2.0 * r)));
    uint cells[8];
    vec4 sum = vec4(0.0);
    uint gathered = 0u;
    for (int k = 0; k < 8; ++k) {
        uint cell = CausticCell(lo + ivec3(k & 1, (k >> 1) & 1, k >> 2), level);
        cells[k] = cell;
        bool seen = false;
        for (int j = 0; j < k; ++j) {
            seen = seen || cells[j] == cell;
        }
        for (uint i = seen ? kNone : photons[4u + cell]; i != kNone && gathered < kMaxGathered && sum.a < enough;
             i = photons[PhotonWord(i) + 3u + 5u * level]) {
            ++gathered;
            uint w = PhotonWord(i);
            vec3 d = uintBitsToFloat(uvec3(photons[w], photons[w + 1u], photons[w + 2u])) - p;
            if (dot(d, d) < r2 && abs(dot(d, n)) < 0.25 * r &&
                dot(OctDecode(unpackSnorm2x16(photons[w + 7u])), n) < 0.0) {
                sum += vec4(uintBitsToFloat(uvec3(photons[w + 4u], photons[w + 5u], photons[w + 6u])), 1.0);
            }
        }
    }
    return sum;
}

// The caustic light at p (normal n facing the viewer): irradiance / pi of
// the photons within the gather radius. The disc's photons are trusted by
// how many it holds, or would hold at the density within the coarse radius
// around (a dark gap in a dense web is dark), up to kCausticEnough; the
// rest of the weight takes the coarse radius's estimate. Light a caster
// spreads thin (a glass ball's reflection of the sun over the walls) puts
// one photon in a small disc now and then: a bright dot per sample, too few
// to average out over the film; the coarse disc spreads it smoothly.
// As the film shrinks the radius (rt.caustics.w the first sample's), the
// photons it should hold shrink with its area.
vec3 CausticDiffuse(vec3 p, vec3 n)
{
    float r = rt.caustics.x;
    float enough = kCausticEnough * min(r * r / (rt.caustics.w * rt.caustics.w), 1.0);
    vec4 fine = CausticGather(p, n, r, 0u, 1e30);
    vec3 light = fine.rgb / (PI * PI * r * r);
    if (fine.a < enough) {
        float R = kCausticCoarse * r, scale = kCausticCoarse * kCausticCoarse;
        vec4 coarse = CausticGather(p, n, R, 1u, enough * scale);
        float trust = min(max(fine.a, coarse.a / scale) / enough, 1.0);
        light = mix(coarse.rgb / (PI * PI * R * R), light, trust);
    }
    return light;
}
#endif
