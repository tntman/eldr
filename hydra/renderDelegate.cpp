#include "renderDelegate.h"

#include "pxr/imaging/hd/renderPass.h"
#include "pxr/imaging/hd/resourceRegistry.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace {

class HdVkRenderPass final : public HdRenderPass
{
public:
    using HdRenderPass::HdRenderPass;

protected:
    void _Execute(HdRenderPassStateSharedPtr const &,
                  TfTokenVector const &) override {}
};

const TfTokenVector _noTypes;

} // namespace

HdVkRenderDelegate::HdVkRenderDelegate(HdRenderSettingsMap const &settings)
    : HdRenderDelegate(settings)
    , _resourceRegistry(std::make_shared<HdResourceRegistry>())
{
}

const TfTokenVector &HdVkRenderDelegate::GetSupportedRprimTypes() const { return _noTypes; }
const TfTokenVector &HdVkRenderDelegate::GetSupportedSprimTypes() const { return _noTypes; }
const TfTokenVector &HdVkRenderDelegate::GetSupportedBprimTypes() const { return _noTypes; }

HdResourceRegistrySharedPtr HdVkRenderDelegate::GetResourceRegistry() const
{
    return _resourceRegistry;
}

HdRenderPassSharedPtr HdVkRenderDelegate::CreateRenderPass(
    HdRenderIndex *index, HdRprimCollection const &collection)
{
    return std::make_shared<HdVkRenderPass>(index, collection);
}

// Hydra only asks for types listed as supported, so these stay unreachable
// until steps 2-4 add mesh/camera/material/light/instancer support.
HdInstancer *HdVkRenderDelegate::CreateInstancer(HdSceneDelegate *, SdfPath const &) { return nullptr; }
void HdVkRenderDelegate::DestroyInstancer(HdInstancer *instancer) { delete instancer; }

HdRprim *HdVkRenderDelegate::CreateRprim(TfToken const &, SdfPath const &) { return nullptr; }
void HdVkRenderDelegate::DestroyRprim(HdRprim *rprim) { delete rprim; }

HdSprim *HdVkRenderDelegate::CreateSprim(TfToken const &, SdfPath const &) { return nullptr; }
HdSprim *HdVkRenderDelegate::CreateFallbackSprim(TfToken const &) { return nullptr; }
void HdVkRenderDelegate::DestroySprim(HdSprim *sprim) { delete sprim; }

HdBprim *HdVkRenderDelegate::CreateBprim(TfToken const &, SdfPath const &) { return nullptr; }
HdBprim *HdVkRenderDelegate::CreateFallbackBprim(TfToken const &) { return nullptr; }
void HdVkRenderDelegate::DestroyBprim(HdBprim *bprim) { delete bprim; }

void HdVkRenderDelegate::CommitResources(HdChangeTracker *) {}

PXR_NAMESPACE_CLOSE_SCOPE
