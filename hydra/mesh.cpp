#include "mesh.h"

#include "scene.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

PXR_NAMESPACE_OPEN_SCOPE

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
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyPrimvar;
}

// ponytail: ignores refinement, normals, per-vertex/face displayColor,
// render tags and instancers. Materials (step 3) and instancing (step 4) add them.
void HdVkMesh::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *,
                    HdDirtyBits *dirtyBits, TfToken const &)
{
    const SdfPath &id = GetId();
    vkr::Mesh &m = *_mesh;

    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
        const HdMeshTopology topology = GetMeshTopology(sceneDelegate);
        VtVec3iArray triangles;
        VtIntArray primitiveParams;
        HdMeshUtil(&topology, id).ComputeTriangleIndices(&triangles, &primitiveParams);
        const auto *first = reinterpret_cast<const uint32_t *>(triangles.cdata());
        m.indices.assign(first, first + triangles.size() * 3);
        m.geometryDirty = true;
    }

    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        const VtValue value = GetPoints(sceneDelegate);
        if (value.IsHolding<VtVec3fArray>()) {
            const VtVec3fArray &points = value.UncheckedGet<VtVec3fArray>();
            const float *first = points.empty() ? nullptr : points.cdata()->data();
            m.points.assign(first, first + points.size() * 3);
        } else {
            m.points.clear();
        }
        m.geometryDirty = true;
    }

    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
        const GfMatrix4f xf(sceneDelegate->GetTransform(id));
        std::copy(xf.data(), xf.data() + 16, m.transform);
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

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

PXR_NAMESPACE_CLOSE_SCOPE
