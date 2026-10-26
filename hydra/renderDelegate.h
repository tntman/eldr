#pragma once

#include "pxr/pxr.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hd/sceneIndex.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/usd/sdf/path.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

namespace vkr {
class Device;
class Renderer;
class Scene;
}

PXR_NAMESPACE_OPEN_SCOPE

#define HDVK_RENDER_SETTING_TOKENS                           \
    ((postProcess, "eldr:postProcess"))                      \
    ((taa, "eldr:taa"))                                      \
    ((ambientOcclusion, "eldr:ambientOcclusion"))            \
    ((aoRadius, "eldr:aoRadius"))                            \
    ((ssr, "eldr:ssr"))                                      \
    ((autoExposure, "eldr:autoExposure"))                    \
    ((exposureCompensation, "eldr:exposureCompensation"))    \
    ((bloomIntensity, "eldr:bloomIntensity"))                \
    ((shadows, "eldr:shadows"))                              \
    ((localShadows, "eldr:localShadows"))                    \
    ((softShadows, "eldr:softShadows"))                      \
    ((taaSamples, "eldr:taaSamples"))                        \
    ((cullPixels, "eldr:cullPixels"))                        \
    ((depthOfField, "eldr:depthOfField"))                    \
    ((motionBlur, "eldr:motionBlur"))                        \
    ((depthPrepass, "eldr:depthPrepass"))                    \
    ((frustumCulling, "eldr:frustumCulling"))                \
    ((localLights, "eldr:localLights"))                      \
    ((clusteredLights, "eldr:clusteredLights"))              \
    ((imageBasedLighting, "eldr:imageBasedLighting"))        \
    ((normalMaps, "eldr:normalMaps"))                        \
    ((materialGraphs, "eldr:materialGraphs"))                \
    ((objectMotion, "eldr:objectMotion"))                    \
    ((translucency, "eldr:translucency"))                    \
    ((shadowResolution, "eldr:shadowResolution"))            \
    ((shadowCascades, "eldr:shadowCascades"))                \
    ((localShadowResolution, "eldr:localShadowResolution"))  \
    ((softShadowSamples, "eldr:softShadowSamples"))          \
    ((aoSlices, "eldr:aoSlices"))                            \
    ((ssrSteps, "eldr:ssrSteps"))                            \
    ((anisotropy, "eldr:anisotropy"))                        \
    ((proceduralTextures, "eldr:proceduralTextures"))        \
    ((tonemapper, "eldr:tonemapper"))                        \
    ((lensDistortion, "eldr:lensDistortion"))                \
    ((blendShapeMotion, "eldr:blendShapeMotion"))            \
    ((extraAovs, "eldr:extraAovs"))                          \
    ((shareFrame, "eldr:shareFrame"))                        \
    ((renderProducts, "eldr:renderProducts"))                \
    ((specularWorkflow, "eldr:specularWorkflow"))            \
    ((clearcoat, "eldr:clearcoat"))                          \
    ((sheen, "eldr:sheen"))                                  \
    ((thinFilm, "eldr:thinFilm"))                            \
    ((anisotropicSpecular, "eldr:anisotropicSpecular"))      \
    ((toon, "eldr:toon"))                                    \
    ((toonBands, "eldr:toonBands"))                          \
    ((hairShading, "eldr:hairShading"))                      \
    ((texturedLobes, "eldr:texturedLobes"))                  \
    ((texturedTransmission, "eldr:texturedTransmission"))    \
    ((rtShadows, "eldr:rtShadows"))                          \
    ((rtShadowSamples, "eldr:rtShadowSamples"))              \
    ((rtReflections, "eldr:rtReflections"))                  \
    ((rtReflectionSamples, "eldr:rtReflectionSamples"))      \
    ((rtSkinnedInstances, "eldr:rtSkinnedInstances"))        \
    ((gi, "eldr:gi"))                                        \
    ((giProbes, "eldr:giProbes"))                            \
    ((giRays, "eldr:giRaysPerProbe"))                        \
    ((giHysteresis, "eldr:giHysteresis"))                    \
    ((ssgi, "eldr:ssgi"))                                    \
    ((ssgiSteps, "eldr:ssgiSteps"))                          \
    ((ssgiRadius, "eldr:ssgiRadius"))                        \
    ((ssgiResolution, "eldr:ssgiResolution"))                \
    ((emissiveLights, "eldr:emissiveLights"))                \
    ((emissiveLightThreshold, "eldr:emissiveLightThreshold"))        \
    ((oit, "eldr:oit"))                                      \
    ((oitDepthWeight, "eldr:oitDepthWeight"))              \
    ((refraction, "eldr:refraction"))                        \
    ((refractionSamples, "eldr:refractionSamples"))              \
    ((refractionBackFaces, "eldr:refractionBackFaces"))              \
    ((multiRefraction, "eldr:multiRefraction"))              \
    ((refractionLayers, "eldr:refractionLayers"))              \
    ((translucentShadows, "eldr:translucentShadows"))        \
    ((translucentShadowDivisor, "eldr:translucentShadowDivisor")) \
    ((subsurface, "eldr:subsurface"))                        \
    ((subsurfaceSamples, "eldr:subsurfaceSamples"))              \
    ((holdouts, "eldr:holdouts"))              \
    ((holdoutAlpha, "eldr:holdoutAlpha")) \
    ((softSunShadows, "eldr:softSunShadows"))                \
    ((fog, "eldr:fog"))                                      \
    ((fogDensity, "eldr:fogDensity"))                        \
    ((fogHeight, "eldr:fogHeight"))                          \
    ((fogHeightFalloff, "eldr:fogHeightFalloff"))            \
    ((fogAnisotropy, "eldr:fogAnisotropy"))                  \
    ((fogColor, "eldr:fogColor"))                            \
    ((fogMaxDistance, "eldr:fogMaxDistance"))                \
    ((fogGridSize, "eldr:fogGridSize"))                      \
    ((fogZUp, "eldr:fogZUp"))                                \
    ((reflectionProbe, "eldr:reflectionProbe"))              \
    ((probeResolution, "eldr:probeResolution"))              \
    ((probePosition, "eldr:probePosition"))                  \
    ((planarReflections, "eldr:planarReflections"))          \
    ((planarScale, "eldr:planarScale"))                      \
    ((volumes, "eldr:volumes"))                              \
    ((volumeStepSize, "eldr:volumeStepSize"))                \
    ((volumeMaxSteps, "eldr:volumeMaxSteps"))                \
    ((volumeShadowSteps, "eldr:volumeShadowSteps"))          \
    ((volumeDensity, "eldr:volumeDensity"))                  \
    ((volumeEmission, "eldr:volumeEmission"))                \
    ((volumeAnisotropy, "eldr:volumeAnisotropy"))            \
    ((volumeResolution, "eldr:volumeResolution"))            \
    ((volumeUnderTransparency, "eldr:volumeUnderTransparency")) \
    ((volumeShadows, "eldr:volumeShadows")) \
    ((subdivision, "eldr:subdivision"))                      \
    ((maxRefineLevel, "eldr:maxRefineLevel"))                \
    ((displacement, "eldr:displacement"))                    \
    ((displacementScale, "eldr:displacementScale"))          \
    ((lod, "eldr:lod"))                                      \
    ((lodBias, "eldr:lodBias"))                              \
    ((filmAccumulation, "eldr:filmAccumulation"))            \
    ((filmSamples, "eldr:filmSamples"))                      \
    ((filmFilterRadius, "eldr:filmFilterRadius"))            \
    ((finalQuality, "eldr:finalQuality"))                    \
    ((sampleDimensions, "eldr:sampleDimensions"))            \
    ((interactiveSamples, "eldr:interactiveSamples"))        \
    ((adaptiveThreshold, "eldr:adaptiveThreshold"))          \
    ((adaptiveMinSamples, "eldr:adaptiveMinSamples"))        \
    ((giPreconverge, "eldr:giPreconverge"))                  \
    ((fogFilmJitter, "eldr:fogFilmJitter"))                  \
    ((pipelinePrewarm, "eldr:pipelinePrewarm"))              \
    ((pipelineCache, "eldr:pipelineCache"))

