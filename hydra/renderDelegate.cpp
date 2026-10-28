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
#include "renderProducts.h"
#include "renderer.h"
#ifdef HDVK_VOLUMES
#include "field.h"
#include "volume.h"
#endif
#include "subdivision.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hgi/hgi.h"
#include "pxr/imaging/hgi/tokens.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <corecrt_startup.h>  // _crt_atexit
#endif
#include <cmath>
#include <cstdio>
#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(HdVkRenderSettingsTokens, HDVK_RENDER_SETTING_TOKENS);
TF_DEFINE_PUBLIC_TOKENS(HdVkRenderSettingsTokens2, HDVK_RENDER_SETTING_TOKENS2);
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
        // Light splits as Karma's light path expressions name them. Eldr's
        // "direct" is its lights' (distant, sphere, rect, disk); the dome,
        // probes, reflections and GI count as indirect (Karma counts the
        // dome as a light). Emission seen in reflections isn't tracked.
        {k.combinedEmission, A::kEmission, HdFormatFloat32Vec3},
        {k.directDiffuse, A::kDirectDiffuse, HdFormatFloat32Vec3},
        {k.indirectDiffuse, A::kIndirectDiffuse, HdFormatFloat32Vec3},
        {k.combinedDiffuse, A::kCombinedDiffuse, HdFormatFloat32Vec3},
        {k.directGlossy, A::kDirectGlossy, HdFormatFloat32Vec3},
        {k.indirectGlossy, A::kIndirectGlossy, HdFormatFloat32Vec3},
        {k.combinedGlossy, A::kCombinedGlossy, HdFormatFloat32Vec3},
        {k.cryptoObject, A::kCryptoObject, HdFormatFloat32Vec3},
        {k.cryptoMaterial, A::kCryptoMaterial, HdFormatFloat32Vec3},
    };
    return aovs;
}

namespace {

// Background teardowns (~HdVkRenderDelegate) still running. A process exit
// must not cut one off: Windows' ExitProcess kills the thread mid Vulkan
// call and the driver's DLL detach then crashes (ntdll 0xc000070a after a
// usdrecord or script that deletes its engine just before exiting).
std::mutex _teardownMutex;
std::condition_variable _teardownDone;
int _teardowns = 0;
bool _exiting = false;

void _WaitForTeardowns()
{
    std::unique_lock<std::mutex> lock(_teardownMutex);
    _exiting = true;  // later deletes tear down on their own thread
    _teardownDone.wait_for(lock, std::chrono::seconds(30), [] { return _teardowns == 0; });
}

// Into the exit() sequence before other threads are killed: on Windows a
// DLL's own atexit runs at DLL_PROCESS_DETACH, after that, so the process'
// table (_crt_atexit). ponytail: assumes hdVk stays loaded (USD never
// unloads plugins); pin the module if a host ever does.
void _WaitForTeardownsAtExit()
{
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        _crt_atexit(_WaitForTeardowns);
#else
        std::atexit(_WaitForTeardowns);
#endif
    });
}

#ifdef HDVK_VOLUMES
// UsdVol's OpenVDB fields (with volumes: built against a USD with OpenVDB).
const TfToken _openvdbAsset("openvdbAsset");
#endif
#ifdef HDVK_HOUDINI
// Houdini's own fields (HoudiniFieldAsset: SOP volumes, .bgeo files),
// read by houdini/field.cpp.
const TfToken _houdiniFieldAsset("houdiniFieldAsset");
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
const TfTokenVector _bprimTypes = {HdPrimTypeTokens->renderBuffer, HdPrimTypeTokens->renderSettings,
#ifdef HDVK_VOLUMES
                                   _openvdbAsset,
#endif
#ifdef HDVK_HOUDINI
                                   _houdiniFieldAsset,
#endif
};

} // namespace

