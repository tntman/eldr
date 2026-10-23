#include "renderDelegate.h"

#include "curves.h"
#include "glInterop.h"
#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh.h"
#include "renderBuffer.h"
#include "renderPass.h"
#include "renderer.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hgi/hgi.h"
#include "pxr/imaging/hgi/tokens.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdVkRenderSettingsTokens, HDVK_RENDER_SETTING_TOKENS);
TF_DEFINE_PUBLIC_TOKENS(HdVkAovTokens, HDVK_AOV_TOKENS);

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
    // Extra AOVs (vkr::Aovs), rendered only when bound.
    if (name == HdAovTokens->normal || name == HdAovTokens->Neye || name == HdAovTokens->Peye ||
        name == HdVkAovTokens->albedo || name == HdVkAovTokens->emission) {
        return HdAovDescriptor(HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
    }
    if (name == HdAovTokens->cameraDepth) {
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(0.0f));
    }
    if (name == HdAovTokensMakePrimvar(TfToken("st"))) {
        return HdAovDescriptor(HdFormatFloat32Vec2, false, VtValue(GfVec2f(0.0f)));
    }
    if (name == HdVkAovTokens->cryptoObject || name == HdVkAovTokens->cryptoMaterial) {
        return HdAovDescriptor(HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
    }
    return HdAovDescriptor();
}