// MSVC cannot expand one token list of more than about 140 names (C1009), so the
// settings of later lanes live in a second one.
#define HDVK_RENDER_SETTING_TOKENS2                          \
    ((directClamp, "eldr:directClamp"))                      \
    ((indirectClamp, "eldr:indirectClamp"))                  \
    ((ssgiThickness, "eldr:ssgiThickness"))                  \
    ((ssgiBitmask, "eldr:ssgiBitmask"))                      \
    ((rtDisplacement, "eldr:rtDisplacement"))                \
    ((ssrDenoise, "eldr:ssrDenoise"))                        \
    ((shadowJitter, "eldr:shadowJitter"))                    \
    ((finalShadows, "eldr:finalShadows"))                    \
    ((rtTranslucentShadows, "eldr:rtTranslucentShadows"))    \
    ((dofJitter, "eldr:dofJitter")) \
    ((lightTextures, "eldr:lightTextures")) \
    ((domeSun, "eldr:domeSun")) \
    ((domeSunThreshold, "eldr:domeSunThreshold")) \
    ((fogGiAmbient, "eldr:fogGiAmbient")) \
    ((authoredProbes, "eldr:authoredProbes")) \
    ((probeNormalize, "eldr:probeNormalize")) \
    ((planarCount, "eldr:planarCount")) \
    ((motionBlurTimeSamples, "eldr:motionBlurTimeSamples")) \
    ((renderScale, "eldr:renderScale")) \
    ((productLinear, "eldr:productLinear")) \
    ((ssgiBitmaskThickness, "eldr:ssgiBitmaskThickness")) \
    ((adaptiveFinalThreshold, "eldr:adaptiveFinalThreshold")) \
    ((adaptivePixelFraction, "eldr:adaptivePixelFraction")) \
    ((curveRibbons, "eldr:curveRibbons")) \
    ((pointSprites, "eldr:pointSprites")) \
    ((lodFade, "eldr:lodFade")) \
    ((displacementGraphs, "eldr:displacementGraphs")) \
    ((splitDomeDirect, "eldr:splitDomeDirect")) \
    ((cryptomatteSoft, "eldr:cryptomatteSoft")) \
    ((directIntensity, "eldr:directIntensity")) \
    ((indirectIntensity, "eldr:indirectIntensity")) \
    ((sampledMotion, "eldr:sampledMotion")) \
    ((deformationBlur, "eldr:deformationBlur")) \
    ((dofResidualBlur, "eldr:dofResidualBlur")) \
    ((motionBlurResidual, "eldr:motionBlurResidual")) \
    ((productResolution, "eldr:productResolution")) \
    ((localVolumeShadows, "eldr:localVolumeShadows")) \
    ((lightShapesVisible, "eldr:lightShapesVisible")) \
    ((screenBackface, "eldr:screenBackface")) \
    ((screenBackfaceScale, "eldr:screenBackfaceScale")) \
    ((ssrHitFilter, "eldr:ssrHitFilter")) \
    ((ssgiTileOrder, "eldr:ssgiTileOrder"))     ((rtRefraction, "eldr:rtRefraction"))     ((rtRefractionBounces, "eldr:rtRefractionBounces")) ((rtRefractionDisplacement, "eldr:rtRefractionDisplacement"))

