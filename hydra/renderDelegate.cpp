#include "renderDelegate.h"

#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh.h"
#include "renderBuffer.h"
#include "renderPass.h"
#include "renderer.h"

#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"

#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdVkRenderSettingsTokens, HDVK_RENDER_SETTING_TOKENS);

namespace {

const TfTokenVector _rprimTypes = {HdPrimTypeTokens->mesh};
const TfTokenVector _sprimTypes = {HdPrimTypeTokens->camera, HdPrimTypeTokens->material,
                                   HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight};
const TfTokenVector _bprimTypes = {HdPrimTypeTokens->renderBuffer};

} // namespace

HdVkRenderDelegate::HdVkRenderDelegate(HdRenderSettingsMap const &settings)
    : HdRenderDelegate(settings)
    , _resourceRegistry(std::make_shared<HdResourceRegistry>())
{
    try {
        _device = std::make_unique<vkr::Device>();
        _renderer = std::make_unique<vkr::Renderer>(*_device);
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk: Vulkan init failed, nothing will render: %s", e.what());
        _renderer.reset();
        _device.reset();
    }
    _scene = std::make_unique<vkr::Scene>(_device.get());
    _textures = std::make_unique<HdVkTextureCache>();
    // Apps (usdview's settings menu) read these back before ever setting them.
    _PopulateDefaultSettings(GetRenderSettingDescriptors());
}

HdVkRenderDelegate::~HdVkRenderDelegate() = default;

const TfTokenVector &HdVkRenderDelegate::GetSupportedRprimTypes() const { return _rprimTypes; }
const TfTokenVector &HdVkRenderDelegate::GetSupportedSprimTypes() const { return _sprimTypes; }
const TfTokenVector &HdVkRenderDelegate::GetSupportedBprimTypes() const { return _bprimTypes; }

HdResourceRegistrySharedPtr HdVkRenderDelegate::GetResourceRegistry() const
{
    return _resourceRegistry;
}

HdAovDescriptor HdVkRenderDelegate::GetDefaultAovDescriptor(TfToken const &name) const
{
    if (name == HdAovTokens->color) {
        // Linear HDR; the app does display transforms (usdview: sRGB).
        return HdAovDescriptor(HdFormatFloat16Vec4, false, VtValue(GfVec4f(0.0f)));
    }
    if (name == HdAovTokens->depth) {
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
    }
    // Picking and selection highlighting. ponytail: no elementId (face
    // picking) yet; the task controller skips AOVs that come back invalid.
    if (name == HdAovTokens->primId || name == HdAovTokens->instanceId) {
        return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    }
    return HdAovDescriptor();
}

// Shown in usdview's Hydra Settings menu; defaults match vkr::PostProcess::Settings.
HdRenderSettingDescriptorList HdVkRenderDelegate::GetRenderSettingDescriptors() const
{
    const vkr::PostProcess::Settings defaults;
    return {
        {"Post Processing (exposure, bloom, tonemap)", HdVkRenderSettingsTokens->postProcess, VtValue(true)},
        {"Auto Exposure", HdVkRenderSettingsTokens->autoExposure, VtValue(defaults.autoExposure)},
        {"Exposure Compensation (EV)", HdVkRenderSettingsTokens->exposureCompensation,
         VtValue(defaults.exposureCompensation)},
        {"Bloom Intensity", HdVkRenderSettingsTokens->bloomIntensity, VtValue(defaults.bloomIntensity)},
    };
}

HdRenderPassSharedPtr HdVkRenderDelegate::CreateRenderPass(
    HdRenderIndex *index, HdRprimCollection const &collection)
{
    return std::make_shared<HdVkRenderPass>(index, collection, *_scene, _renderer.get());
}

HdInstancer *HdVkRenderDelegate::CreateInstancer(HdSceneDelegate *delegate, SdfPath const &id)
{
    return new HdVkInstancer(delegate, id);
}

void HdVkRenderDelegate::DestroyInstancer(HdInstancer *instancer) { delete instancer; }

HdRprim *HdVkRenderDelegate::CreateRprim(TfToken const &typeId, SdfPath const &rprimId)
{
    if (typeId == HdPrimTypeTokens->mesh) {
        return new HdVkMesh(rprimId, _scene.get());
    }
    return nullptr;
}

void HdVkRenderDelegate::DestroyRprim(HdRprim *rprim) { delete rprim; }

HdSprim *HdVkRenderDelegate::CreateSprim(TfToken const &typeId, SdfPath const &sprimId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdCamera(sprimId);
    }
    if (typeId == HdPrimTypeTokens->material) {
        return new HdVkMaterial(sprimId, _textures.get());
    }
    if (typeId == HdPrimTypeTokens->distantLight || typeId == HdPrimTypeTokens->domeLight) {
        return new HdVkLight(sprimId, typeId, _scene.get(), _textures.get());
    }
    return nullptr;
}

HdSprim *HdVkRenderDelegate::CreateFallbackSprim(TfToken const &typeId)
{
    return CreateSprim(typeId, SdfPath::EmptyPath());
}

void HdVkRenderDelegate::DestroySprim(HdSprim *sprim) { delete sprim; }

HdBprim *HdVkRenderDelegate::CreateBprim(TfToken const &typeId, SdfPath const &bprimId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        return new HdVkRenderBuffer(bprimId);
    }
    return nullptr;
}

HdBprim *HdVkRenderDelegate::CreateFallbackBprim(TfToken const &typeId)
{
    return CreateBprim(typeId, SdfPath::EmptyPath());
}

void HdVkRenderDelegate::DestroyBprim(HdBprim *bprim) { delete bprim; }

void HdVkRenderDelegate::CommitResources(HdChangeTracker *)
{
    try {
        _scene->Commit();
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk upload failed: %s", e.what());
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
