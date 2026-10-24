#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace vkr {

// One level of detail of an indexed triangle mesh: triangles over the same
// vertices (so attributes, skin weights and blend shapes stay valid) and
// how far the surface (or its attributes, see BuildLods) moved, relative
// to the mesh's bounding radius.
struct LodLevel {
    std::vector<uint32_t> indices;
    float error = 0;
};

// Coarser levels of `indices` (3 per triangle), each about half the
// triangles of the one before, up to `maxLevels`, by quadric error edge
// collapses onto existing vertices. Stops where halving would move the
// surface more than `maxError` (relative) or stalls. `vertices`: `stride`
// floats per vertex, xyz and then shading attributes (normal, uv, ...)
// scaled to distances: a collapse that changes an interpolated attribute
// by 1 costs as much as one that moves the surface 1 unit (Hoppe's
// attribute quadrics), so flat but smoothly shaded or textured regions
// keep their vertices. `difference` (optional): how much two vertices'
// skin weights differ, 0..1; collapsing a vertex next to one that differs
// by d counts as moving the surface d times their distance (what a joint
// between them bent by a radian moves; the rest pose hides where it bends).
// ponytail: vertices on borders and attribute seams (an edge one triangle
// uses) never move, so meshes cut into many uv islands simplify little;
// welding seams by position (meshoptimizer's wedges) lifts that.
std::vector<LodLevel> BuildLods(const float *vertices, size_t stride, size_t vertexCount,
                                const std::vector<uint32_t> &indices, uint32_t maxLevels, float maxError,
                                const std::function<float(uint32_t, uint32_t)> &difference = nullptr);

} // namespace vkr
