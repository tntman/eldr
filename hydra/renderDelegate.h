#pragma once

#include "pxr/pxr.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/dataSourceLocator.h"

#include "subdivision.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

namespace vkr {
class Device;
class Renderer;
class Scene;
struct VolumeGrid;
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
    ((shadowCullPixels, "eldr:shadowCullPixels"))            \
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
    ((colorLimit, "eldr:colorLimit"))                        \
    ((colorLimitShared, "eldr:colorLimitShared"))            \
    ((opacityMode, "eldr:opacityMode"))                      \
    ((ssgiThickness, "eldr:ssgiThickness"))                  \
    ((ssgiBitmask, "eldr:ssgiBitmask"))                      \
    ((rtDisplacement, "eldr:rtDisplacement"))                \
    ((ssrDenoise, "eldr:ssrDenoise"))                        \
    ((shadowJitter, "eldr:shadowJitter"))                    \
    ((finalShadows, "eldr:finalShadows"))                    \
    ((rtTranslucentShadows, "eldr:rtTranslucentShadows"))    \
    ((dofJitter, "eldr:dofJitter")) \
    ((lightTextures, "eldr:lightTextures")) \
    ((lightLinking, "eldr:lightLinking")) \
    ((lightIes, "eldr:lightIes")) \
    ((domeSun, "eldr:domeSun")) \
    ((domeSunThreshold, "eldr:domeSunThreshold")) \
    ((fogGiAmbient, "eldr:fogGiAmbient")) \
    ((authoredProbes, "eldr:authoredProbes")) \
    ((probeNormalize, "eldr:probeNormalize")) \
    ((planarCount, "eldr:planarCount")) \
    ((planarBlur, "eldr:planarBlur")) \
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
    ((aovFilters, "eldr:aovFilters")) \
    ((directIntensity, "eldr:directIntensity")) \
    ((indirectIntensity, "eldr:indirectIntensity")) \
    ((sampledMotion, "eldr:sampledMotion")) \
    ((deformationBlur, "eldr:deformationBlur")) \
    ((dofResidualBlur, "eldr:dofResidualBlur")) \
    ((motionBlurResidual, "eldr:motionBlurResidual")) \
    ((planarMotionBlur, "eldr:planarMotionBlur")) \
    ((productResolution, "eldr:productResolution")) \
    ((localVolumeShadows, "eldr:localVolumeShadows")) \
    ((volumeMutualShadows, "eldr:volumeMutualShadows")) ((volumeMultipleScattering, "eldr:volumeMultipleScattering"))     ((volumeDefaultFields, "eldr:volumeDefaultFields")) \
    ((volumeVelocityBlur, "eldr:volumeVelocityBlur")) \
    ((lightShapesVisible, "eldr:lightShapesVisible")) \
    ((domeCameraVisible, "eldr:domeCameraVisible")) \
    ((multiScatter, "eldr:multiScatter")) \
    ((overlapReadback, "eldr:overlapReadback")) \
    ((inPlaceReadback, "eldr:inPlaceReadback")) \
    ((offlineBatchSeconds, "eldr:offlineBatchSeconds")) \
    ((staleMovingIds, "eldr:staleMovingIds")) \
    ((lazyPipelines, "eldr:lazyPipelines")) \
    ((lazyExtraAovs, "eldr:lazyExtraAovs")) \
    ((screenBackface, "eldr:screenBackface")) \
    ((screenBackfaceScale, "eldr:screenBackfaceScale")) \
    ((ssrHitFilter, "eldr:ssrHitFilter")) \
    ((ssgiTileOrder, "eldr:ssgiTileOrder"))     ((rtRefraction, "eldr:rtRefraction"))     ((rtRefractionBounces, "eldr:rtRefractionBounces")) ((rtRefractionDisplacement, "eldr:rtRefractionDisplacement")) \
    ((rtHitShadows, "eldr:rtHitShadows"))     ((giRelocation, "eldr:giRelocation"))     ((finalGather, "eldr:finalGather"))     ((finalGatherDistance, "eldr:finalGatherDistance")) ((finalGatherRays, "eldr:finalGatherRays")) ((finalGatherDenoise, "eldr:finalGatherDenoise"))     ((effectDenoiseSamples, "eldr:effectDenoiseSamples")) ((rtShadowDenoise, "eldr:rtShadowDenoise")) ((rtReflectionDenoise, "eldr:rtReflectionDenoise")) \
    ((screenDicing, "eldr:screenDicing")) \
    ((denoise, "eldr:denoise")) \
    ((adaptiveTiles, "eldr:adaptiveTiles")) \
    ((lightQuality, "eldr:lightQuality")) ((diffuseQuality, "eldr:diffuseQuality")) ((reflectQuality, "eldr:reflectQuality")) ((sssQuality, "eldr:sssQuality")) ((volumeQuality, "eldr:volumeQuality")) \
    ((caustics, "eldr:caustics")) \
    ((causticsPhotons, "eldr:causticsPhotons")) \
    ((causticsRadius, "eldr:causticsRadius")) \
    ((rtCompaction, "eldr:rtCompaction")) \
    ((textureCompression, "eldr:textureCompression"))

TF_DECLARE_PUBLIC_TOKENS(HdVkRenderSettingsTokens, HDVK_RENDER_SETTING_TOKENS);
TF_DECLARE_PUBLIC_TOKENS(HdVkRenderSettingsTokens2, HDVK_RENDER_SETTING_TOKENS2);

// Karma's names for what Eldr's extra AOVs hold: the source names of the
// render vars Houdini's Karma Standard Render Vars LOP makes, as UsdRender
// hands them to Hydra (raw names and "primvars:" ones; light path
// expressions: HdVkFindAov).
#define HDVK_AOV_TOKENS                                     \
    ((hitN, "ray:hitN"))                                    \
    ((hitNFacing, "ray:hitN_facing"))                       \
    ((hitNCamera, "ray:hitN_camera"))                       \
    ((hitP, "ray:hitP"))                                    \
    ((hitPCamera, "ray:hitP_camera"))                       \
    ((hitPz, "ray:hitPz"))                                  \
    ((baseColor, "export_basecolor"))                       \
    ((cryptoObject, "primvars:__name"))                     \
    ((cryptoMaterial, "primvars:__materialname"))           \
    ((beautyRaw, "eldr:beautyRaw"))                         \
    ((caustics, "caustics"))

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
// The extra AOV a name binds: one of HdVkExtraAovs, or a light path
// expression ("lpe:...") of the forms Karma writes (renderDelegate.cpp's
// LpeKind; format Float32Vec3). False for any other name.
bool HdVkFindAov(TfToken const &name, HdVkExtraAov *aov);
// Light groups: Karma's LPE tags (inputs:karma:light:lpetag) in the order
// AOVs first asked for them, at most vkr::Aovs::kMaxLightGroups (another
// gets -1 and a warning). Process-wide.
int HdVkLightGroup(const std::string &tag);
std::vector<std::string> HdVkLightGroups();
// Light linking: Hydra's link categories (HdsiLightLinkingSceneIndex's ids
// of UsdLux collection:lightLink / shadowLink) as vkr ids (Mesh::
// linkCategories, Light::lightLink), the empty one 0. A light naming
// (`light`) more than vkr::Scene::kMaxLinkCategories warns. Process-wide.
uint32_t HdVkLinkCategory(TfToken const &category, bool light = false);
std::vector<uint32_t> HdVkLinkCategories(VtArray<TfToken> const &categories);

// A render tag as a bit of vkr::Mesh::tag: geometry, render, proxy and guide
// have their own, other tags share one, hidden has none (never drawn).
uint32_t HdVkRenderTagBit(TfToken const &tag);

// Dirties `locators` of every prim of `types` in the scene upstream of
// `terminal` (a render index's), so they sync again (sceneIndexPlugin.cpp).
void HdVkDirtyPrims(HdSceneIndexBaseRefPtr const &terminal, TfTokenVector const &types,
                    HdDataSourceLocatorSet const &locators);

class HdRenderSettings;
// The sourceName/sourceType of the render var named `var` (the prim's name) in
// the settings prim's products: Houdini's viewport binds a var by that name
// alone. False when no such var.
bool HdVkRenderVarSource(const HdRenderSettings *settings, TfToken const &var, std::string *name, std::string *type);

class HdVkTextureCache;
class HdVkMaterialFallbacks;
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
    // GetRenderStats plus husk:productmetadata as JSON: the husk:render_stats metadata of render products.
    std::string RenderStatsJson() const;
    // Settings the meshes read when they sync (subdivision) re-sync them.
    void SetRenderSetting(TfToken const &key, VtValue const &value) override;
    void SetTerminalSceneIndex(HdSceneIndexBaseRefPtr const &terminalSceneIndex) override;