HdVkRenderDelegate::HdVkRenderDelegate(HdRenderSettingsMap const &settings)
    : HdRenderDelegate(settings)
    , _resourceRegistry(std::make_shared<HdResourceRegistry>())
{
    try {
        const auto start = std::chrono::steady_clock::now();
        _device = std::make_unique<vkr::Device>();
        if (std::getenv("HDVK_TIMING")) {
            std::fprintf(stderr, "HdVk device: %.1f ms\n",
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        _renderer = std::make_unique<vkr::Renderer>(*_device);
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk: Vulkan init failed, nothing will render: %s", e.what());
        _renderer.reset();
        _device.reset();
    }
    _scene = std::make_unique<vkr::Scene>(_device.get());
    _textures = std::make_unique<HdVkTextureCache>();
    _materialFallbacks = std::make_unique<HdVkMaterialFallbacks>();
    // Apps (usdview's settings menu) read these back before ever setting them.
    _PopulateDefaultSettings(GetRenderSettingDescriptors());
}

HdVkRenderDelegate::~HdVkRenderDelegate()
{
    using Clock = std::chrono::steady_clock;
    const bool timing = std::getenv("HDVK_TIMING") != nullptr;
    auto lap = [&, last = Clock::now()](const char *what) mutable {
        const auto now = Clock::now();
        if (timing) {
            std::fprintf(stderr, "HdVk teardown: %s %.3f s\n", what, std::chrono::duration<double>(now - last).count());
        }
        last = now;
    };
    // The GL side goes here, on the caller's thread and context.
    _interop.reset();
    lap("interop");
    _textures.reset();
    lap("textures");
    if (!_renderer) {
        return;
    }
    // The Vulkan side goes in the background: the renderer's destructor waits
    // for the prewarm worker's in-flight pipeline compiles (seconds), and the
    // caller (Houdini's viewport switch, a scene change) has no reason to.
    // Nothing else uses these any more: Hydra has destroyed the prims, and
    // the next delegate makes its own device. The cache file is saved as sets
    // are compiled, so that one still finds it. HDVK_SYNC_TEARDOWN=1 waits
    // (validation runs then see the leak check). A process exit waits for
    // it (_WaitForTeardowns, at most 30 s).
    _renderer->StopPrewarm();
    auto teardown = [renderer = std::move(_renderer), scene = std::move(_scene), device = std::move(_device),
                     timing]() mutable {
        const auto start = std::chrono::steady_clock::now();
        renderer.reset();
        scene.reset();
        device.reset();
        if (timing) {
            std::fprintf(stderr, "HdVk teardown (background): %.3f s\n",
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
    };
    _WaitForTeardownsAtExit();
    bool background = !std::getenv("HDVK_SYNC_TEARDOWN");
    {
        std::lock_guard<std::mutex> lock(_teardownMutex);
        background = background && !_exiting;
        _teardowns += background;
    }
    if (!background) {
        teardown();
    } else {
        std::thread([teardown = std::move(teardown)]() mutable {
            teardown();
            std::lock_guard<std::mutex> lock(_teardownMutex);
            --_teardowns;
            _teardownDone.notify_all();
        }).detach();
    }
    lap("handed off");
}

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
    // kma: Karma's context, for its kma_volume (HdVkReadVolumeShading);
    // only where a material has no mtlx or glslfx output.
    return {TfToken("mtlx"), TfToken("glslfx"), TfToken("kma")};
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
    const auto &t2 = *HdVkRenderSettingsTokens2;
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
        {"Level of Detail Crossfade (dithered)", t2.lodFade, VtValue(frame.lodFade)},
        {"Curves as Camera-Facing Ribbons (else tubes)", t2.curveRibbons, VtValue(kHdVkCurveRibbons)},
        {"Points as Camera-Facing Discs (else spheres)", t2.pointSprites, VtValue(kHdVkPointSprites)},
        {"Prewarm Pipelines (compile toggles in the background)", t.pipelinePrewarm, VtValue(true)},
        {"Pipeline Cache on Disk (save compiled shaders)", t.pipelineCache, VtValue(frame.pipelineCache)},
        {"Dome Light Lighting (IBL)", t.imageBasedLighting, VtValue(frame.imageBasedLighting)},
        {"Global Illumination (RTX probes)", t.gi, VtValue(frame.gi)},
        {"GI Probes along the Scene (2-64)", t.giProbes, VtValue(int(frame.giProbes))},
        {"GI Rays per Probe per Frame (16-512)", t.giRays, VtValue(int(frame.giRays))},
        {"GI Hysteresis (0-0.99)", t.giHysteresis, VtValue(frame.giHysteresis)},
        {"Screen-Space GI (replaces AO)", t.ssgi, VtValue(frame.ssgi)},
        {"SSGI Steps (1-32)", t.ssgiSteps, VtValue(int(frame.ssgiSteps))},
        {"SSGI Radius", t.ssgiRadius, VtValue(frame.ssgiRadius)},
        {"SSGI Resolution Divisor (1-2)", t.ssgiResolution, VtValue(int(frame.ssgiResolution))},
        {"Screen-Space Thickness Estimator (SSGI, SSR)", t2.ssgiThickness, VtValue(frame.ssgiThickness)},
        {"SSGI Visibility Bitmask (light behind thin objects)", t2.ssgiBitmask, VtValue(frame.ssgiBitmask)},
        {"SSGI Tile Order (half resolution: traced pixel per frame)", t2.ssgiTileOrder, VtValue(frame.ssgiTileOrder)},
        {"Screen-Space Back-Face Hits (SSR, SSGI)", t2.screenBackface, VtValue(frame.screenBackface)},
        {"Screen-Space Back-Face Hit Scale (0-1)", t2.screenBackfaceScale, VtValue(frame.screenBackfaceScale)},
        {"SSGI Bitmask Object Thickness (fraction of SSGI radius)", t2.ssgiBitmaskThickness,
         VtValue(frame.ssgiBitmaskThickness)},
        {"Emissive Surfaces as Lights (small, bright meshes)", t.emissiveLights, VtValue(frame.emissiveLights)},
        {"Emissive Light Threshold (radiance)", t.emissiveLightThreshold, VtValue(frame.emissiveLightThreshold)},
        {"Dome Sun Extraction (HDRI sun as a shadowed light)", t2.domeSun, VtValue(frame.domeSun)},
        {"Dome Sun Threshold (radiance)", t2.domeSunThreshold, VtValue(frame.domeSunThreshold)},
        {"Direct Light Clamp (radiance, 0 = off)", t2.directClamp, VtValue(frame.directClamp)},
        {"Indirect Light Clamp (radiance, 0 = off)", t2.indirectClamp, VtValue(frame.indirectClamp)},
        {"Local Lights (sphere, rect, disk, cylinder)", t.localLights, VtValue(frame.localLights)},
        {"Clustered Local Lights (off: all lights per pixel)", t.clusteredLights, VtValue(frame.clusteredLights)},
        {"Light Textures (rect/disk inputs:texture:file)", t2.lightTextures, VtValue(frame.lightTextures)},
        {"Ambient Occlusion (GTAO)", t.ambientOcclusion, VtValue(frame.ambientOcclusion)},
        {"Ambient Occlusion Radius", t.aoRadius, VtValue(frame.aoRadius)},
        {"Ambient Occlusion Directions (1-8)", t.aoSlices, VtValue(int(frame.aoSlices))},
        {"Screen Space Reflections", t.ssr, VtValue(frame.ssr)},
        {"Reflection Ray Steps (1-256)", t.ssrSteps, VtValue(int(frame.ssrSteps))},
        {"Denoised Glossy SSR (GGX rays, ratio estimator)", t2.ssrDenoise, VtValue(frame.ssrDenoise)},
        {"SSR Hit Footprint Filter (mip by the ray's cone)", t2.ssrHitFilter, VtValue(frame.ssrHitFilter)},
        {"Ray-Traced Reflections (RTX; replace SSR)", t.rtReflections, VtValue(frame.rtReflections)},
        {"Ray Tracing Sees Displacement", t2.rtDisplacement, VtValue(frame.rtDisplacement)},
        {"Ray-Traced Reflection Rays per Pixel (1-16)", t.rtReflectionSamples,
         VtValue(int(frame.rtReflectionSamples))},
        {"Reflection Probe (captured cube)", t.reflectionProbe, VtValue(frame.reflectionProbe)},
        {"Reflection Probe Resolution (16-512)", t.probeResolution, VtValue(int(frame.probeResolution))},
        {"Reflection Probe Position (x y z, empty = scene centre)", t.probePosition, VtValue(std::string())},
        {"Authored Reflection Probes (primvars:eldr:reflectionProbe)", t2.authoredProbes, VtValue(frame.authoredProbes)},
        {"Normalize Reflection Probes (by GI/SSGI light)", t2.probeNormalize, VtValue(frame.probeNormalize)},
        {"Planar Reflections (flat glossy floors)", t.planarReflections, VtValue(frame.planarReflections)},
        {"Planar Reflection Scale (0.1-1)", t.planarScale, VtValue(frame.planarScale)},
        {"Planar Reflection Mirrors (most)", t2.planarCount, VtValue(int(frame.planarCount))},
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
        {"Fog Ambient from GI Probes (occluded dome light)", t2.fogGiAmbient, VtValue(frame.fogGiAmbient)},
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
        {"Textured Lobe Inputs (coat, sheen, film, IORs)", t.texturedLobes, VtValue(frame.texturedLobes)},
        {"Textured Transmission and Subsurface Inputs", t.texturedTransmission, VtValue(frame.texturedTransmission)},
        {"Toon Shading (materials' eldr:toonBands)", t.toon, VtValue(frame.toon)},
        {"Toon Bands for Every Material (0 = off)", t.toonBands, VtValue(int(frame.toonBands))},
        {"MaterialX Graphs (material programs)", t.materialGraphs, VtValue(frame.materialGraphs)},
        {"Procedural Textures (MaterialX noise)", t.proceduralTextures, VtValue(frame.proceduralTextures)},
        {"Texture Anisotropic Filtering (1-16)", t.anisotropy, VtValue(int(frame.anisotropy))},
        {"Subsurface Scattering", t.subsurface, VtValue(frame.subsurface)},
        {"Subsurface Samples (1-64)", t.subsurfaceSamples, VtValue(int(frame.subsurfaceSamples))},
        {"Displacement", t.displacement, VtValue(frame.displacement)},
        {"Displacement Scale", t.displacementScale, VtValue(frame.displacementScale)},
        {"Displacement Graphs (per vertex)", t2.displacementGraphs, VtValue(frame.displacementGraphs)},
        {"Translucent Objects", t.translucency, VtValue(frame.translucency)},
        {"Holdouts (matte objects: black, zero alpha)", t.holdouts, VtValue(frame.holdouts)},
        {"Translucent Holdouts Cut Alpha (by opacity)", t.holdoutAlpha, VtValue(frame.holdoutAlpha)},
        {"Order-Independent Transparency (off: sorted per mesh)", t.oit, VtValue(frame.oit)},
        {"OIT Depth Weight (exponent, 0 = plain average)", t.oitDepthWeight, VtValue(frame.oitDepthWeight)},
        {"Refraction (transmission, ior)", t.refraction, VtValue(frame.refraction)},
        {"Refraction Blur Samples (screen-space refraction only, 1-16)", t.refractionSamples, VtValue(int(frame.refractionSamples))},
        {"Refraction Back Faces (water from below, TIR)", t.refractionBackFaces, VtValue(frame.refractionBackFaces)},
        {"Refraction Through Layers (glass behind glass)", t.multiRefraction, VtValue(frame.multiRefraction)},
        {"Refraction Layers (copies, 1-16)", t.refractionLayers, VtValue(int(frame.refractionLayers))},
        {"Ray-Traced Refraction (0 off, 1 final quality, 2 always)", t2.rtRefraction, VtValue(int(frame.rtRefraction))},
        {"Ray-Traced Refraction Interfaces (1-16)", t2.rtRefractionBounces, VtValue(int(frame.rtRefractionBounces))},
        {"Ray-Traced Refraction Sees Displacement", t2.rtRefractionDisplacement, VtValue(frame.rtRefractionDisplacement)},
        {"Object Motion Vectors", t.objectMotion, VtValue(frame.objectMotion)},
        {"Blend Shape Motion Vectors", t.blendShapeMotion, VtValue(frame.blendShapeMotion)},
        {"Temporal Anti-Aliasing", t.taa, VtValue(frame.taa)},
        {"TAA Samples (still camera)", t.taaSamples, VtValue(int(frame.taaSamples))},
        {"Render Scale (interactive: TAA upscales from it; < 1 = on, 0.25-1)", t2.renderScale,
         VtValue(frame.renderScale)},
        {"Film Accumulation (still camera: plain average, no TAA clipping)", t.filmAccumulation,
         VtValue(frame.filmAccumulation)},
        {"Film Samples (still camera, final frames)", t.filmSamples, VtValue(int(frame.filmSamples))},
        {"Film Pixel Filter Radius (Gaussian, pixels, 0-2)", t.filmFilterRadius, VtValue(frame.filmFilterRadius)},
        {"Final Quality (offline: also when enableInteractive is false)", t.finalQuality,
         VtValue(frame.finalQuality)},
        {"Sample Dimensions (blue noise per effect; off: IGN)", t.sampleDimensions, VtValue(frame.sampleDimensions)},
        {"Interactive Noise Loop (samples while moving, 1-256)", t.interactiveSamples,
         VtValue(int(frame.interactiveSamples))},
        {"Adaptive Sampling Noise Threshold (0 = off)", t.adaptiveThreshold, VtValue(frame.adaptiveThreshold)},
        {"Adaptive Sampling Threshold, Final Quality (0 = off)", t2.adaptiveFinalThreshold,
         VtValue(frame.adaptiveFinalThreshold)},
        {"Adaptive Sampling Converged Pixel Fraction", t2.adaptivePixelFraction, VtValue(frame.adaptivePixelFraction)},
        {"Adaptive Sampling Min Samples (max: Film Samples)", t.adaptiveMinSamples,
         VtValue(int(frame.adaptiveMinSamples))},
        {"GI Preconverge (film waits for fresh probes)", t.giPreconverge, VtValue(frame.giPreconverge)},
        {"Fog Film Jitter (still: per-sample froxels, no history)", t.fogFilmJitter, VtValue(frame.fogFilmJitter)},
        {"Jittered Light Shape per Sample (final quality: true soft shadows)", t2.shadowJitter, VtValue(frame.shadowJitter)},
        {"Final Quality Shadows (maps, raytraced, auto)", t2.finalShadows, VtValue(std::string("maps"))},
        {"Ray-Traced Shadows through Glass and Volumes", t2.rtTranslucentShadows, VtValue(frame.rtTranslucentShadows)},
        {"Lens Jitter per Sample (film: depth of field without post)", t2.dofJitter, VtValue(frame.dofJitter)},
        {"Residual DOF between Lens Samples (1/sqrt(samples))", t2.dofResidualBlur, VtValue(frame.dofResidualBlur)},
        {"Depth of Field (camera fStop)", t.depthOfField, VtValue(frame.depthOfField)},
        {"Motion Blur (camera shutter)", t.motionBlur, VtValue(frame.motionBlur)},
        {"Motion Blur Time Samples (film: scene at shutter times)", t2.motionBlurTimeSamples,
         VtValue(frame.motionBlurTimeSamples)},
        {"Residual Motion Blur between Time Samples (shutter/samples)", t2.motionBlurResidual,
         VtValue(frame.motionBlurResidual)},
        {"Sampled Motion (shutter samples: first frames blur too)", t2.sampledMotion, VtValue(true)},
        {"Deformation Blur (point caches through the shutter)", t2.deformationBlur, VtValue(frame.deformationBlur)},
        {"Lens Distortion (camera lensDistortion:*)", t.lensDistortion, VtValue(frame.lensDistortion)},
        {"Extra AOVs (normal, st, albedo, Cryptomatte...)", t.extraAovs, VtValue(frame.extraAovs)},
        {"Light Splits: Dome as Direct (Karma)", t2.splitDomeDirect, VtValue(frame.splitDomeDirect)},
        {"Cryptomatte: Soft Edges and Ranks (film samples)", t2.cryptomatteSoft, VtValue(frame.cryptomatteSoft)},
        {"Direct Light Intensity (final pixel only)", t2.directIntensity, VtValue(frame.directIntensity)},
        {"Indirect Light Intensity (final pixel only)", t2.indirectIntensity, VtValue(frame.indirectIntensity)},
        {"Local Volume Shadows (volumes shadow surfaces from local lights)", t2.localVolumeShadows,
         VtValue(frame.localVolumeShadows)},
        {"Volume Mutual Shadows (volumes shadow each other)", t2.volumeMutualShadows,
         VtValue(frame.volume.mutualShadows)},
        {"Volume Default Fields (Cd, temperature without a volume material)", t2.volumeDefaultFields,
         VtValue(frame.volume.defaultFields)},
        {"Volume Multiple Scattering (octaves; off: single, as Karma's volume limit 0)",
         t2.volumeMultipleScattering, VtValue(frame.volume.multipleScattering)},
        {"Light Shapes Visible (to the camera; off by default)", t2.lightShapesVisible,
         VtValue(frame.lightShapesVisible)},
        {"Dome in the Camera (background)", t2.domeCameraVisible, VtValue(frame.showEnvironment)},
        {"Multiple Scattering (where the surface's spec has it: standard_surface, OpenPBR)", t2.multiScatter,
         VtValue(frame.multiScatter)},
        {"Share Frames with OpenGL (off: read back)", t.shareFrame, VtValue(frame.shareFrame)},
        {"Overlap Readback while Moving (one frame behind)", t2.overlapReadback, VtValue(frame.overlapReadback)},
        {"Read Back in Place (no copy)", t2.inPlaceReadback, VtValue(frame.inPlaceReadback)},
        {"Keep Ids while Moving (no readback)", t2.staleMovingIds, VtValue(frame.staleMovingIds)},
        {"Compile Pipelines on Demand (fast start)", t2.lazyPipelines, VtValue(frame.lazyPipelines)},
        {"Unread AOVs Once Converged", t2.lazyExtraAovs, VtValue(frame.lazyExtraAovs)},
        {"Write Render Products (UsdRender, EXR)", t.renderProducts, VtValue(frame.renderProducts)},
        {"Render Products at their Resolution", t2.productResolution, VtValue(frame.productResolution)},
        {"Post Processing (exposure, bloom, tonemap)", t.postProcess, VtValue(frame.postProcess)},
        {"Exposure Compensation (EV)", t.exposureCompensation, VtValue(defaults.exposureCompensation)},
        {"Linear Beauty (Houdini viewport, husk products: no tonemapper, bloom or auto exposure)", t2.productLinear,
         VtValue(frame.linearBeauty)},
        {"Auto Exposure (Linear Beauty off)", t.autoExposure, VtValue(defaults.autoExposure)},
        {"Tonemapper (Linear Beauty off) (aces, agx, agxPunchy, none)", t.tonemapper, VtValue(std::string("aces"))},
        {"Bloom Intensity (Linear Beauty off) (0-1)", t.bloomIntensity, VtValue(defaults.bloomIntensity)},
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
        {"Volumes under Transparency (glass dims and refracts them)", t.volumeUnderTransparency,
         VtValue(volume.underTransparency)},
        {"Volume Shadows (volumes shadow surfaces from the sun)", t.volumeShadows, VtValue(volume.shadows)},
    });
#endif
    return settings;
}

void HdVkRenderDelegate::SetRenderSetting(TfToken const &key, VtValue const &value)
{
    const auto &t = *HdVkRenderSettingsTokens;
    const auto &t2 = *HdVkRenderSettingsTokens2;
    const bool changed = GetRenderSetting(key) != value;
    HdRenderDelegate::SetRenderSetting(key, value);
    // What the host sets (Houdini's viewport: houdini:*, renderCameraPath; husk: husk:*).
    if (static const bool debug = TfGetenvBool("HDVK_DEBUG_AOVS", false);
        debug && changed && !TfStringStartsWith(key.GetString(), "eldr:")) {
        std::fprintf(stderr, "Eldr setting %s = %s\n", key.GetText(), TfStringify(value).c_str());
        std::fflush(stderr);
    }
    // Through the scene: a scene index render index's change tracker can't
    // dirty prims, and nothing may during a sync (this runs between frames).
    if (changed && _terminalSceneIndex) {
        if (key == t.subdivision || key == t.maxRefineLevel) {
            HdVkDirtyMeshes(HdSceneIndexBaseRefPtr(_terminalSceneIndex));
        } else if (key == t2.curveRibbons) {
            HdVkDirtyMeshes(HdSceneIndexBaseRefPtr(_terminalSceneIndex), HdPrimTypeTokens->basisCurves);
        } else if (key == t2.pointSprites) {
            HdVkDirtyMeshes(HdSceneIndexBaseRefPtr(_terminalSceneIndex), HdPrimTypeTokens->points);
        }
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
        return new HdVkMaterial(sprimId, _textures.get(), _materialFallbacks.get());
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
    if (typeId == HdPrimTypeTokens->renderSettings) {
        _renderSettings.push_back(new HdVkRenderSettings(bprimId));
        return _renderSettings.back();
    }
#ifdef HDVK_VOLUMES
    // A field made again at a path keeps the grid volumes may still hold.
    auto grid = [&] {
        std::weak_ptr<vkr::VolumeGrid> &known = _fieldGrids[bprimId];
        std::shared_ptr<vkr::VolumeGrid> g = bprimId.IsEmpty() ? nullptr : known.lock();
        if (!g) {
            known = g = std::make_shared<vkr::VolumeGrid>();
        }
        return g;
    };
    if (typeId == _openvdbAsset) {
        return new HdVkField(bprimId, grid());
    }
#endif
#ifdef HDVK_HOUDINI
    if (typeId == _houdiniFieldAsset) {
        return new HdVkField(bprimId, grid(), true);
    }
#endif
    return nullptr;
}

HdBprim *HdVkRenderDelegate::CreateFallbackBprim(TfToken const &typeId)
{
    return CreateBprim(typeId, SdfPath::EmptyPath());
}

void HdVkRenderDelegate::DestroyBprim(HdBprim *bprim)
{
    _renderSettings.erase(std::remove(_renderSettings.begin(), _renderSettings.end(), bprim), _renderSettings.end());
    delete bprim;
}

void HdVkRenderDelegate::NoteSceneEdit(HdSceneDelegate *sceneDelegate)
{
    ++static_cast<HdVkRenderDelegate *>(sceneDelegate->GetRenderIndex().GetRenderDelegate())->_sceneEdits;
}

const HdRenderSettings *HdVkRenderDelegate::ActiveRenderSettings() const
{
    for (const HdRenderSettings *settings : _renderSettings) {
        if (settings->IsActive()) {
            return settings;
        }
    }
    return nullptr;
}

VtDictionary HdVkRenderDelegate::GetRenderStats() const
{
    HdVkTimedCall timed(HdVkCallTimes::kStats);
    VtDictionary stats;
    stats["rendererName"] = std::string("Eldr");  // plugInfo.json's displayName
    // HDVK_NO_STATS=1: the name only (A/B: what the app's stats cost).
    static const bool none = TfGetenvBool("HDVK_NO_STATS", false);
    if (!_device || !_renderer || none) {
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
    stats["rayTracingSupported"] = _device->rayTracing;  // else the eldr:rt* settings are ignored
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
    // Material inputs whose graphs Eldr couldn't compile whole (bypassed
    // or dropped nodes, unknown surfaces), and which nodes.
    stats["materialFallbackInputs"] = size_t(_materialFallbacks->Inputs());
    stats["materialFallbackNodes"] = _materialFallbacks->Nodes(12);

    // Convergence: TAA's average of still frames (usdview's "Samples done").
    const bool converged = _renderer->IsConverged();
    stats["numCompletedSamples"] = size_t(frame.taaSamples);
    stats["taaTarget"] = size_t(frame.taaTarget);
    // Houdini's viewport shows "<rendererStage> <percentDone>%" and the two
    // annotations (houdini/UsdRenderers.json maps them). The percentage is
    // the film's: samples over target, or with adaptive sampling the share of
    // pixels that stopped, whichever is further. Interactive TAA settles in a
    // blink after every camera move: counting it up would flicker, so it
    // reads "interactive 100%".
    const double total = std::max(1.0, double(frame.renderWidth) * double(frame.renderHeight));
    const double samplesDone = 100.0 * frame.taaSamples / std::max(frame.taaTarget, 1u);
    const double pixelsDone = frame.filmActivePixels ? 100.0 * (1.0 - frame.filmActivePixels / total) : 0.0;
    stats["percentDone"] = converged || !frame.film ? 100.0 : std::min(100.0, std::max(samplesDone, pixelsDone));
    stats["rendererStage"] = std::string(converged                ? "converged"
                                        : frame.film && frame.filmFinal ? "final"
                                        : frame.film                    ? "accumulating"
                                                                        : "interactive");
    stats["totalClockTime"] = frame.accumSeconds;  // time to converge once converged
    stats["pixelsConvergedPercent"] = converged ? 100.0 : pixelsDone;
    {
        char text[160];
        // Moving, TAA blends each frame's one sample into its history: no count to reach.
        if (!frame.film && frame.moving) {
            std::snprintf(text, sizeof text, "TAA interactive (moving)");
        } else {
            std::snprintf(text, sizeof text, "%s %u/%u samples%s", frame.film ? (frame.filmFinal ? "final" : "film") : "TAA",
                          frame.taaSamples, frame.taaTarget,
                          frame.filmActivePixels ? (", " + std::to_string(int(pixelsDone)) + "% of pixels done").c_str()
                                                 : "");
        }
        stats["renderProgressAnnotation"] = std::string(text);
        std::snprintf(text, sizeof text, "GPU %.1f ms, %llu triangles, %u draws, %.2f GB", frame.gpuMs,
                      (unsigned long long)frame.triangles, frame.drawCalls, memory.deviceLocal / 1073741824.0);
        stats["renderStatsAnnotation"] = std::string(text);
    }
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
#ifdef HDVK_HOUDINI
    // Houdini's viewport hands delegates an OpenGL Hgi but composites with
    // Vulkan: a shared GL texture shows nothing there, so it reads back.
    return nullptr;
#endif
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

void HdVkRenderDelegate::NoteCameraShutter(SdfPath const &camera, float open, float close)
{
    std::lock_guard<std::mutex> lock(_shutterMutex);
    _shutters[camera] = GfVec2f(open, close);
    _lastOpenCamera = close > open ? camera : _lastOpenCamera;
}

void HdVkRenderDelegate::SetActiveCamera(SdfPath const &camera)
{
    std::lock_guard<std::mutex> lock(_shutterMutex);
    _activeCamera = camera;
    _activeKnown = true;
}

bool HdVkRenderDelegate::SampledMotionShutter(float *open, float *close)
{
    if (!GetRenderSetting<bool>(HdVkRenderSettingsTokens2->sampledMotion, true)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_shutterMutex);
    // A render pass without a scene camera (usdview's free camera): none.
    const auto it = _shutters.find(_activeKnown ? _activeCamera : _lastOpenCamera);
    if (it == _shutters.end() || !(it->second[1] > it->second[0])) {
        return false;
    }
    *open = it->second[0];
    *close = it->second[1];
    return true;
}

void HdVkRenderDelegate::CommitResources(HdChangeTracker *)
{
    const auto start = std::chrono::steady_clock::now();
    // Nothing synced (camera-only frames: cameras note no edit) and no
    // setting changed since the last commit: the scene can skip its rebuild.
    const uint64_t edits = _sceneEdits;
    const unsigned settings = GetRenderSettingsVersion();
    const bool unchanged = edits == _committedEdits && settings == _committedSettings;
    _committedEdits = edits;
    _committedSettings = settings;
    try {
        _scene->Commit(false, unchanged);
        _commitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk upload failed: %s", e.what());
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
