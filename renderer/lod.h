#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace vkr {

// One level of detail of an indexed triangle mesh: triangles over the same
// vertices (so attributes, skin weights and blend shapes stay valid) and
// the largest distance a surface point moved, relative to the mesh's
// bounding radius.
struct LodLevel {
    std::vector<uint32_t> indices;
    float error = 0;
};

// Coarser levels of `indices` (3 per triangle), each about half the
// triangles of the one before, up to `maxLevels`, by quadric error edge
// collapses onto existing vertices. Stops where halving would move the
// surface more than `maxError` (relative) or stalls. `positions`: xyz at
// `stride` floats per vertex. `difference` (optional): how much two
// vertices' skin weights differ, 0..1; merging a difference of 1 counts
// as moving a quarter of the bounding radius (a skinned mesh's rest pose
// hides where it bends).
// ponytail: vertices on borders and attribute seams (an edge one triangle
// uses) never move, so meshes cut into many uv islands simplify little;
// welding seams by position (meshoptimizer's wedges) lifts that.
std::vector<LodLevel> BuildLods(const float *positions, size_t stride, size_t vertexCount,
                                const std::vector<uint32_t> &indices, uint32_t maxLevels, float maxError,
                                const std::function<float(uint32_t, uint32_t)> &difference = nullptr);

} // namespace vkr