TF_DECLARE_PUBLIC_TOKENS(HdVkRenderSettingsTokens, HDVK_RENDER_SETTING_TOKENS);
TF_DECLARE_PUBLIC_TOKENS(HdVkRenderSettingsTokens2, HDVK_RENDER_SETTING_TOKENS2);

// Karma's names for what Eldr's extra AOVs hold: the source names of the
// render vars Houdini's Karma Standard Render Vars LOP makes, as UsdRender
// hands them to Hydra (raw names, "lpe:" and "primvars:" ones).
#define HDVK_AOV_TOKENS                                     \
    ((hitN, "ray:hitN"))                                    \
    ((hitNFacing, "ray:hitN_facing"))                       \
    ((hitNCamera, "ray:hitN_camera"))                       \
    ((hitP, "ray:hitP"))                                    \
    ((hitPCamera, "ray:hitP_camera"))                       \
    ((hitPz, "ray:hitPz"))                                  \
    ((baseColor, "export_basecolor"))                       \
    ((diffuseAlbedo, "lpe:CDA"))                            \
    ((directEmission, "lpe:CO"))                            \
    ((combinedEmission, "lpe:C.*O"))                        \
    ((directDiffuse, "lpe:C<RD>L"))                         \
    ((indirectDiffuse, "lpe:C<RD>.+L"))                     \
    ((combinedDiffuse, "lpe:C<RD>.*L"))                     \
    ((directGlossy, "lpe:C<RG>L"))                          \
    ((indirectGlossy, "lpe:C<RG>.+L"))                      \
    ((combinedGlossy, "lpe:C<RG>.*[LO]"))                   \
    ((cryptoObject, "primvars:__name"))                     \
    ((cryptoMaterial, "primvars:__materialname"))

