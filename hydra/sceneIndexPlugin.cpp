// Turns implicit prims (UsdGeomCube, Sphere, Cone, Cylinder, Capsule, Plane)
// into meshes for HdVk, same as hdEmbree does.
#include "pxr/pxr.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdVk_ImplicitSurfaceSceneIndexPlugin final : public HdSceneIndexPlugin
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
            inputScene,
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
    HdSceneIndexPluginRegistry::Define<HdVk_ImplicitSurfaceSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    // Keyed by the renderer's displayName in plugInfo.json.
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "HdVk", TfToken("HdVk_ImplicitSurfaceSceneIndexPlugin"),
        /*inputArgs=*/nullptr, /*insertionPhase=*/0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE
