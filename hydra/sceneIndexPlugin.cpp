// HdVk's scene index plugin, appended after UsdImaging (so after
// UsdSkelImaging) for this renderer:
// - turns implicit prims (UsdGeomCube, Sphere, Cone, Cylinder, Capsule,
//   Plane) into meshes, same as hdEmbree does;
// - skips UsdSkelImaging's CPU skinning: for a skinned mesh it hands the
//   ExtComputation inputs over as primvars (the same hydra:* primvars
//   UsdSkelImaging writes itself with HD_ENABLE_DEFERRED_SKINNING), so
//   HdVkMesh skins on the GPU and per frame only the skinning transforms
//   change. Authored normals come back the same way (UsdSkelImaging blocks
//   them on skinned meshes) and are skinned on the GPU too.
#include "pxr/pxr.h"
#include "pxr/imaging/hd/extComputationPrimvarsSchema.h"
#include "pxr/imaging/hd/extComputationSchema.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// UsdSkelImaging's computation and input names (usdSkelImaging/tokens.h),
// spelled out so HdVk doesn't link UsdImaging.
TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((aggregator, "skinningPointsInputAggregatorComputation"))
    ((computation, "skinningPointsComputation"))
    ((normalsAggregator, "skinningNormalsInputAggregatorComputation"))
    (restPoints)
    (restNormals)
    (hasFaceVaryingNormals)
    (geomBindXform)
    (influences)
    (numInfluencesPerComponent)
    (hasConstantInfluences)
    (skinningXforms)
    (skinningDualQuats)
    (skelLocalToWorld)
    (primWorldToLocal)
    (blendShapeOffsets)
    (blendShapeOffsetRanges)
    (numBlendShapeOffsetRanges)
    (blendShapeWeights));

TF_DECLARE_REF_PTRS(_SkinningSceneIndex);

