#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/imaging/hd/mesh.h"

#include <memory>

namespace vkr {
class Scene;
struct Mesh;
struct Primvar;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdVkSubdivision;

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
    std::vector<TfToken> _ProgramPrimvarNames(HdSceneDelegate *sceneDelegate) const;
    void _ApplyInstancePrimvars(vkr::Mesh &mesh) const;

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
    bool _dualQuaternion = false;    // hydra:numSkinningMethod 1

    // Primvars the material programs read (the mesh's material's and its
    // subsets'), as authored on the mesh, and per instance from its instancer.
    struct ProgramPrimvar {
        TfToken name;
        HdInterpolation interp = HdInterpolationConstant;
        uint32_t components = 1;
        std::vector<float> values;
        bool operator==(ProgramPrimvar const &o) const
        {
            return name == o.name && interp == o.interp && components == o.components && values == o.values;
        }
    };
    std::vector<TfToken> _programPrimvarNames;
    std::vector<ProgramPrimvar> _programPrimvars;
    std::vector<vkr::Primvar> _instancePrimvars;

    // Subdivision surfaces (subdivision.cpp): _Flatten for the cage refined
    // to _refineLevel (0: the cage), the refiner kept for animated points.
    int _RefineLevel(HdSceneDelegate *sceneDelegate) const;
    bool _FlattenRefined();
    int _refineLevel = 0;
    std::shared_ptr<HdVkSubdivision> _subdivision;
};

PXR_NAMESPACE_CLOSE_SCOPE
