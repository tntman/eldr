#include "renderDelegate.h"

#include "camera.h"
#include "curves.h"
#include "glInterop.h"
#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh.h"
#include "renderBuffer.h"
#include "renderPass.h"
#include "renderer.h"
#ifdef HDVK_VOLUMES
#include "field.h"
#include "volume.h"
#endif
#include "subdivision.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/stringUtils.h"
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

const std::vector<HdVkExtraAov> &HdVkExtraAovs()
{
    using A = vkr::Aovs;
    const auto &k = *HdVkAovTokens;
    static const std::vector<HdVkExtraAov> aovs = {
        {HdAovTokens->normal, A::kNormal, HdFormatFloat32Vec3},
        {HdAovTokens->Neye, A::kNormalEye, HdFormatFloat32Vec3},
        {HdAovTokens->Peye, A::kPositionEye, HdFormatFloat32Vec3},
        {HdAovTokens->cameraDepth, A::kCameraDepth, HdFormatFloat32},
        {HdAovTokensMakePrimvar(TfToken("st")), A::kSt, HdFormatFloat32Vec2},
        // Karma's N is the smooth normal; Eldr's always faces the camera,
        // which is Karma's N_facing.
        {k.hitN, A::kNormal, HdFormatFloat32Vec3},
        {k.hitNFacing, A::kNormal, HdFormatFloat32Vec3},
        {k.hitNCamera, A::kNormalEye, HdFormatFloat32Vec3},
        {k.hitP, A::kPositionWorld, HdFormatFloat32Vec3},
        {k.hitPCamera, A::kPositionEye, HdFormatFloat32Vec3},
        {k.hitPz, A::kDepth, HdFormatFloat32},
        {k.baseColor, A::kAlbedo, HdFormatFloat32Vec3},
        {k.diffuseAlbedo, A::kDiffuseAlbedo, HdFormatFloat32Vec3},
        {k.directEmission, A::kEmission, HdFormatFloat32Vec3},
        {k.cryptoObject, A::kCryptoObject, HdFormatFloat32Vec3},
        {k.cryptoMaterial, A::kCryptoMaterial, HdFormatFloat32Vec3},
    };
    return aovs;
}

