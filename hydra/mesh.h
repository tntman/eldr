#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/mesh.h"

namespace vkr {
class Scene;
struct Mesh;
}

PXR_NAMESPACE_OPEN_SCOPE

// Sync() only copies scene data into its vkr::Mesh; no Vulkan here.
class HdVkMesh final : public HdMesh
{
public:
    HdVkMesh(SdfPath const &id, vkr::Scene *scene);
    ~HdVkMesh() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &, HdDirtyBits *) override {}

private:
    void _Flatten();

    vkr::Scene *_scene;
    vkr::Mesh *_mesh;

    // Hydra-side data kept to rebuild the per-corner arrays when any part changes.
    HdMeshTopology _topology;
    VtVec3iArray _triangles;
    VtIntArray _primitiveParams;
    VtVec3fArray _points;
    VtValue _normals, _uvs;
    HdInterpolation _normalsInterp = HdInterpolationVertex;
    HdInterpolation _uvsInterp = HdInterpolationVertex;
};

PXR_NAMESPACE_CLOSE_SCOPE
