// Caustic photons (RayTracing::Work::caustics): rt_refraction.rgen built
// with PHOTONS (rt_photons.glsl) stores them every sample, mesh.frag and
// traced hits (rt_shade.glsl's ShadeHit) gather them. Include after
// gi.glsl; CAUSTICS_WRITE: the storing side.
// One buffer: [0] the photons stored, [4, 4 + cells) the hash grid's heads
// (the last photon in that cell, kNone: empty), then 8 words per photon:
// position, the next photon in its cell, flux (rgb), its direction of
// travel (octahedral, snorm16 x 2). A cell is twice the gather radius
// (rt.caustics.y), so a gather reads 2 x 2 x 2 of them.
#ifndef CAUSTICS_GLSL
#define CAUSTICS_GLSL
#ifdef CAUSTICS_WRITE
layout(std430, set = 0, binding = 92) buffer CausticPhotons { uint photons[]; };
#else
layout(std430, set = 0, binding = 92) readonly buffer CausticPhotons { uint photons[]; };
#endif
// DrawInfo::caustics.
const uint kCausticsCast = 1u, kCausticsReceive = 2u;

uint CausticCell(ivec3 c)
{
    return ((uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ (uint(c.z) * 83492791u)) & (rt.causticInfo.z - 1u);
}

uint PhotonWord(uint photon)
{
    return 4u + rt.causticInfo.z + 8u * photon;
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
    photons[w + 3u] = atomicExchange(photons[4u + CausticCell(ivec3(floor(p / rt.caustics.y)))], i);
}
#endif
// The caustic light at p (normal n facing the viewer): irradiance / pi of
// the photons within the gather radius (a flat disc around p: at most a
// quarter radius off its plane) that arrive on n's side.
// ponytail: at most kMaxGathered photons a pixel (a hang guard for a far too
// large radius); raise the photon count's share of the radius instead.
const uint kMaxGathered = 65536u;  // 4096 cut caustics.usda's focus to half
vec3 CausticDiffuse(vec3 p, vec3 n)
{
    float r = rt.caustics.x, r2 = r * r;
    ivec3 lo = ivec3(floor((p - r) / rt.caustics.y));
    uint cells[8];
    vec3 sum = vec3(0.0);
    uint gathered = 0u;
    for (int k = 0; k < 8; ++k) {
        uint cell = CausticCell(lo + ivec3(k & 1, (k >> 1) & 1, k >> 2));
        cells[k] = cell;
        bool seen = false;
        for (int j = 0; j < k; ++j) {
            seen = seen || cells[j] == cell;
        }
        for (uint i = seen ? kNone : photons[4u + cell]; i != kNone && gathered < kMaxGathered;
             i = photons[PhotonWord(i) + 3u]) {
            ++gathered;
            uint w = PhotonWord(i);
            vec3 d = uintBitsToFloat(uvec3(photons[w], photons[w + 1u], photons[w + 2u])) - p;
            if (dot(d, d) < r2 && abs(dot(d, n)) < 0.25 * r &&
                dot(OctDecode(unpackSnorm2x16(photons[w + 7u])), n) < 0.0) {
                sum += uintBitsToFloat(uvec3(photons[w + 4u], photons[w + 5u], photons[w + 6u]));
            }
        }
    }
    return sum / (PI * PI * r2);
}
#endif