namespace {

#ifdef HDVK_VOLUMES
// UsdVol's OpenVDB fields (with volumes: built against a USD with OpenVDB).
const TfToken _openvdbAsset("openvdbAsset");
#endif

const TfTokenVector _rprimTypes = {HdPrimTypeTokens->mesh, HdPrimTypeTokens->basisCurves,
                                   HdPrimTypeTokens->points,
#ifdef HDVK_VOLUMES
                                   HdPrimTypeTokens->volume
#endif
};
const TfTokenVector _sprimTypes = {HdPrimTypeTokens->camera,      HdPrimTypeTokens->material,
                                   HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight,
                                   HdPrimTypeTokens->sphereLight,  HdPrimTypeTokens->rectLight,
                                   HdPrimTypeTokens->diskLight,    HdPrimTypeTokens->cylinderLight};
const TfTokenVector _bprimTypes = {HdPrimTypeTokens->renderBuffer,
#ifdef HDVK_VOLUMES
                                   _openvdbAsset
#endif
};

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
    // Extra AOVs (vkr::Aovs), rendered only when bound; cleared to 0.
    for (const HdVkExtraAov &aov : HdVkExtraAovs()) {
        if (name == aov.name) {
            const VtValue zero = aov.format == HdFormatFloat32       ? VtValue(0.0f)
                               : aov.format == HdFormatFloat32Vec2 ? VtValue(GfVec2f(0.0f))
                                                                   : VtValue(GfVec3f(0.0f));
            return HdAovDescriptor(aov.format, false, zero);
        }
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
    // image (volumes last).
    HdRenderSettingDescriptorList settings = {
        {"Depth Prepass (off: no AO, SSR, refraction, subsurface)", t.depthPrepass, VtValue(frame.depthPrepass)},
        {"Frustum Culling (GPU)", t.frustumCulling, VtValue(frame.frustumCulling)},
        {"Small Object Culling (pixels, 0 = off)", t.cullPixels, VtValue(frame.cullPixels)},
        {"Subdivision Surfaces (Complexity menu)", t.subdivision, VtValue(kHdVkSubdivision)},
        {"Subdivision Max Level (0-8)", t.maxRefineLevel, VtValue(kHdVkMaxRefineLevel)},
        {"Levels of Detail (heavy meshes)", t.lod, VtValue(frame.lod)},
        {"Level of Detail Bias (+1: coarser)", t.lodBias, VtValue(frame.lodBias)},
        {"Dome Light Lighting (IBL)", t.imageBasedLighting, VtValue(frame.imageBasedLighting)},
        {"Global Illumination (RTX probes)", t.gi, VtValue(frame.gi)},
        {"GI Probes along the Scene (2-64)", t.giProbes, VtValue(int(frame.giProbes))},
        {"GI Rays per Probe per Frame (16-512)", t.giRays, VtValue(int(frame.giRays))},
        {"GI Hysteresis (0-0.99)", t.giHysteresis, VtValue(frame.giHysteresis)},
        {"Screen-Space GI (replaces AO)", t.ssgi, VtValue(frame.ssgi)},
        {"SSGI Steps (1-32)", t.ssgiSteps, VtValue(int(frame.ssgiSteps))},
        {"SSGI Radius", t.ssgiRadius, VtValue(frame.ssgiRadius)},
        {"SSGI Resolution Divisor (1-2)", t.ssgiResolution, VtValue(int(frame.ssgiResolution))},
        {"Emissive Surfaces as Lights (small, bright meshes)", t.emissiveLights, VtValue(frame.emissiveLights)},
        {"Emissive Light Threshold (radiance)", t.emissiveLightThreshold, VtValue(frame.emissiveLightThreshold)},
        {"Local Lights (sphere, rect, disk, cylinder)", t.localLights, VtValue(frame.localLights)},
        {"Clustered Local Lights (off: all lights per pixel)", t.clusteredLights, VtValue(frame.clusteredLights)},
        {"Ambient Occlusion (GTAO)", t.ambientOcclusion, VtValue(frame.ambientOcclusion)},
        {"Ambient Occlusion Radius", t.aoRadius, VtValue(frame.aoRadius)},
        {"Ambient Occlusion Directions (1-8)", t.aoSlices, VtValue(int(frame.aoSlices))},
        {"Screen Space Reflections", t.ssr, VtValue(frame.ssr)},
        {"Reflection Ray Steps (1-256)", t.ssrSteps, VtValue(int(frame.ssrSteps))},
        {"Ray-Traced Reflections (RTX; replace SSR)", t.rtReflections, VtValue(frame.rtReflections)},
        {"Ray-Traced Reflection Rays per Pixel (1-16)", t.rtReflectionSamples,
         VtValue(int(frame.rtReflectionSamples))},
        {"Reflection Probe (captured cube)", t.reflectionProbe, VtValue(frame.reflectionProbe)},
        {"Reflection Probe Resolution (16-512)", t.probeResolution, VtValue(int(frame.probeResolution))},
        {"Reflection Probe Position (x y z, empty = scene centre)", t.probePosition, VtValue(std::string())},
        {"Planar Reflections (flat glossy floors)", t.planarReflections, VtValue(frame.planarReflections)},
        {"Planar Reflection Scale (0.1-1)", t.planarScale, VtValue(frame.planarScale)},
        {"Sun Shadows (cascades)", t.shadows, VtValue(frame.shadows)},
        {"Sun Shadow Resolution (256-8192)", t.shadowResolution, VtValue(int(frame.shadowResolution))},
        {"Sun Shadow Cascades (1-4)", t.shadowCascades, VtValue(int(frame.shadowCascades))},
        {"Local Light Shadows", t.localShadows, VtValue(frame.localShadows)},
        {"Local Light Shadow Resolution (64-2048)", t.localShadowResolution,
         VtValue(int(frame.localShadowResolution))},
        {"Soft Shadows (PCSS)", t.softShadows, VtValue(frame.softShadows)},
        {"Soft Sun Shadows (distant light angle)", t.softSunShadows, VtValue(frame.softSunShadows)},
        {"Volumetric Fog", t.fog, VtValue(frame.fog)},
        {"Fog Density (extinction per unit)", t.fogDensity, VtValue(frame.fogDensity)},
        {"Fog Height (base)", t.fogHeight, VtValue(frame.fogHeight)},
        {"Fog Height Falloff (per unit, 0 = even)", t.fogHeightFalloff, VtValue(frame.fogHeightFalloff)},
        {"Fog Anisotropy (phase g, -1..1)", t.fogAnisotropy, VtValue(frame.fogAnisotropy)},
        {"Fog Color (r g b)", t.fogColor,
         VtValue(TfStringPrintf("%g %g %g", frame.fogColor[0], frame.fogColor[1], frame.fogColor[2]))},
        {"Fog Max Distance (0 = scene)", t.fogMaxDistance, VtValue(frame.fogMaxDistance)},
        {"Fog Grid Size (froxels across, 16-512)", t.fogGridSize, VtValue(int(frame.fogGridSize))},
        {"Fog Height Along Z (Z-up stages)", t.fogZUp, VtValue(frame.fogZUp)},
        {"Soft Shadow Samples (1-16)", t.softShadowSamples, VtValue(int(frame.softShadowSamples))},
        {"Translucent Shadows (coloured, partial)", t.translucentShadows, VtValue(frame.translucentShadows)},
        {"Translucent Shadow Resolution (1 = full, 2 = half, 4 = quarter)", t.translucentShadowDivisor,
         VtValue(int(frame.translucentShadowDivisor))},
        {"Ray-Traced Shadows (RTX; replace shadow maps)", t.rtShadows, VtValue(frame.rtShadows)},
        {"Ray-Traced Shadow Rays per Light (1-16)", t.rtShadowSamples, VtValue(int(frame.rtShadowSamples))},
        {"Ray-Traced Skinned Instances (largest on screen, 0-256)", t.rtSkinnedInstances,
         VtValue(int(frame.rtSkinnedInstances))},
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
        {"Subsurface Scattering", t.subsurface, VtValue(frame.subsurface)},
        {"Subsurface Samples (1-64)", t.subsurfaceSamples, VtValue(int(frame.subsurfaceSamples))},
        {"Displacement", t.displacement, VtValue(frame.displacement)},
        {"Displacement Scale", t.displacementScale, VtValue(frame.displacementScale)},
        {"Translucent Objects", t.translucency, VtValue(frame.translucency)},
        {"Holdouts (matte objects: black, zero alpha)", t.holdouts, VtValue(frame.holdouts)},
        {"Order-Independent Transparency (off: sorted per mesh)", t.oit, VtValue(frame.oit)},
        {"OIT Depth Weight (exponent, 0 = plain average)", t.oitDepthWeight, VtValue(frame.oitDepthWeight)},
        {"Refraction (transmission, ior)", t.refraction, VtValue(frame.refraction)},
        {"Refraction Blur Samples (1-16)", t.refractionSamples, VtValue(int(frame.refractionSamples))},
        {"Object Motion Vectors", t.objectMotion, VtValue(frame.objectMotion)},
        {"Blend Shape Motion Vectors", t.blendShapeMotion, VtValue(frame.blendShapeMotion)},
        {"Temporal Anti-Aliasing", t.taa, VtValue(frame.taa)},
        {"TAA Samples (still camera)", t.taaSamples, VtValue(int(frame.taaSamples))},
        {"Film Accumulation (still camera: plain average, no TAA clipping)", t.filmAccumulation,
         VtValue(frame.filmAccumulation)},
        {"Film Samples (still camera, final frames)", t.filmSamples, VtValue(int(frame.filmSamples))},
        {"Film Pixel Filter Radius (Gaussian, pixels, 0-2)", t.filmFilterRadius, VtValue(frame.filmFilterRadius)},
        {"Final Quality (offline: also when enableInteractive is false)", t.finalQuality,
         VtValue(frame.finalQuality)},
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
#ifdef HDVK_VOLUMES
    const vkr::VolumePass::Settings &volume = frame.volume;
    settings.insert(settings.end(), {
        {"Volumes (OpenVDB)", t.volumes, VtValue(frame.volumes)},
        {"Volume Step Size (voxels)", t.volumeStepSize, VtValue(volume.stepSize)},
        {"Volume Max Steps", t.volumeMaxSteps, VtValue(int(volume.maxSteps))},
        {"Volume Shadow Steps", t.volumeShadowSteps, VtValue(int(volume.shadowSteps))},
        {"Volume Density Multiplier (on the material's)", t.volumeDensity, VtValue(volume.density)},
        {"Volume Emission Multiplier (on the material's)", t.volumeEmission, VtValue(volume.emission)},
        {"Volume Phase Anisotropy (without a material, -1..1)", t.volumeAnisotropy, VtValue(volume.anisotropy)},
        {"Volume Resolution (1 = full, 2 = half)", t.volumeResolution, VtValue(int(volume.resolution))},
    });
#endif
    return settings;
}

void HdVkRenderDelegate::SetRenderSetting(TfToken const &key, VtValue const &value)
{
    const auto &t = *HdVkRenderSettingsTokens;
    const bool geometry = (key == t.subdivision || key == t.maxRefineLevel) && GetRenderSetting(key) != value;
    HdRenderDelegate::SetRenderSetting(key, value);
    // Through the scene: a scene index render index's change tracker can't
    // dirty prims, and nothing may during a sync (this runs between frames).
    if (geometry && _terminalSceneIndex) {
        HdVkDirtyMeshes(HdSceneIndexBaseRefPtr(_terminalSceneIndex));
    }
}

void HdVkRenderDelegate::SetTerminalSceneIndex(HdSceneIndexBaseRefPtr const &terminalSceneIndex)
{
    _terminalSceneIndex = terminalSceneIndex;
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
#ifdef HDVK_VOLUMES
    if (typeId == HdPrimTypeTokens->volume) {
        return new HdVkVolume(rprimId, _scene.get());
    }
#endif
    return nullptr;
}

void HdVkRenderDelegate::DestroyRprim(HdRprim *rprim) { delete rprim; }

HdSprim *HdVkRenderDelegate::CreateSprim(TfToken const &typeId, SdfPath const &sprimId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        return new HdVkCamera(sprimId);
    }
    if (typeId == HdPrimTypeTokens->material) {
        return new HdVkMaterial(sprimId, _textures.get());
    }
    if (typeId == HdPrimTypeTokens->distantLight || typeId == HdPrimTypeTokens->domeLight ||
        typeId == HdPrimTypeTokens->sphereLight || typeId == HdPrimTypeTokens->rectLight ||
        typeId == HdPrimTypeTokens->diskLight || typeId == HdPrimTypeTokens->cylinderLight) {
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
#ifdef HDVK_VOLUMES
    if (typeId == _openvdbAsset) {
        return new HdVkField(bprimId);
    }
#endif
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
    stats["volumeMemory"] = size_t(_scene->VolumeBytes());  // NanoVDB grids
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
    stats["meshes"] = size_t(gpu.meshCount);
    stats["drawCalls"] = size_t(frame.drawCalls);
    stats["volumes"] = size_t(gpu.volumeCount);
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
