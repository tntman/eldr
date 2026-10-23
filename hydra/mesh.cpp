#include "mesh.h"

#include "instancer.h"
#include "material.h"
#include "scene.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/vertexAdjacency.h"
#include "pxr/imaging/pxOsd/tokens.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(_tokens, (st));

// Finds a primvar's interpolation; false if the prim doesn't have it.
bool FindPrimvar(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name,
                 HdInterpolation *interp)
{
    for (int i = 0; i < HdInterpolationCount; ++i) {
        for (const HdPrimvarDescriptor &d :
             sd->GetPrimvarDescriptors(id, HdInterpolation(i))) {
            if (d.name == name) {
                *interp = HdInterpolation(i);
                return true;
            }
        }
    }
    return false;
}

// Expands a primvar of any interpolation to one value per triangle corner.
// Returns false (and leaves `out` empty) if the data doesn't fit the topology.
template <typename T>
bool FlattenPrimvar(VtValue const &value, HdInterpolation interp, HdMeshUtil const &util,
                    VtVec3iArray const &triangles, VtIntArray const &primitiveParams,
                    HdType type, std::vector<float> *out)
{
    out->clear();
    if (!value.IsHolding<VtArray<T>>()) {
        return false;
    }
    const VtArray<T> &src = value.UncheckedGet<VtArray<T>>();
    const size_t corners = triangles.size() * 3;
    constexpr size_t dim = T::dimension;
    std::vector<float> result(corners * dim);
    auto put = [&](size_t corner, const T &v) {
        std::copy(v.data(), v.data() + dim, result.data() + corner * dim);
    };

    switch (interp) {
    case HdInterpolationConstant:
        if (src.empty()) {
            return false;
        }
        for (size_t c = 0; c < corners; ++c) {
            put(c, src[0]);
        }
        break;
    case HdInterpolationUniform:
        for (size_t t = 0; t < triangles.size(); ++t) {
            const size_t face = size_t(
                HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(primitiveParams[t]));
            if (face >= src.size()) {
                return false;
            }
            put(t * 3, src[face]);
            put(t * 3 + 1, src[face]);
            put(t * 3 + 2, src[face]);
        }
        break;
    case HdInterpolationVertex:
    case HdInterpolationVarying:
        for (size_t t = 0; t < triangles.size(); ++t) {
            for (int k = 0; k < 3; ++k) {
                const int i = triangles[t][k];
                if (i < 0 || size_t(i) >= src.size()) {
                    return false;
                }
                put(t * 3 + k, src[i]);
            }
        }
        break;
    case HdInterpolationFaceVarying: {
        VtValue triangulated;
        const HdMeshComputationResult r = util.ComputeTriangulatedFaceVaryingPrimvar(
            src.cdata(), int(src.size()), type, &triangulated);
        if (r == HdMeshComputationResult::Error) {
            return false;
        }
        const VtArray<T> &perCorner = r == HdMeshComputationResult::Success
            ? triangulated.Get<VtArray<T>>() : src;
        if (perCorner.size() != corners) {
            return false;
        }
        for (size_t c = 0; c < corners; ++c) {
            put(c, perCorner[c]);
        }
        break;
    }
    default:
        return false;
    }
    *out = std::move(result);
    return true;
}

} // namespace

HdVkMesh::HdVkMesh(SdfPath const &id, vkr::Scene *scene)
    : HdMesh(id)
    , _scene(scene)
    , _mesh(scene->AddMesh())
{
}

HdVkMesh::~HdVkMesh()
{
    _scene->RemoveMesh(_mesh);
}

HdDirtyBits HdVkMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::InitRepr | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex;
}

