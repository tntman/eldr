// CPU precomputation for image-based lighting. Hydra-free.
#pragma once

#include <cstdint>
#include <vector>

namespace vkr {

struct Texture;

// Prefiltered radiance cube + diffuse SH for one environment.
// Cube faces follow Vulkan's face/texel convention, so `data` uploads as is.
struct EnvironmentMaps {
    uint32_t size = 0;    // face size of level 0
    uint32_t levels = 0;  // level m is prefiltered for roughness m / (levels - 1)
    // RGBA32F texels, level-major, then face (+X,-X,+Y,-Y,+Z,-Z), then rows.
    std::vector<float> data;
    // Irradiance E(n) = sum sh[i] * Y_i(n), cosine lobe already convolved.
    float sh[9][3] = {};
};

// `latlong` uses the UsdLux/Storm mapping in the dome's local space:
// u = (atan2(z, x) + pi/2) / 2pi, v = acos(y) / pi measured from the top row.
// Null means a uniform environment of radiance 1.
EnvironmentMaps BuildEnvironment(const Texture *latlong);

// A sun found in a latlong (dome sun extraction): the light of the texels
// whose luminance is over a threshold, beyond it. Direction (towards it)
// in the dome's local space, the irradiance it gave in texture units, its
// angular diameter in degrees (from the texels' spread); none: zero
// irradiance.
struct ExtractedSun {
    float direction[3] = {0, 1, 0};
    float irradiance[3] = {0, 0, 0};
    float angle = 0;
};

// BuildEnvironment with every texel's luminance clamped to `threshold`
// (texture units) and what that took away as `sun` (when not null).
EnvironmentMaps BuildEnvironment(const Texture *latlong, float threshold, ExtractedSun *sun);

// The same from a cube: `faces` is RGBA floats (alpha ignored) in the
// layout of `data`'s level 0, `size` a power of two (reflection probes).
EnvironmentMaps BuildEnvironment(const float *faces, uint32_t size);

// Split-sum DFG terms (A, B) with f0 * A + B = directional albedo, indexed by
// (NoV, perceptual roughness). RG32F, size x size.
std::vector<float> BuildBrdfLut(uint32_t size);

} // namespace vkr
