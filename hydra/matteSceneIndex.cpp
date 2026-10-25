// Holdouts from UsdRenderPass's matte collection. USD's convention for
// matte objects ("render with zero alpha") is the active render pass's
// collection:matte (usdview: a RenderPass prim's "Set As Active Render
// Pass"; usdrecord --renderPassPrimPath). Hydra hands a delegate no matte
// state of its own (HdsiRenderPassPruneSceneIndex applies only the prune
// collection), so this scene index does for HdVk what hdPrman's
// HdPrman_RenderPassVisibilityAndMatteSceneIndexPlugin does: gprims in the
// collection get the constant primvar eldr:matte = 1, Eldr's own channel,
// which HdVkMesh reads (as it does Karma's karma:object:holdoutmode).
// It sits after the app's scene globals (insertion phase 1), so it sees
// the active render pass and is told when it changes.
// ponytail: whole gprims only; instances matched through an instancer's
// instanceLocations stay as they are.
#include "pxr/pxr.h"
#include "pxr/imaging/hd/collectionExpressionEvaluator.h"
#include "pxr/imaging/hd/collectionsSchema.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/overlayContainerDataSource.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/sceneIndexPrimView.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/utils.h"

#include <optional>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (matte)
    ((eldrMatte, "eldr:matte")));

TF_DECLARE_REF_PTRS(_MatteSceneIndex);

class _MatteSceneIndex final : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _MatteSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input)
    {
        return TfCreateRefPtr(new _MatteSceneIndex(input));
    }

    HdSceneIndexPrim GetPrim(SdfPath const &path) const override
    {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
        if (_eval && !path.IsEmpty() && prim.dataSource && HdPrimTypeIsGprim(prim.primType) &&
            _eval->Match(path)) {
            static const HdContainerDataSourceHandle matte = HdRetainedContainerDataSource::New(
                HdPrimvarsSchema::GetSchemaToken(),
                HdRetainedContainerDataSource::New(
                    _tokens->eldrMatte,
                    HdPrimvarSchema::Builder()
                        .SetPrimvarValue(HdRetainedTypedSampledDataSource<int>::New(1))
                        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(
                            HdPrimvarSchemaTokens->constant))
                        .Build()));
            prim.dataSource = HdOverlayContainerDataSource::New(matte, prim.dataSource);
        }
        return prim;
    }

    SdfPathVector GetChildPrimPaths(SdfPath const &path) const override
    {
        return _GetInputSceneIndex()->GetChildPrimPaths(path);
    }

protected:
    explicit _MatteSceneIndex(HdSceneIndexBaseRefPtr const &input)
        : HdSingleInputFilteringSceneIndexBase(input)
    {
        _Update(nullptr);
    }

    void _PrimsAdded(HdSceneIndexBase const &, HdSceneIndexObserver::AddedPrimEntries const &entries) override
    {
        HdSceneIndexObserver::DirtiedPrimEntries dirtied;
        if (_AffectsPass(entries)) {
            _Update(&dirtied);
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

    void _PrimsDirtied(HdSceneIndexBase const &, HdSceneIndexObserver::DirtiedPrimEntries const &entries) override
    {
        HdSceneIndexObserver::DirtiedPrimEntries dirtied = entries;
        if (_AffectsPass(entries)) {
            _Update(&dirtied);
        }
        _SendPrimsDirtied(dirtied);
    }

private:
    // The scene globals live on the root; the pass's collections on its prim.
    template <typename Entries>
    bool _AffectsPass(Entries const &entries) const
    {
        for (const auto &e : entries) {
            if (e.primPath.IsAbsoluteRootPath() || e.primPath == _pass) {
                return true;
            }
        }
        return false;
    }

    // Reads the active pass's matte collection; when it changed, every
    // gprim's primvars are dirtied (into `dirtied`, if given).
    void _Update(HdSceneIndexObserver::DirtiedPrimEntries *dirtied)
    {
        const HdSceneIndexBaseRefPtr input = _GetInputSceneIndex();
        SdfPath pass;
        if (const HdPathDataSourceHandle ds = HdSceneGlobalsSchema::GetFromSceneIndex(input).GetActiveRenderPassPrim()) {
            pass = ds->GetTypedValue(0.0);
        }
        SdfPathExpression expr;
        std::optional<HdCollectionExpressionEvaluator> eval;
        if (!pass.IsEmpty()) {
            if (HdCollectionsSchema collections = HdCollectionsSchema::GetFromParent(input->GetPrim(pass).dataSource)) {
                HdsiUtilsCompileCollection(collections, _tokens->matte, input, &expr, &eval);
            }
        }
        _pass = pass;
        if (expr == _expr && eval.has_value() == _eval.has_value()) {
            return;
        }
        _expr = expr;
        _eval = std::move(eval);
        if (!dirtied) {
            return;
        }
        // All primvars: one appearing changes their names, which caching
        // downstream refreshes only when their container is dirtied.
        static const HdDataSourceLocatorSet locators{HdPrimvarsSchema::GetDefaultLocator()};
        for (const SdfPath &path : HdSceneIndexPrimView(input)) {
            if (HdPrimTypeIsGprim(input->GetPrim(path).primType)) {
                dirtied->emplace_back(path, locators);
            }
        }
    }

    SdfPath _pass;
    SdfPathExpression _expr;
    std::optional<HdCollectionExpressionEvaluator> _eval;
};

} // namespace

class HdVk_MatteSceneIndexPlugin final : public HdSceneIndexPlugin
{
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(HdSceneIndexBaseRefPtr const &inputScene,
                                             HdContainerDataSourceHandle const &) override
    {
        return _MatteSceneIndex::New(inputScene);
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<HdVk_MatteSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    // After the app's scene globals (phase 0), which name the active pass.
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        "Eldr", TfToken("HdVk_MatteSceneIndexPlugin"),
        /*inputArgs=*/nullptr, /*insertionPhase=*/1, HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

PXR_NAMESPACE_CLOSE_SCOPE