// ponytail: renders the subdivision cage, reads only the "st" primvar for
// texcoords and constant displayColor, ignores render tags.
void HdVkMesh::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *,
                    HdDirtyBits *dirtyBits, TfToken const &)
{
    const SdfPath &id = GetId();
    vkr::Mesh &m = *_mesh;
    bool geometryDirty = false;
    m.primId = GetPrimId();  // assigned by the render index after construction

    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
        _topology = HdMeshTopology(GetMeshTopology(sceneDelegate), 0);
        HdMeshUtil(&_topology, id).ComputeTriangleIndices(&_triangles, &_primitiveParams);
        geometryDirty = true;
    }
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        const VtValue value = GetPoints(sceneDelegate);
        _points = value.IsHolding<VtVec3fArray>() ? value.UncheckedGet<VtVec3fArray>()
                                                  : VtVec3fArray();
        geometryDirty = true;
    }
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->normals) ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, _tokens->st)) {
        _normals = FindPrimvar(sceneDelegate, id, HdTokens->normals, &_normalsInterp)
            ? sceneDelegate->Get(id, HdTokens->normals) : VtValue();
        _uvs = FindPrimvar(sceneDelegate, id, _tokens->st, &_uvsInterp)
            ? sceneDelegate->Get(id, _tokens->st) : VtValue();
        geometryDirty = true;
    }
    if (geometryDirty) {
        _Flatten();
    }

    // Instancers are synced by the rprims that use them (as in hdEmbree);
    // the helper locks, so parallel rprim syncs are fine.
    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstanceIndexDirty(*dirtyBits, id)) {
        const GfMatrix4d xf = sceneDelegate->GetTransform(id);
        VtMatrix4dArray instances(1, GfMatrix4d(1));
        if (!GetInstancerId().IsEmpty()) {
            auto *instancer = static_cast<HdVkInstancer *>(
                sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId()));
            instances = instancer ? instancer->ComputeInstanceTransforms(id) : VtMatrix4dArray();
        }
        m.transforms.resize(instances.size() * 16);
        for (size_t i = 0; i < instances.size(); ++i) {
            const GfMatrix4f world(xf * instances[i]);  // prototype first, then instance
            std::copy(world.data(), world.data() + 16, m.transforms.data() + i * 16);
        }
    }

    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
        m.visible = IsVisible();
    }

    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->displayColor)) {
        const VtValue value = sceneDelegate->Get(id, HdTokens->displayColor);
        if (value.IsHolding<VtVec3fArray>() && !value.UncheckedGet<VtVec3fArray>().empty()) {
            const GfVec3f &c = value.UncheckedGet<VtVec3fArray>()[0];
            std::copy(c.data(), c.data() + 3, m.color);
        }
    }

    // Sprims sync before rprims, so the material already exists here.
    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        const auto *material = static_cast<const HdVkMaterial *>(
            sceneDelegate->GetRenderIndex().GetSprim(
                HdPrimTypeTokens->material, sceneDelegate->GetMaterialId(id)));
        m.material = material ? material->Get() : nullptr;
    }

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

void HdVkMesh::_Flatten()
{
    vkr::Mesh &m = *_mesh;
    m.geometryDirty = true;
    const HdMeshUtil util(&_topology, GetId());

    if (!FlattenPrimvar<GfVec3f>(VtValue(_points), HdInterpolationVertex, util, _triangles,
                                 _primitiveParams, HdTypeFloatVec3, &m.points)) {
        // Indices past the points array: draw nothing rather than garbage.
        m.normals.clear();
        m.uvs.clear();
        return;
    }

    // Authored normals win. Otherwise cage-only schemes stay faceted (empty
    // normals: the shader uses the geometric normal) and subdivision schemes
    // get smooth vertex normals as a stand-in for the limit surface.
    if (!FlattenPrimvar<GfVec3f>(_normals, _normalsInterp, util, _triangles,
                                 _primitiveParams, HdTypeFloatVec3, &m.normals)) {
        const TfToken scheme = _topology.GetScheme();
        if (scheme != PxOsdOpenSubdivTokens->none && scheme != PxOsdOpenSubdivTokens->bilinear) {
            Hd_VertexAdjacency adjacency;
            adjacency.BuildAdjacencyTable(&_topology);
            const VtVec3fArray smooth = Hd_SmoothNormals::ComputeSmoothNormals(
                &adjacency, int(_points.size()), _points.cdata());
            FlattenPrimvar<GfVec3f>(VtValue(smooth), HdInterpolationVertex, util, _triangles,
                                    _primitiveParams, HdTypeFloatVec3, &m.normals);
        }
    }

    FlattenPrimvar<GfVec2f>(_uvs, _uvsInterp, util, _triangles, _primitiveParams,
                            HdTypeFloatVec2, &m.uvs);
}

PXR_NAMESPACE_CLOSE_SCOPE