    // Cryptomatte manifests of the last frame with those AOVs (JSON: name
    // to hex id), reported in GetRenderStats()["cryptomatte"].
    void SetCryptomatteManifests(std::string object, std::string material);
    double CommitMs() const { return _commitMs; }  // the last CommitResources
    // After a frame rendered: when it followed a load (new scene, new
    // shaders: over 0.25 s of work since the last frame), sums the load's
    // times into the one-line summary (stats "loadLine") and prints it.
    void NoteFrame();

    // GPU sharing: the app's Hgi comes as a driver (HgiTokens->renderDriver).
    void SetDrivers(HdDriverVector const &drivers) override;
    // Hands frames to the app's OpenGL on the GPU (made on first use, with
    // its context current); null when that can't work.
    HdVkGLInterop *GLInterop();
    // After it failed: read back from then on.
    void DisableGLInterop();
    // The active render settings prim (UsdRenderSettings), if any.
    const HdRenderSettings *ActiveRenderSettings() const;
    // The settings prim to look render var `var` up in: the active one, else
    // (Houdini's viewport marks none active) any whose products have it.
    const HdRenderSettings *RenderSettingsWithVar(TfToken const &var) const;
    // Whether the last frame went to OpenGL on the GPU (GetRenderStats).
    void SetFrameShared(bool shared) { _frameShared = shared; }
    // Sampled motion (eldr:sampledMotion): the shutter (frame-relative open,
    // close) rprims sample their transforms over. Cameras note theirs when
    // they sync, before rprims do; the render pass names the one it uses.
    // False when off or without an open shutter.
    void NoteCameraShutter(SdfPath const &camera, float open, float close);
    void SetActiveCamera(SdfPath const &camera);
    bool SampledMotionShutter(float *open, float *close);
    // A motion blur setting changed since the last commit: rprims derive
    // their motion again (transforms, deformation velocities) even where
    // nothing moved since their last sync.
    bool MotionSettingsChanged() const { return _motionSettingsChanged; }
    // The meshes re-sync for the same frame (re-dicing, a subdivision
    // setting): the next commit keeps the last one's motion where it moves
    // nothing itself (a re-diced mesh uploads geometry, not its motion).
    void KeepMotion() { _keepMotion = true; }
    // What the film starts over on besides the camera and the settings:
    // every sync of geometry, instancers, materials, volumes and fields
    // (lights through the renderer's lighting hash). Not cameras: an app may
    // sync its camera prim again unchanged (Houdini's free camera is a
    // prim); the view is in the frame's matrices, the pass hashes the lens.
    static void NoteSceneEdit(HdSceneDelegate *sceneDelegate);
    uint64_t SceneEdits() const { return _sceneEdits; }