// Shown in usdview's Hydra Settings menu; defaults match vkr::Renderer::Frame.
HdRenderSettingDescriptorList HdVkRenderDelegate::GetRenderSettingDescriptors() const
{
    const vkr::Renderer::Frame frame{};
    const vkr::PostProcess::Settings &defaults = frame.post;
    const auto &t = *HdVkRenderSettingsTokens;
    // In usdview's order: pipeline, lighting, shadows, materials, motion,
    // image.
    return {
        {"Depth Prepass (off: no AO, no SSR)", t.depthPrepass, VtValue(frame.depthPrepass)},
        {"Frustum Culling (GPU)", t.frustumCulling, VtValue(frame.frustumCulling)},
        {"Small Object Culling (pixels, 0 = off)", t.cullPixels, VtValue(frame.cullPixels)},
        {"Dome Light Lighting (IBL)", t.imageBasedLighting, VtValue(frame.imageBasedLighting)},
        {"Global Illumination (RTX probes)", t.gi, VtValue(frame.gi)},
        {"GI Probes along the Scene (2-64)", t.giProbes, VtValue(int(frame.giProbes))},
        {"GI Rays per Probe per Frame (16-512)", t.giRays, VtValue(int(frame.giRays))},
        {"GI Hysteresis (0-0.99)", t.giHysteresis, VtValue(frame.giHysteresis)},
        {"Emissive Surfaces as Lights (small, bright meshes)", t.emissiveLights, VtValue(frame.emissiveLights)},
        {"Emissive Light Threshold (radiance)", t.emissiveLightThreshold, VtValue(frame.emissiveLightThreshold)},
        {"Local Lights (sphere, rect, disk)", t.localLights, VtValue(frame.localLights)},
        {"Clustered Local Lights (off: all lights per pixel)", t.clusteredLights, VtValue(frame.clusteredLights)},
        {"Ambient Occlusion (GTAO)", t.ambientOcclusion, VtValue(frame.ambientOcclusion)},
        {"Ambient Occlusion Radius", t.aoRadius, VtValue(frame.aoRadius)},
        {"Ambient Occlusion Directions (1-8)", t.aoSlices, VtValue(int(frame.aoSlices))},
        {"Screen Space Reflections", t.ssr, VtValue(frame.ssr)},
        {"Reflection Ray Steps (1-256)", t.ssrSteps, VtValue(int(frame.ssrSteps))},
        {"Ray-Traced Reflections (RTX; replace SSR)", t.rtReflections, VtValue(frame.rtReflections)},
        {"Ray-Traced Reflection Rays per Pixel (1-16)", t.rtReflectionSamples,
         VtValue(int(frame.rtReflectionSamples))},
        {"Sun Shadows (cascades)", t.shadows, VtValue(frame.shadows)},
        {"Sun Shadow Resolution (256-8192)", t.shadowResolution, VtValue(int(frame.shadowResolution))},
        {"Sun Shadow Cascades (1-4)", t.shadowCascades, VtValue(int(frame.shadowCascades))},
        {"Local Light Shadows", t.localShadows, VtValue(frame.localShadows)},
        {"Local Light Shadow Resolution (64-2048)", t.localShadowResolution,
         VtValue(int(frame.localShadowResolution))},
        {"Soft Shadows (PCSS)", t.softShadows, VtValue(frame.softShadows)},
        {"Soft Shadow Samples (1-16)", t.softShadowSamples, VtValue(int(frame.softShadowSamples))},
        {"Ray-Traced Shadows (RTX; replace shadow maps)", t.rtShadows, VtValue(frame.rtShadows)},
        {"Ray-Traced Shadow Rays per Light (1-16)", t.rtShadowSamples, VtValue(int(frame.rtShadowSamples))},
        {"Normal Maps", t.normalMaps, VtValue(frame.normalMaps)},
        {"Specular Workflow (UsdPreviewSurface specularColor)", t.specularWorkflow,
         VtValue(frame.specularWorkflow)},
        {"Clearcoat (coat layer)", t.clearcoat, VtValue(frame.clearcoat)},
        {"Sheen (fabric, fuzz)", t.sheen, VtValue(frame.sheen)},
        {"Thin Film Iridescence", t.thinFilm, VtValue(frame.thinFilm)},
        {"Anisotropic Specular (brushed metal)", t.anisotropicSpecular, VtValue(frame.anisotropicSpecular)},
        {"Hair Shading (curves)", t.hairShading, VtValue(frame.hairShading)},
        {"Toon Shading (materials' eldr:toonBands)", t.toon, VtValue(frame.toon)},
        {"Toon Bands for Every Material (0 = off)", t.toonBands, VtValue(int(frame.toonBands))},
        {"MaterialX Graphs (material programs)", t.materialGraphs, VtValue(frame.materialGraphs)},
        {"Procedural Textures (MaterialX noise)", t.proceduralTextures, VtValue(frame.proceduralTextures)},
        {"Texture Anisotropic Filtering (1-16)", t.anisotropy, VtValue(int(frame.anisotropy))},
        {"Translucent Objects", t.translucency, VtValue(frame.translucency)},
        {"Object Motion Vectors", t.objectMotion, VtValue(frame.objectMotion)},
        {"Blend Shape Motion Vectors", t.blendShapeMotion, VtValue(frame.blendShapeMotion)},
        {"Temporal Anti-Aliasing", t.taa, VtValue(frame.taa)},
        {"TAA Samples (still camera)", t.taaSamples, VtValue(int(frame.taaSamples))},
        {"Depth of Field (camera fStop)", t.depthOfField, VtValue(frame.depthOfField)},
        {"Motion Blur (camera shutter)", t.motionBlur, VtValue(frame.motionBlur)},
        {"Lens Distortion (camera lensDistortion:*)", t.lensDistortion, VtValue(frame.lensDistortion)},
        {"Extra AOVs (normal, st, albedo, Cryptomatte...)", t.extraAovs, VtValue(frame.extraAovs)},
        {"Share Frames with OpenGL (off: read back)", t.shareFrame, VtValue(frame.shareFrame)},
        {"Post Processing (exposure, bloom, tonemap)", t.postProcess, VtValue(frame.postProcess)},
        {"Auto Exposure", t.autoExposure, VtValue(defaults.autoExposure)},
        {"Exposure Compensation (EV)", t.exposureCompensation, VtValue(defaults.exposureCompensation)},
        {"Bloom Intensity", t.bloomIntensity, VtValue(defaults.bloomIntensity)},
        {"Tonemapper (aces, agx, agxPunchy, none)", t.tonemapper, VtValue(std::string("aces"))},
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
    stats["rendererName"] = std::string("Eldr");  // plugInfo.json's displayName
    if (!_device || !_renderer) {
        return stats;
    }
    // Memory in bytes. gpuMemoryUsed (usdview's "GPU mem") is the graphics
    // memory HdVk holds: render targets, shadow maps, the environment,
    // textures and the scene's pools; hostMemoryUsed the system memory the
    // GPU reads (per-frame tables, staging). vram*: the driver's view of the
    // card, all of this process's use included (0 without VK_EXT_memory_budget).
    const vkr::Device::MemoryStats memory = _device->Memory();
    stats[HdPerfTokens->gpuMemoryUsed.GetString()] = size_t(memory.deviceLocal);
    stats[HdPerfTokens->textureMemory.GetString()] = size_t(memory.textures);
    stats[HdTokens->primvar.GetString()] = size_t(_scene->VertexBytes());
    stats[HdTokens->topology.GetString()] = size_t(_scene->IndexBytes());
    stats["hostMemoryUsed"] = size_t(memory.host);
    stats["accelerationStructureMemory"] = size_t(_renderer->GetStats().accelerationStructureBytes);
    stats["rayTracingSupported"] = _device->rayTracing;  // else the hdvk:rt* settings are ignored
    stats["totalMemory"] = size_t(memory.deviceLocal + memory.host);
    stats["peakMemory"] = size_t(memory.peakDeviceLocal);
    stats["vramSize"] = size_t(memory.heapSize);
    stats["vramUsage"] = size_t(memory.heapUsage);
    stats["vramBudget"] = size_t(memory.heapBudget);

    // The last frame, times in milliseconds.
    const vkr::Renderer::Stats &frame = _renderer->GetStats();
    const vkr::Scene::Gpu &gpu = _scene->GetGpu();
    stats["gpuTimeMs"] = frame.gpuMs;
    stats["renderTimeMs"] = frame.cpuMs;
    stats["commitTimeMs"] = _commitMs;
    VtDictionary passes;
    for (const auto &[name, ms] : frame.passMs) {
        passes[name] = ms;
    }
    stats["gpuPassMs"] = passes;
    stats["trianglesDrawn"] = size_t(frame.triangles);
    stats["trianglesTotal"] = size_t(gpu.triangleCount);
    stats["instancesDrawn"] = size_t(frame.instances);
    stats["instancesTotal"] = size_t(gpu.instanceCount);
    stats["meshes"] = size_t(gpu.drawCount);
    stats["drawCalls"] = size_t(frame.drawCalls);
    stats["exposureEV"] = frame.exposure > 0 ? std::log2(frame.exposure) : 0.0;

    // Convergence: TAA's average of still frames (usdview's "Samples done").
    const bool converged = _renderer->IsConverged();
    stats["numCompletedSamples"] = size_t(frame.taaSamples);
    stats["taaTarget"] = size_t(frame.taaTarget);
    stats["percentDone"] = converged ? 100.0 : 100.0 * frame.taaSamples / std::max(frame.taaTarget, 1u);
    stats["rendererStage"] = std::string(converged ? "converged" : "accumulating");
    stats["frameSharing"] = std::string(_frameShared ? "OpenGL" : "readback");
    if (!_cryptomatteObject.empty()) {
        VtDictionary manifests;
        manifests["CryptoObject"] = _cryptomatteObject;
        manifests["CryptoMaterial"] = _cryptomatteMaterial;
        stats["cryptomatte"] = manifests;
    }
    return stats;
}

void HdVkRenderDelegate::SetDrivers(HdDriverVector const &drivers)
{
    for (const HdDriver *driver : drivers) {
        if (driver->name == HgiTokens->renderDriver && driver->driver.IsHolding<Hgi *>()) {
            _hgi = driver->driver.UncheckedGet<Hgi *>();
        }
    }
}

HdVkGLInterop *HdVkRenderDelegate::GLInterop()
{
    if (!_interopTried && _device) {
        _interopTried = true;
        try {
            _interop = HdVkGLInterop::Create(_hgi, *_device);
        } catch (const std::exception &e) {
            TF_WARN("HdVk: no GPU sharing with OpenGL: %s", e.what());
        }
    }
    return _interop.get();
}

void HdVkRenderDelegate::DisableGLInterop()
{
    _interop.reset();
}

void HdVkRenderDelegate::SetCryptomatteManifests(std::string object, std::string material)
{
    _cryptomatteObject = std::move(object);
    _cryptomatteMaterial = std::move(material);
}

void HdVkRenderDelegate::CommitResources(HdChangeTracker *)
{
    const auto start = std::chrono::steady_clock::now();
    try {
        _scene->Commit();
        _commitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk upload failed: %s", e.what());
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