class _SkinningSceneIndex final : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _SkinningSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input)
    {
        return TfCreateRefPtr(new _SkinningSceneIndex(input));
    }

    HdSceneIndexPrim GetPrim(SdfPath const &path) const override
    {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
        if (prim.primType != HdPrimTypeTokens->mesh || !prim.dataSource) {
            return prim;
        }
        const HdContainerDataSourceHandle aggregator =
            _GetInputSceneIndex()->GetPrim(path.AppendChild(_tokens->aggregator)).dataSource;
        const HdContainerDataSourceHandle computation =
            _GetInputSceneIndex()->GetPrim(path.AppendChild(_tokens->computation)).dataSource;
        if (!aggregator || !computation) {
            return prim;
        }
        const HdSampledDataSourceContainerSchema rest =
            HdExtComputationSchema::GetFromParent(aggregator).GetInputValues();
        const HdSampledDataSourceContainerSchema pose =
            HdExtComputationSchema::GetFromParent(computation).GetInputValues();

        auto primvar = [](HdSampledDataSourceHandle const &value, TfToken const &interpolation,
                          TfToken const &role) -> HdDataSourceBaseHandle {
            return HdPrimvarSchema::Builder()
                .SetPrimvarValue(value)
                .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(interpolation))
                .SetRole(HdPrimvarSchema::BuildRoleDataSource(role))
                .Build();
        };
        const TfToken &constant = HdPrimvarSchemaTokens->constant;
        TfTokenVector names = {
            HdTokens->points,
            HdSkinningSkelInputTokens->geomBindTransform,
            HdSkinningInputTokens->influences,
            HdSkinningInputTokens->numInfluencesPerComponent,
            HdSkinningInputTokens->hasConstantInfluences,
            HdSkinningInputTokens->skinningXforms,
            HdSkinningInputTokens->skelLocalToCommonSpace,
            HdSkinningInputTokens->commonSpaceToPrimLocal,
            HdSkinningInputTokens->blendShapeOffsets,
            HdSkinningInputTokens->blendShapeOffsetRanges,
            HdSkinningInputTokens->numBlendShapeOffsetRanges,
            HdSkinningInputTokens->blendShapeWeights,
        };
        std::vector<HdDataSourceBaseHandle> values = {
            primvar(rest.Get(_tokens->restPoints), HdPrimvarSchemaTokens->vertex, HdPrimvarRoleTokens->point),
            primvar(rest.Get(_tokens->geomBindXform), constant, TfToken()),
            primvar(rest.Get(_tokens->influences), constant, TfToken()),
            primvar(rest.Get(_tokens->numInfluencesPerComponent), constant, TfToken()),
            primvar(rest.Get(_tokens->hasConstantInfluences), constant, TfToken()),
            primvar(pose.Get(_tokens->skinningXforms), constant, TfToken()),
            primvar(pose.Get(_tokens->skelLocalToWorld), constant, TfToken()),
            primvar(pose.Get(_tokens->primWorldToLocal), constant, TfToken()),
            primvar(rest.Get(_tokens->blendShapeOffsets), constant, TfToken()),
            primvar(rest.Get(_tokens->blendShapeOffsetRanges), constant, TfToken()),
            primvar(rest.Get(_tokens->numBlendShapeOffsetRanges), constant, TfToken()),
            primvar(pose.Get(_tokens->blendShapeWeights), constant, TfToken()),
        };
        // Authored normals are blocked too; they reach only the normals
        // aggregator, which UsdSkelImaging serves for every skinned mesh
        // (announced as a prim only with USDSKELIMAGING_ENABLE_NORMAL_COMPUTATIONS,
        // without which Storm shades skinned meshes faceted).
        const HdSampledDataSourceContainerSchema normalsInputs =
            HdExtComputationSchema::GetFromParent(
                _GetInputSceneIndex()->GetPrim(path.AppendChild(_tokens->normalsAggregator)).dataSource)
                .GetInputValues();
        if (const HdSampledDataSourceHandle normals = normalsInputs.Get(_tokens->restNormals)) {
            const HdSampledDataSourceHandle faceVarying = normalsInputs.Get(_tokens->hasFaceVaryingNormals);
            names.push_back(HdTokens->normals);
            values.push_back(primvar(normals,
                                     faceVarying && faceVarying->GetValue(0.0f).GetWithDefault<bool>(false)
                                         ? HdPrimvarSchemaTokens->faceVarying
                                         : HdPrimvarSchemaTokens->vertex,
                                     HdPrimvarRoleTokens->normal));
        }
        // The skinning method, as UsdSkelImaging's deferred skinning
        // announces it: its pose inputs have dual quaternions only for
        // "dualQuaternion" (HdVk takes them from the joint matrices).
        names.push_back(HdSkinningInputTokens->numSkinningMethod);
        values.push_back(primvar(HdRetainedTypedSampledDataSource<int>::New(
                                     pose.Get(_tokens->skinningDualQuats) ? 1 : 0),
                                 constant, TfToken()));
        prim.dataSource = HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(
                HdPrimvarsSchema::GetSchemaToken(),
                HdRetainedContainerDataSource::New(names.size(), names.data(), values.data()),
                HdExtComputationPrimvarsSchema::GetSchemaToken(),
                HdBlockDataSource::New()),
            prim.dataSource);
        return prim;
    }

    SdfPathVector GetChildPrimPaths(SdfPath const &path) const override
    {
        return _GetInputSceneIndex()->GetChildPrimPaths(path);
    }

