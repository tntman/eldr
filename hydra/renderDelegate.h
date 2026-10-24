#pragma once

#include "pxr/pxr.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hd/sceneIndex.h"

#include <memory>
#include <string>
#include <vector>

namespace vkr {
class Device;
class Renderer;
class Scene;
}

PXR_NAMESPACE_OPEN_SCOPE

#define HDVK_RENDER_SETTING_TOKENS                           \
    ((postProcess, "hdvk:postProcess"))                      \
    ((taa, "hdvk:taa"))                                      \
    ((ambientOcclusion, "hdvk:ambientOcclusion"))            \
    ((aoRadius, "hdvk:aoRadius"))                            \
    ((ssr, "hdvk:ssr"))                                      \
    ((autoExposure, "hdvk:autoExposure"))                    \
    ((exposureCompensation, "hdvk:exposureCompensation"))    \
    ((bloomIntensity, "hdvk:bloomIntensity"))                \
    ((shadows, "hdvk:shadows"))                              \
    ((localShadows, "hdvk:localShadows"))                    \
    ((softShadows, "hdvk:softShadows"))                      \
    ((taaSamples, "hdvk:taaSamples"))                        \
    ((cullPixels, "hdvk:cullPixels"))                        \
    ((depthOfField, "hdvk:depthOfField"))                    \
    ((motionBlur, "hdvk:motionBlur"))                        \
    ((depthPrepass, "hdvk:depthPrepass"))                    \
    ((frustumCulling, "hdvk:frustumCulling"))                \
    ((localLights, "hdvk:localLights"))                      \
    ((clusteredLights, "hdvk:clusteredLights"))              \
    ((imageBasedLighting, "hdvk:imageBasedLighting"))        \
    ((normalMaps, "hdvk:normalMaps"))                        \
    ((materialGraphs, "hdvk:materialGraphs"))                \
    ((objectMotion, "hdvk:objectMotion"))                    \
    ((translucency, "hdvk:translucency"))                    \
    ((shadowResolution, "hdvk:shadowResolution"))            \
    ((shadowCascades, "hdvk:shadowCascades"))                \
    ((localShadowResolution, "hdvk:localShadowResolution"))  \
    ((softShadowSamples, "hdvk:softShadowSamples"))          \
    ((aoSlices, "hdvk:aoSlices"))                            \
    ((ssrSteps, "hdvk:ssrSteps"))                            \
    ((anisotropy, "hdvk:anisotropy"))                        \
    ((proceduralTextures, "hdvk:proceduralTextures"))        \
    ((tonemapper, "hdvk:tonemapper"))                        \
    ((lensDistortion, "hdvk:lensDistortion"))                \
    ((blendShapeMotion, "hdvk:blendShapeMotion"))            \
    ((extraAovs, "hdvk:extraAovs"))                          \
    ((shareFrame, "hdvk:shareFrame"))                        \
    ((specularWorkflow, "hdvk:specularWorkflow"))            \
    ((clearcoat, "hdvk:clearcoat"))                          \
    ((sheen, "hdvk:sheen"))                                  \
    ((thinFilm, "hdvk:thinFilm"))                            \
    ((anisotropicSpecular, "hdvk:anisotropicSpecular"))      \
    ((toon, "hdvk:toon"))                                    \
    ((toonBands, "hdvk:toonBands"))                          \
    ((hairShading, "hdvk:hairShading"))                      \
    ((rtShadows, "hdvk:rtShadows"))                          \
    ((rtShadowSamples, "hdvk:rtShadowSamples"))              \
    ((rtReflections, "hdvk:rtReflections"))                  \
    ((rtReflectionSamples, "hdvk:rtReflectionSamples"))      \
    ((rtSkinnedInstances, "hdvk:rtSkinnedInstances"))        \
    ((gi, "hdvk:gi"))                                        \
    ((giProbes, "hdvk:giProbes"))                            \
    ((giRays, "hdvk:giRaysPerProbe"))                        \
    ((giHysteresis, "hdvk:giHysteresis"))                    \
    ((ssgi, "hdvk:ssgi"))                                    \
    ((ssgiSteps, "hdvk:ssgiSteps"))                          \
    ((ssgiRadius, "hdvk:ssgiRadius"))                        \
    ((ssgiResolution, "hdvk:ssgiResolution"))                \
    ((emissiveLights, "hdvk:emissiveLights"))                \
    ((emissiveLightThreshold, "hdvk:emissiveLightThreshold"))        \
    ((oit, "hdvk:oit"))                                      \
    ((oitDepthWeight, "hdvk:oitDepthWeight"))              \
    ((refraction, "hdvk:refraction"))                        \
    ((refractionSamples, "hdvk:refractionSamples"))              \
    ((translucentShadows, "hdvk:translucentShadows"))        \
    ((translucentShadowDivisor, "hdvk:translucentShadowDivisor")) \
    ((subsurface, "hdvk:subsurface"))                        \
    ((subsurfaceSamples, "hdvk:subsurfaceSamples"))              \
    ((holdouts, "hdvk:holdouts")) \
    ((softSunShadows, "hdvk:softSunShadows"))                \
    ((fog, "hdvk:fog"))                                      \
    ((fogDensity, "hdvk:fogDensity"))                        \
    ((fogHeight, "hdvk:fogHeight"))                          \
    ((fogHeightFalloff, "hdvk:fogHeightFalloff"))            \
    ((fogAnisotropy, "hdvk:fogAnisotropy"))                  \
    ((fogColor, "hdvk:fogColor"))                            \
    ((fogMaxDistance, "hdvk:fogMaxDistance"))                \
    ((fogGridSize, "hdvk:fogGridSize"))                      \
    ((fogZUp, "hdvk:fogZUp"))                                \
    ((reflectionProbe, "hdvk:reflectionProbe"))              \
    ((probeResolution, "hdvk:probeResolution"))              \
    ((probePosition, "hdvk:probePosition"))                  \
    ((authoredProbes, "hdvk:authoredProbes"))            \
    ((planarReflections, "hdvk:planarReflections"))          \
    ((planarScale, "hdvk:planarScale"))                      \
    ((volumes, "hdvk:volumes"))                              \
    ((volumeStepSize, "hdvk:volumeStepSize"))                \
    ((volumeMaxSteps, "hdvk:volumeMaxSteps"))                \
    ((volumeShadowSteps, "hdvk:volumeShadowSteps"))          \
    ((volumeDensity, "hdvk:volumeDensity"))                  \
    ((volumeEmission, "hdvk:volumeEmission"))                \
    ((volumeAnisotropy, "hdvk:volumeAnisotropy"))            \
    ((volumeResolution, "hdvk:volumeResolution"))            \
    ((subdivision, "hdvk:subdivision"))                      \
    ((maxRefineLevel, "hdvk:maxRefineLevel"))                \
    ((displacement, "hdvk:displacement"))                    \
    ((displacementScale, "hdvk:displacementScale"))          \
    ((lod, "hdvk:lod"))                                      \
    ((lodBias, "hdvk:lodBias"))

TF_DECLARE_PUBLIC_TOKENS(HdVkRenderSettingsTokens, HDVK_RENDER_SETTING_TOKENS);

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

// Dirties every mesh's display style in the scene upstream of `terminal` (a
// render index's), so they sync again (sceneIndexPlugin.cpp).
void HdVkDirtyMeshes(HdSceneIndexBaseRefPtr const &terminal);

class HdVkTextureCache;
class HdVkGLInterop;
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
    // Whether the last frame went to OpenGL on the GPU (GetRenderStats).
    void SetFrameShared(bool shared) { _frameShared = shared; }

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
    std::unique_ptr<HdVkGLInterop> _interop;
};

PXR_NAMESPACE_CLOSE_SCOPE