TF_DECLARE_PUBLIC_TOKENS(HdVkAovTokens, HDVK_AOV_TOKENS);

// Extra AOVs (vkr::Aovs::Kind) by the names Hydra binds them with:
// HdAovTokens' (usdview's AOV menu) and Karma's (HdVkAovTokens). `format` is
// what GetDefaultAovDescriptor gives; a bound buffer of any float format
// gets as many components.
struct HdVkExtraAov {
    TfToken name;
    uint32_t kind;
    HdFormat format;
};
const std::vector<HdVkExtraAov> &HdVkExtraAovs();

// A render tag as a bit of vkr::Mesh::tag: geometry, render, proxy and guide
// have their own, other tags share one, hidden has none (never drawn).
uint32_t HdVkRenderTagBit(TfToken const &tag);

// Dirties every mesh's display style (or other `type` prims' primvars) in
// the scene upstream of `terminal` (a render index's), so they sync again
// (sceneIndexPlugin.cpp).
void HdVkDirtyMeshes(HdSceneIndexBaseRefPtr const &terminal, TfToken const &type = HdPrimTypeTokens->mesh);

class HdVkTextureCache;
class HdVkGLInterop;
class HdRenderSettings;
class Hgi;

class HdVkRenderDelegate final : public HdRenderDelegate
{
public:
    explicit HdVkRenderDelegate(HdRenderSettingsMap const &settings = {});
    ~HdVkRenderDelegate() override;

    const TfTokenVector &GetSupportedRprimTypes() const override;
    const TfTokenVector &GetSupportedSprimTypes() const override;
    const TfTokenVector &GetSupportedBprimTypes() const override;
    HdResourceRegistrySharedPtr GetResourceRegistry() const override;
    HdAovDescriptor GetDefaultAovDescriptor(TfToken const &name) const override;
    HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;
    TfTokenVector GetMaterialRenderContexts() const override;

    HdRenderPassSharedPtr CreateRenderPass(
        HdRenderIndex *index, HdRprimCollection const &collection) override;

    HdInstancer *CreateInstancer(HdSceneDelegate *delegate,
                                 SdfPath const &id) override;
    void DestroyInstancer(HdInstancer *instancer) override;

    HdRprim *CreateRprim(TfToken const &typeId, SdfPath const &rprimId) override;
    void DestroyRprim(HdRprim *rprim) override;