protected:
    _SkinningSceneIndex(HdSceneIndexBaseRefPtr const &input)
        : HdSingleInputFilteringSceneIndexBase(input)
    {
    }

    void _PrimsAdded(HdSceneIndexBase const &, HdSceneIndexObserver::AddedPrimEntries const &entries) override
    {
        // A mesh that gains its computations becomes skinned.
        HdSceneIndexObserver::DirtiedPrimEntries dirtied;
        for (const auto &e : entries) {
            if (_IsComputation(e.primPath)) {
                dirtied.emplace_back(e.primPath.GetParentPath(),
                                     HdDataSourceLocatorSet{HdPrimvarsSchema::GetDefaultLocator(),
                                                            HdExtComputationPrimvarsSchema::GetDefaultLocator()});
            }
        }
        _SendPrimsAdded(entries);
        if (!dirtied.empty()) {
            _SendPrimsDirtied(dirtied);
        }
    }

    void _PrimsRemoved(HdSceneIndexBase const &, HdSceneIndexObserver::RemovedPrimEntries const &entries) override
    {
        _SendPrimsRemoved(entries);
    }

    // Edits of the computations' inputs become edits of the mesh's primvars:
    // per frame the pose computation changes, which only dirties the
    // skinning transforms (DirtyPrimvar), not the points.
    void _PrimsDirtied(HdSceneIndexBase const &, HdSceneIndexObserver::DirtiedPrimEntries const &entries) override
    {
        HdSceneIndexObserver::DirtiedPrimEntries dirtied = entries;
        for (const auto &e : entries) {
            if (!_IsComputation(e.primPath)) {
                continue;
            }
            auto primvar = [](TfToken const &name) { return HdPrimvarsSchema::GetDefaultLocator().Append(name); };
            HdDataSourceLocatorSet locators;
            if (e.primPath.GetNameToken() == _tokens->normalsAggregator) {
                locators = {primvar(HdTokens->normals)};
            } else if (e.primPath.GetNameToken() == _tokens->aggregator) {
                locators = {primvar(HdTokens->points), primvar(HdSkinningSkelInputTokens->geomBindTransform),
                            primvar(HdSkinningInputTokens->influences),
                            primvar(HdSkinningInputTokens->numInfluencesPerComponent),
                            primvar(HdSkinningInputTokens->hasConstantInfluences),
                            primvar(HdSkinningInputTokens->blendShapeOffsets),
                            primvar(HdSkinningInputTokens->blendShapeOffsetRanges),
                            primvar(HdSkinningInputTokens->numBlendShapeOffsetRanges)};
            } else {
                locators = {primvar(HdSkinningInputTokens->skinningXforms),
                            primvar(HdSkinningInputTokens->skelLocalToCommonSpace),
                            primvar(HdSkinningInputTokens->commonSpaceToPrimLocal),
                            primvar(HdSkinningInputTokens->blendShapeWeights)};
            }
            dirtied.emplace_back(e.primPath.GetParentPath(), locators);
        }
        _SendPrimsDirtied(dirtied);
    }

private:
    static bool _IsComputation(SdfPath const &path)
    {
        return path.IsPrimPath() && (path.GetNameToken() == _tokens->aggregator ||
                                     path.GetNameToken() == _tokens->computation ||
                                     path.GetNameToken() == _tokens->normalsAggregator);
    }
};

} // namespace

class HdVk_SceneIndexPlugin final : public HdSceneIndexPlugin
{
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        HdSceneIndexBaseRefPtr const &inputScene,
        HdContainerDataSourceHandle const &) override
    {
        HdDataSourceBaseHandle const toMesh =
            HdRetainedTypedSampledDataSource<TfToken>::New(
                HdsiImplicitSurfaceSceneIndexTokens->toMesh);
        return HdsiImplicitSurfaceSceneIndex::New(
            _SkinningSceneIndex::New(inputScene),
            HdRetainedContainerDataSource::New(
                HdPrimTypeTokens->sphere, toMesh,
                HdPrimTypeTokens->cube, toMesh,
                HdPrimTypeTokens->cone, toMesh,
                HdPrimTypeTokens->cylinder, toMesh,
                HdPrimTypeTokens->capsule, toMesh,
                HdPrimTypeTokens->plane, toMesh));
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<HdVk_SceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    // Keyed by the renderer's displayName in plugInfo.json.
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "HdVk", TfToken("HdVk_SceneIndexPlugin"),
        /*inputArgs=*/nullptr, /*insertionPhase=*/0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE
