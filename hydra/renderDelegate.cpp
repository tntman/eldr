#include "renderDelegate.h"

#include "curves.h"
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

const TfTokenVector _rprimTypes = {HdPrimTypeTokens->mesh, HdPrimTypeTokens->basisCurves,
                                   HdPrimTypeTokens->points};
const TfTokenVector _sprimTypes = {HdPrimTypeTokens->camera,      HdPrimTypeTokens->material,
                                   HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight,
                                   HdPrimTypeTokens->sphereLight,  HdPrimTypeTokens->rectLight,
                                   HdPrimTypeTokens->diskLight};
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

uint32_t HdVkRenderTagBit(TfToken const &tag)
{
    return tag == HdRenderTagTokens->geometry ? 1u : tag == HdRenderTagTokens->render ? 2u :
           tag == HdRenderTagTokens->proxy    ? 4u : tag == HdRenderTagTokens->guide  ? 8u :
           tag == HdRenderTagTokens->hidden   ? 0u : 16u;
}

const TfTokenVector &HdVkRenderDelegate::GetSupportedRprimTypes() const { return _rprimTypes; }
const TfTokenVector &HdVkRenderDelegate::GetSupportedSprimTypes() const { return _sprimTypes; }
const TfTokenVector &HdVkRenderDelegate::GetSupportedBprimTypes() const { return _bprimTypes; }

// MaterialX first (Houdini and Karma assets), then Storm's context: assets
// made for usdview (Disney's Moana) bind their UsdPreviewSurface only to
// outputs:glslfx:surface. UsdShade falls back to the universal
// outputs:surface by itself.
TfTokenVector HdVkRenderDelegate::GetMaterialRenderContexts() const
{
    return {TfToken("mtlx"), TfToken("glslfx")};
}

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

// Shown in usdview's Hydra Settings menu; defaults match vkr::Renderer::Frame.
HdRenderSettingDescriptorList HdVkRenderDelegate::GetRenderSettingDescriptors() const
{
    const vkr::Renderer::Frame frame{};
    const vkr::PostProcess::Settings &defaults = frame.post;
    return {
        {"Post Processing (exposure, bloom, tonemap)", HdVkRenderSettingsTokens->postProcess,
         VtValue(frame.postProcess)},
        {"Temporal Anti-Aliasing", HdVkRenderSettingsTokens->taa, VtValue(frame.taa)},
        {"Ambient Occlusion (GTAO)", HdVkRenderSettingsTokens->ambientOcclusion,
         VtValue(frame.ambientOcclusion)},
        {"Ambient Occlusion Radius", HdVkRenderSettingsTokens->aoRadius, VtValue(frame.aoRadius)},
        {"Screen Space Reflections", HdVkRenderSettingsTokens->ssr, VtValue(frame.ssr)},
        {"Auto Exposure", HdVkRenderSettingsTokens->autoExposure, VtValue(defaults.autoExposure)},
        {"Exposure Compensation (EV)", HdVkRenderSettingsTokens->exposureCompensation,
         VtValue(defaults.exposureCompensation)},
        {"Bloom Intensity", HdVkRenderSettingsTokens->bloomIntensity, VtValue(defaults.bloomIntensity)},
        {"Sun Shadows (cascades)", HdVkRenderSettingsTokens->shadows, VtValue(frame.shadows)},
        {"Local Light Shadows", HdVkRenderSettingsTokens->localShadows, VtValue(frame.localShadows)},
        {"Soft Shadows (PCSS)", HdVkRenderSettingsTokens->softShadows, VtValue(frame.softShadows)},
        {"TAA Samples (still camera)", HdVkRenderSettingsTokens->taaSamples, VtValue(int(frame.taaSamples))},
        {"Depth of Field (camera fStop)", HdVkRenderSettingsTokens->depthOfField, VtValue(frame.depthOfField)},
        {"Motion Blur (camera shutter)", HdVkRenderSettingsTokens->motionBlur, VtValue(frame.motionBlur)},
        {"Small Object Culling (pixels, 0 = off)", HdVkRenderSettingsTokens->cullPixels,
         VtValue(frame.cullPixels)},
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
    if (typeId == HdPrimTypeTokens->basisCurves) {
        return new HdVkBasisCurves(rprimId, _scene.get());
    }
    if (typeId == HdPrimTypeTokens->points) {
        return new HdVkPoints(rprimId, _scene.get());
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
    if (typeId == HdPrimTypeTokens->distantLight || typeId == HdPrimTypeTokens->domeLight ||
        typeId == HdPrimTypeTokens->sphereLight || typeId == HdPrimTypeTokens->rectLight ||
        typeId == HdPrimTypeTokens->diskLight) {
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

VtDictionary HdVkRenderDelegate::GetRenderStats() const
{
    VtDictionary stats;
    if (!_device) {
        return stats;
    }
    // "GPU mem" is graphics memory: render targets, shadow maps, the
    // environment, textures and the scene's pools. hostMemoryUsed is the
    // system memory the GPU reads (per-frame tables, staging).
    const vkr::Device::MemoryStats memory = _device->Memory();
    stats[HdPerfTokens->gpuMemoryUsed.GetString()] = size_t(memory.deviceLocal);
    stats[HdPerfTokens->textureMemory.GetString()] = size_t(memory.textures);
    stats[HdTokens->primvar.GetString()] = size_t(_scene->VertexBytes());
    stats[HdTokens->topology.GetString()] = size_t(_scene->IndexBytes());
    stats["hostMemoryUsed"] = size_t(memory.host);
    return stats;
}

void HdVkRenderDelegate::CommitResources(HdChangeTracker *)
{
    try {
        _scene->Commit();
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk upload failed: %s", e.what());
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
