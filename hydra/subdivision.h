#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/enums.h"
#include "pxr/imaging/hd/meshTopology.h"

#include <opensubdiv/version.h>

#include <memory>
#include <vector>

namespace OpenSubdiv { namespace OPENSUBDIV_VERSION { namespace Far { class TopologyRefiner; } } }
namespace vkr {
struct Mesh;
}

PXR_NAMESPACE_OPEN_SCOPE

// Render settings (eldr:subdivision, eldr:maxRefineLevel) defaults: the
// display style's refine level (usdview's Complexity) up to this.
constexpr bool kHdVkSubdivision = true;
constexpr int kHdVkMaxRefineLevel = 3;

// Uniform subdivision with OpenSubdiv (through pxOsd, so creases, corners,
// holes, interpolateBoundary and faceVaryingLinearInterpolation apply): a
// cage refined `level` times, catmullClark and bilinear into quads, loop
// into triangles. Vertex data is projected onto the limit surface. A mesh
// keeps one so animated points refine without rebuilding the refiner.
class HdVkSubdivision
{
public:
    // Refines `cage` with one face-varying channel per entry of `fvar`
    // (indices per face-vertex); reuses the last refinement while all three
    // are unchanged. False if OpenSubdiv rejects the topology.
    bool Update(HdMeshTopology const &cage, int level, std::vector<VtIntArray> const &fvar);

    // The refined faces: right-handed, holes and GeomSubsets carried over.
    HdMeshTopology const &Topology() const { return _topology; }
    int Channels() const { return int(_fvar.size()); }

    // A primvar of `n` (1..4) floats per element refined per element:
    // vertex (smooth, on the limit) and varying (linear) per refined vertex,
    // face-varying per value of channel `channel` (see Weld), uniform per
    // refined face; constant ones stay. False if `values` is short.
    bool Refine(std::vector<float> const &values, uint32_t n, HdInterpolation interp, int channel,
                std::vector<float> *out) const;
    // Limit positions and, unless bilinear, normals per refined vertex.
    bool RefinePoints(VtVec3fArray const &points, VtVec3fArray *out, VtVec3fArray *normals) const;
    // Per cage vertex, values by key (joints' weights in x, blend shapes'
    // offsets): per refined vertex their weighted sums.
    using Sparse = std::vector<std::pair<int, GfVec3f>>;
    std::vector<Sparse> RefineSparse(std::vector<Sparse> const &values) const;

    // The refined faces as indexed triangles over vertices that are a
    // refined vertex with one value per face-varying channel (and one face,
    // `perFace`): a vertex per distinct combination.
    struct Triangles {
        std::vector<int> vertex;  // per vertex: its refined vertex
        std::vector<int> keys;    // per vertex: its value per channel, then its face (-1 unless perFace)
        std::vector<uint32_t> indices;  // 3 per triangle
        std::vector<int> faces;   // per triangle: its refined face
    };
    Triangles Triangulate(bool perFace) const;

    // Face-varying values (n floats per face-vertex of `cage`) as a channel:
    // returns indices per face-vertex into `values`, which keeps one value
    // per vertex and distinct value (seams are where values differ).
    static VtIntArray Weld(HdMeshTopology const &cage, std::vector<float> *values, uint32_t n);

private:
    HdMeshTopology _cage;
    int _level = 0;
    std::vector<VtIntArray> _fvar;
    std::shared_ptr<OpenSubdiv::OPENSUBDIV_VERSION::Far::TopologyRefiner> _refiner;
    HdMeshTopology _topology;
    std::vector<int> _faceParents;  // per refined face: the cage face
};

// The triangles `tris` of `m` into `d`, with the vertices they use, when
// `m` is indexed (a refined subdivision surface); else clears d's indices.
void HdVkSplitIndexed(vkr::Mesh const &m, std::vector<size_t> const &tris, vkr::Mesh *d);

PXR_NAMESPACE_CLOSE_SCOPE
