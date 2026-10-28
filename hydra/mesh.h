#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/imaging/hd/mesh.h"
#include "pxr/imaging/hd/sceneDelegate.h"

#include <memory>
#include <optional>
#include <string>
#include <type_traits>

namespace vkr {
class Scene;
struct Mesh;
struct Primvar;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdVkSubdivision;
// Whether a gprim is a holdout (matte object): a non-zero constant primvar
// eldr:matte (which HdVk's matte scene index writes for the active
// UsdRenderPass's collection:matte), eldr:object:holdoutmode or Karma's
// karma:object:holdoutmode (1 matte; 2, background, is taken as matte).
bool HdVkHoldout(HdSceneDelegate *sceneDelegate, SdfPath const &id);

// A gprim's primvar as one T (bool, int, float or std::string; the first
// of an array, a token as a string, numbers cast), else nullopt.
template <typename T>
std::optional<T> HdVkPrimvarIfSet(HdSceneDelegate *sceneDelegate, SdfPath const &id, TfToken const &name)
{
    const VtValue v = sceneDelegate->Get(id, name);
    if (v.IsHolding<VtArray<T>>()) {
        const VtArray<T> &a = v.UncheckedGet<VtArray<T>>();
        return a.empty() ? std::nullopt : std::optional<T>(a[0]);
    }
    if constexpr (std::is_same_v<T, std::string>) {
        if (v.IsHolding<TfToken>()) {
            return v.UncheckedGet<TfToken>().GetString();
        }
    }
    const VtValue cast = VtValue::Cast<T>(v);
    return cast.IsHolding<T>() ? std::optional<T>(cast.UncheckedGet<T>()) : std::nullopt;
}

// HdVkPrimvarIfSet, else `fallback`.
template <typename T>
T HdVkPrimvar(HdSceneDelegate *sceneDelegate, SdfPath const &id, TfToken const &name, T fallback)
{
    return HdVkPrimvarIfSet<T>(sceneDelegate, id, name).value_or(fallback);
}

// A per-object attribute with its Eldr twin: primvars:eldr:<name> when set,
// else Karma's primvars:karma:<name> when set, else `fallback` (the USD
// standard or default). `name` is e.g. "object:mblur".
template <typename T>
T HdVkObjectPrimvar(HdSceneDelegate *sceneDelegate, SdfPath const &id, std::string const &name, T fallback)
{
    if (auto v = HdVkPrimvarIfSet<T>(sceneDelegate, id, TfToken("eldr:" + name))) {
        return *v;
    }
    return HdVkPrimvar<T>(sceneDelegate, id, TfToken("karma:" + name), fallback);
}

// The rays that see a gprim (vkr::Mesh::Rays) by the render visibility,
// primvars:eldr:object:rendervisibility (else Karma's karma:object:rendervisibility): a traceset expression such as
// "*", "primary|shadow", "-primary", "-diffuse&-reflect&-refract" or ""
// (unrenderable). Primary is the camera; diffuse, reflect and refract are
// Eldr's secondary rays together (off only when all three are). Volume and
// sss are ignored.
uint32_t HdVkRays(HdSceneDelegate *sceneDelegate, SdfPath const &id);

// The per-object motion blur switch, primvars:eldr:object:mblur (else Karma's
// karma:object:mblur): off,
// the gprim keeps no motion of its own (transform or deformation).
bool HdVkMotionBlur(HdSceneDelegate *sceneDelegate, SdfPath const &id);

// Deformation blur (eldr:deformationBlur, with sampled motion's shutter):
// a gprim's points' velocity per frame from their samples at shutter open
// and close (velocities, accelerations resolved into them by Velocity
// Blur), continued linearly as the camera's and the transforms' are. Empty
// when they don't move through it or when the point count isn't
// `pointCount` there.
VtVec3fArray HdVkShutterVelocities(HdSceneDelegate *sceneDelegate, SdfPath const &id, size_t pointCount);

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
    VtVec3fArray _velocities;  // deformation blur: per point, units per frame (empty: still)
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
    bool _FlattenIndexed();
    bool _FlattenRefined();
    int _refineLevel = 0;
    bool _asSubdivision = false;  // primvars:eldr:renderAsSubdivision
    std::shared_ptr<HdVkSubdivision> _subdivision;
    // The points changed after the first sync: keep _subdivision. A still
    // mesh frees it (its refiner holds every level: Kitchen_set at level 3,
    // husk's default complexity, took 8 GB).
    bool _animated = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