    HdSprim *CreateSprim(TfToken const &typeId, SdfPath const &sprimId) override;
    HdSprim *CreateFallbackSprim(TfToken const &typeId) override;
    void DestroySprim(HdSprim *sprim) override;

    HdBprim *CreateBprim(TfToken const &typeId, SdfPath const &bprimId) override;
    HdBprim *CreateFallbackBprim(TfToken const &typeId) override;
    void DestroyBprim(HdBprim *bprim) override;

    void CommitResources(HdChangeTracker *tracker) override;
    // Memory, timings and what the last frame drew: usdview's GPU Stats HUD
    // and HdVk's stats window (usdview/hdvkUsdview) read it, and Houdini's
    // render stats through houdini/UsdRenderers.json's statsdatapaths.
    VtDictionary GetRenderStats() const override;
    // Settings the meshes read when they sync (subdivision) re-sync them.
    void SetRenderSetting(TfToken const &key, VtValue const &value) override;
    void SetTerminalSceneIndex(HdSceneIndexBaseRefPtr const &terminalSceneIndex) override;

    // Cryptomatte manifests of the last frame with those AOVs (JSON: name
    // to hex id), reported in GetRenderStats()["cryptomatte"].
    void SetCryptomatteManifests(std::string object, std::string material);

    // GPU sharing: the app's Hgi comes as a driver (HgiTokens->renderDriver).
    void SetDrivers(HdDriverVector const &drivers) override;
    // Hands frames to the app's OpenGL on the GPU (made on first use, with
    // its context current); null when that can't work.
    HdVkGLInterop *GLInterop();
    // After it failed: read back from then on.
    void DisableGLInterop();
    // The active render settings prim (UsdRenderSettings), if any.
    const HdRenderSettings *ActiveRenderSettings() const;
    // Whether the last frame went to OpenGL on the GPU (GetRenderStats).
    void SetFrameShared(bool shared) { _frameShared = shared; }
    // Sampled motion (eldr:sampledMotion): the shutter (frame-relative open,
    // close) rprims sample their transforms over. Cameras note theirs when
    // they sync, before rprims do; the render pass names the one it uses.
    // False when off or without an open shutter.
    void NoteCameraShutter(SdfPath const &camera, float open, float close);
    void SetActiveCamera(SdfPath const &camera);
    bool SampledMotionShutter(float *open, float *close);

private:
    HdResourceRegistrySharedPtr _resourceRegistry;
    // Declaration order is destruction order in reverse: renderer and scene
    // release GPU resources before the device goes away.
    std::unique_ptr<vkr::Device> _device;     // null if Vulkan init failed
    std::unique_ptr<vkr::Scene> _scene;       // always present (CPU mirror)
    std::unique_ptr<vkr::Renderer> _renderer; // null if Vulkan init failed
    std::unique_ptr<HdVkTextureCache> _textures;
    double _commitMs = 0;  // the last CommitResources (scene upload)
    HdSceneIndexBasePtr _terminalSceneIndex;
    std::string _cryptomatteObject, _cryptomatteMaterial;
    // GPU sharing; declared last: released before the renderer and device.
    Hgi *_hgi = nullptr;
    bool _interopTried = false, _frameShared = false;
    std::vector<HdRenderSettings *> _renderSettings;  // render settings Bprims
    std::unique_ptr<HdVkGLInterop> _interop;
    // Sampled motion: the active camera's shutter, else the last synced
    // open one's (a first frame's camera hasn't been named yet).
    // ponytail: a shutter edit doesn't re-sync the rprims.
    std::mutex _shutterMutex;
    std::unordered_map<SdfPath, GfVec2f, SdfPath::Hash> _shutters;
    SdfPath _activeCamera, _lastOpenCamera;
    bool _activeKnown = false;  // a render pass named its camera
};

PXR_NAMESPACE_CLOSE_SCOPE
