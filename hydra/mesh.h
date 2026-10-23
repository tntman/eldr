#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
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
    // Hydra updates the tag (purpose) outside Sync.
    void UpdateRenderTag(HdSceneDelegate *delegate, HdRenderParam *renderParam) override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &, HdDirtyBits *) override {}

private:
    void _Flatten();
    void _SplitSubsets();
    void _SyncSubsets(HdSceneDelegate *sceneDelegate);
    void _UpdateJoints();
    bool _Skinned() const;

    vkr::Scene *_scene;
    vkr::Mesh *_mesh;  // the faces outside material subsets
    // GeomSubsets with a material, each drawn as its own vkr::Mesh (as
    // Storm's draw items): their triangles and _mesh's shared state.
    std::vector<vkr::Mesh *> _subsetMeshes;
    std::vector<SdfPath> _subsetMaterials;

    // Hydra-side data kept to rebuild the per-corner arrays when any part changes.
    HdMeshTopology _topology;
    VtVec3iArray _triangles;
    VtIntArray _primitiveParams;
    VtVec3fArray _points;
    VtValue _normals, _uvs, _colors;
    TfToken _uvName;  // the primvar _uvs came from (see Sync)
    HdInterpolation _normalsInterp = HdInterpolationVertex;
    HdInterpolation _uvsInterp = HdInterpolationVertex;
    HdInterpolation _colorsInterp = HdInterpolationConstant;

    // GPU skinning inputs, the hydra:* primvars (see sceneIndexPlugin.cpp).
    VtVec2fArray _influences;        // (joint, weight) per point, or once if constant
    int _influencesPerPoint = 0;
    bool _constantInfluences = false;
    GfMatrix4d _geomBind{1.0};
    VtMatrix4fArray _skinningXforms; // per joint, or per instance and joint
    int _numJoints = 0;              // > 0: _skinningXforms holds every instance
    GfMatrix4d _skelToPrim{1.0};     // skelLocalToWorld * primWorldToLocal
    VtVec4fArray _blendOffsets;      // xyz offset, shape index
    VtVec2iArray _blendRanges;       // per point: [first, end) into _blendOffsets
    int _blendRangeCount = 0;        // points with a range
    VtFloatArray _blendWeights;      // per shape, or per instance and shape
    int _numBlendShapeWeights = 0;   // > 0: _blendWeights holds every instance
};

PXR_NAMESPACE_CLOSE_SCOPE
