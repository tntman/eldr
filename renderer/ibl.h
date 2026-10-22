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

// Split-sum DFG terms (A, B) with f0 * A + B = directional albedo, indexed by
// (NoV, perceptual roughness). RG32F, size x size.
std::vector<float> BuildBrdfLut(uint32_t size);

} // namespace vkr