    // Who renders: Houdini's viewport (houdini:viewport or
    // houdini:interactive), or an offline renderer (husk's
    // rasterRenderProducts, enableInteractive false): final quality.
    bool HoudiniViewport() const;
    bool Offline() const;
    // eldr:screenDicing in final quality (eldr:finalQuality or offline):
    // subdivision surfaces dice for DicingView, the view the render pass
    // last re-diced for (null before the first). SetDicingView re-syncs the
    // meshes; only the render pass calls it, between syncs.
    bool ScreenDicing() const;
    // Whether materials load block-compressed textures now.
    bool TextureCompression() const;
    const HdVkDicingView *DicingView() const { return _dicingView ? &*_dicingView : nullptr; }
    void SetDicingView(std::optional<HdVkDicingView> const &view);

    // The descriptor of an AOV Eldr knows by `name`; invalid otherwise.
    static HdAovDescriptor KnownAovDescriptor(TfToken const &name);

private:
    std::atomic<uint64_t> _sceneEdits{0};  // rprims sync in parallel
    // Fields' grids by path (CreateBprim). ponytail: entries of removed
    // fields stay (an expired pointer each).
    std::unordered_map<SdfPath, std::weak_ptr<vkr::VolumeGrid>, SdfPath::Hash> _fieldGrids;
    HdResourceRegistrySharedPtr _resourceRegistry;
    // Declaration order is destruction order in reverse: renderer and scene
    // release GPU resources before the device goes away.
    std::unique_ptr<vkr::Device> _device;     // null if Vulkan init failed
    std::unique_ptr<vkr::Scene> _scene;       // always present (CPU mirror)
    std::unique_ptr<vkr::Renderer> _renderer; // null if Vulkan init failed
    std::unique_ptr<HdVkTextureCache> _textures;
    std::unique_ptr<HdVkMaterialFallbacks> _materialFallbacks;
    double _commitMs = 0;  // the last CommitResources (scene upload)
    mutable std::mutex _loadMutex;
    double _loadSyncMs = 0;  // CommitResources' time since the last NoteFrame
    std::string _loadLine;   // the last load's summary
    uint64_t _committedEdits = ~0ull;   // _sceneEdits at the last CommitResources
    unsigned _committedSettings = ~0u;  // and the render settings' version
    bool _motionSettingsChanged = false;  // since the last CommitResources
    bool _keepMotion = false;             // KeepMotion since the last CommitResources
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
    std::optional<HdVkDicingView> _dicingView;
};

PXR_NAMESPACE_CLOSE_SCOPE
