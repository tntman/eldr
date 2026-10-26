#include "renderPass.h"

#include "camera.h"
#include "glInterop.h"
#include "renderBuffer.h"
#include "renderDelegate.h"
#include "renderer.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/cameraUtil/conformWindow.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/types.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// A product's projection (renderPass's productSized): the camera's own
// conformed to the product's image aspect (resolution times pixel aspect
// ratio) by its aspectRatioConformPolicy, then its data window's part of
// the image stretched over the frame.
GfMatrix4d ProductProjection(GfMatrix4d projection, HdVkRenderProducts::Framing const &f)
{
    const TfToken &p = f.conformPolicy;
    const CameraUtilConformWindowPolicy policy = p == TfToken("cropAperture")             ? CameraUtilCrop
                                               : p == TfToken("adjustApertureWidth")      ? CameraUtilMatchVertically
                                               : p == TfToken("adjustApertureHeight")     ? CameraUtilMatchHorizontally
                                               : p == TfToken("adjustPixelAspectRatio") ? CameraUtilDontConform
                                                                                          : CameraUtilFit;
    projection = CameraUtilConformedWindow(projection, policy,
                                           double(f.resolution[0]) * f.pixelAspectRatio / double(f.resolution[1]));
    GfMatrix4d crop(1.0);  // NDC x' = (x - centre) / half size (row vectors: clip * crop)
    for (int k = 0; k < 2; ++k) {
        const double lo = 2.0 * f.window[k] / f.resolution[k] - 1.0, hi = 2.0 * f.window[k + 2] / f.resolution[k] - 1.0;
        crop[k][k] = 2.0 / (hi - lo);
        crop[3][k] = -(hi + lo) / (hi - lo);
    }
    return projection * crop;
}

// The AOV a binding asks for. husk names its bindings by its own channel
// names ("C", "depth", "primid") and carries UsdRender's source in the
// binding's settings; that source (as UsdRender names it: lpe: or primvars:
// prefixed, else raw) wins. usdview and usdrecord bind by Hydra's names.
// A string (or token) setting of an AOV binding, "" when absent.
std::string BindingText(HdRenderPassAovBinding const &b, const char *key)
{
    const auto it = b.aovSettings.find(TfToken(key));
    if (it == b.aovSettings.end()) {
        return std::string();
    }
    return it->second.IsHolding<TfToken>()       ? it->second.UncheckedGet<TfToken>().GetString()
           : it->second.IsHolding<std::string>() ? it->second.UncheckedGet<std::string>()
                                                 : std::string();
}

TfToken BoundAov(HdRenderPassAovBinding const &b)
{
    const auto text = [&](const char *key) { return BindingText(b, key); };
    const std::string name = text("sourceName"), type = text("sourceType");
    if (name.empty()) {
        return b.aovName;
    }
    const TfToken source = type == "lpe"       ? HdAovTokensMakeLpe(TfToken(name))
                           : type == "primvar" ? HdAovTokensMakePrimvar(TfToken(name))
                                               : TfToken(name);
    if (source == TfToken("lpe:C.*[LO]")) {
        return HdAovTokens->color;
    }
    // Karma's own names for what Hydra calls primId and its N-less raw vars.
    return source == TfToken("ray:primid") ? HdAovTokens->primId : source;
}

} // namespace

HdVkRenderPass::HdVkRenderPass(HdRenderIndex *index, HdRprimCollection const &collection,
                               vkr::Scene &scene, vkr::Renderer *renderer)
    : HdRenderPass(index, collection)
    , _scene(scene)
    , _renderer(renderer)
{
}

// Not while levels of detail build either: the app keeps drawing until
// they arrive (usdrecord's image has them).
bool HdVkRenderPass::IsConverged() const
{
    return !_renderer || (_renderer->IsConverged() && !_scene.LodsPending());
}

// ponytail: renders the whole buffer and ignores the framing's data window
// and the collection. Fine for usdview; revisit for Houdini (step 7).
void HdVkRenderPass::_Execute(HdRenderPassStateSharedPtr const &state,
                              TfTokenVector const &renderTags)
{
    if (!_renderer) {
        return;
    }

    // Only the pass's render tags (purposes) are drawn; no tags: all.
    uint32_t tagMask = renderTags.empty() ? ~0u : 0u;
    for (const TfToken &tag : renderTags) {
        tagMask |= HdVkRenderTagBit(tag);
    }
    if (_scene.SetTagMask(tagMask)) {
        _scene.Commit(true);  // the frame's second: its motion stays
    }

    vkr::Renderer::Frame frame;
    HdRenderBuffer *color = nullptr, *depth = nullptr, *primId = nullptr, *instanceId = nullptr;
    // Extra AOVs (vkr::Aovs) by their Hydra names (HdVkExtraAovs).
    // ponytail: one buffer per kind; of two names for the same (normal and
    // ray:hitN) bound at once, the last one gets it.
    HdRenderBuffer *extra[vkr::Aovs::kCount] = {};
    // husk's var name of the extra AOVs it binds ("driver:parameters:aov:husk:name").
    std::string huskName[vkr::Aovs::kCount];
    bool huskPrimId = false;
    for (const HdRenderPassAovBinding &b : state->GetAovBindings()) {
        const TfToken aovName = BoundAov(b);
        for (const HdVkExtraAov &aov : HdVkExtraAovs()) {
            if (aovName == aov.name) {
                extra[aov.kind] = b.renderBuffer;
                huskName[aov.kind] = BindingText(b, "driver:parameters:aov:husk:name");
            }
        }
        if (aovName == HdAovTokens->color) {
            color = b.renderBuffer;
            if (b.clearValue.IsHolding<GfVec4f>()) {
                const GfVec4f &c = b.clearValue.UncheckedGet<GfVec4f>();
                std::copy(c.data(), c.data() + 4, frame.clearColor);
            }
        } else if (aovName == HdAovTokens->depth) {
            depth = b.renderBuffer;
            if (b.clearValue.IsHolding<float>()) {
                frame.clearDepth = b.clearValue.UncheckedGet<float>();
            }
        } else if (aovName == HdAovTokens->primId) {
            primId = b.renderBuffer;
            huskPrimId = !BindingText(b, "driver:parameters:aov:husk:name").empty();
        } else if (aovName == HdAovTokens->instanceId) {
            instanceId = b.renderBuffer;
        }
    }
    // The frame has the colour's size, or the first buffer's (usdview binds
    // only the AOV it shows). Formats are the ones GetDefaultAovDescriptor
    // asks for; a buffer with another format or size is left alone rather
    // than overrun.
    for (const HdRenderPassAovBinding &b : state->GetAovBindings()) {
        if (HdRenderBuffer *sized = color ? color : b.renderBuffer) {
            frame.width = sized->GetWidth();
            frame.height = sized->GetHeight();
            break;
        }
    }
    if (frame.width == 0 || frame.height == 0) {
        return;
    }
    auto usable = [&](HdRenderBuffer *b, HdFormat format) {
        return b && b->GetFormat() == format && b->GetWidth() == frame.width &&
               b->GetHeight() == frame.height ? b : nullptr;
    };
    // The buffers the app mapped (read) since the last frame, new ones too:
    // read back with the frame, in one copy; the rest only if mapped.
    std::unordered_set<HdRenderBuffer *> read;
    for (const HdRenderPassAovBinding &b : state->GetAovBindings()) {
        if (b.renderBuffer && static_cast<HdVkRenderBuffer *>(b.renderBuffer)->TakeMapped()) {
            read.insert(b.renderBuffer);
        }
    }
    // HDVK_DEBUG_AOVS=1: print what the app binds, once per set of bindings,
    // and every 60 frames how many of them it mapped (read).
    if (static const bool debug = TfGetenvBool("HDVK_DEBUG_AOVS", false); debug) {
        std::string list;
        for (const HdRenderPassAovBinding &b : state->GetAovBindings()) {
            list += TfStringPrintf(" %s:%d:%ux%u", b.aovName.GetText(), b.renderBuffer ? int(b.renderBuffer->GetFormat()) : -1,
                                   b.renderBuffer ? b.renderBuffer->GetWidth() : 0,
                                   b.renderBuffer ? b.renderBuffer->GetHeight() : 0);
        }
        static std::string last;
        if (list != last) {
            last = list;
            std::fprintf(stderr, "Eldr AOV bindings:%s\n", list.c_str());
            std::fflush(stderr);
        }
        static std::map<std::string, int> maps;
        static int frames = 0;
        for (const HdRenderPassAovBinding &b : state->GetAovBindings()) {
            maps[b.aovName.GetString()] += read.count(b.renderBuffer) > 0;
        }
        if (++frames % 60 == 0) {
            std::string counts;
            for (auto &[name, n] : maps) {
                counts += TfStringPrintf(" %s %d", name.c_str(), n);
                n = 0;
            }
            std::fprintf(stderr, "Eldr AOVs mapped in the last 60 frames:%s\n", counts.c_str());
            std::fflush(stderr);
        }
    }
    // Apps may bind the colour in another format whatever the descriptor
    // says (husk: Float32Vec4; Houdini's viewport may differ): rendered as
    // half and converted after the frame.
    HdRenderBuffer *colorFloat32 = nullptr;
    if (color && color->GetFormat() != HdFormatFloat16Vec4) {
        const HdFormat f = color->GetFormat();
        const HdFormat component = HdGetComponentFormat(f);
        const size_t n = HdGetComponentCount(f);
        const bool convertible = (component == HdFormatFloat32 || component == HdFormatFloat16 ||
                                  component == HdFormatUNorm8) && (n == 3 || n == 4);
        colorFloat32 = convertible && color->GetWidth() == frame.width && color->GetHeight() == frame.height ? color
                                                                                                           : nullptr;
        static bool warned = false;
        if (!colorFloat32 && !warned) {
            warned = true;
            TF_WARN("Eldr: the colour AOV's format %d (%ux%u) can't be written; the frame is %ux%u", int(f),
                    color->GetWidth(), color->GetHeight(), frame.width, frame.height);
        }
    }
    color = usable(color, HdFormatFloat16Vec4);
    depth = usable(depth, HdFormatFloat32);
    // husk's id var is a float in the image (it reads an Int32 buffer as a
    // normalised int, which is 0): that buffer becomes Float32 below.
    HdRenderBuffer *const idForHusk = huskPrimId && primId && primId->GetWidth() == frame.width &&
                                      primId->GetHeight() == frame.height ? primId : nullptr;
    primId = usable(primId, HdFormatInt32);
    instanceId = usable(instanceId, HdFormatInt32);

    HdRenderDelegate *delegate = GetRenderIndex()->GetRenderDelegate();
    // Any float format will do: it gets that many components.
    uint8_t extraComponents[vkr::Aovs::kCount] = {};
    for (int k = 0; k < vkr::Aovs::kCount; ++k) {
        const HdFormat format = extra[k] ? extra[k]->GetFormat() : HdFormatInvalid;
        extra[k] = HdGetComponentFormat(format) == HdFormatFloat32 ? usable(extra[k], format) : nullptr;
        extraComponents[k] = extra[k] ? uint8_t(HdGetComponentCount(format)) : 0;
    }
    // eldr:* attributes of the active UsdRenderSettings prim are render
    // settings of the scene: applied when the prim's value changes (so a
    // later edit in usdview's menu stands until the prim changes again).
    // Typed like the setting's default; unknown names are ignored.
    bool batch = false;  // husk sets no enableInteractive, but its settings prim says who renders
    if (const HdRenderSettings *prim = static_cast<HdVkRenderDelegate *>(delegate)->ActiveRenderSettings()) {
        for (const auto &[key, value] : prim->GetNamespacedSettings()) {
            batch |= key.compare(0, 5, "husk:") == 0;  // HuskRenderSettingsAPI
            if (key.compare(0, 5, "eldr:") != 0) {
                continue;
            }
            const TfToken token(key);
            const VtValue current = delegate->GetRenderSetting(token);
            VtValue &applied = _appliedSettings[token];
            if (current.IsEmpty() || applied == value) {
                continue;
            }
            applied = value;
            const VtValue typed = VtValue::CastToTypeOf(value, current);
            if (!typed.IsEmpty()) {
                delegate->SetRenderSetting(token, typed);
            }
        }
    }
    frame.showEnvironment = delegate->GetRenderSetting<bool>(
        HdRenderSettingsTokens->domeLightCameraVisibility, true);
    // Each setting over the renderer's default (renderDelegate.cpp lists
    // them); counts are ints in Hydra, at least 1.
    const auto &t = *HdVkRenderSettingsTokens;
    const auto &t2 = *HdVkRenderSettingsTokens2;
    for (auto [token, value] : {std::pair{t.postProcess, &frame.postProcess}, {t.taa, &frame.taa},
                                {t.ambientOcclusion, &frame.ambientOcclusion}, {t.ssr, &frame.ssr},
                                {t.autoExposure, &frame.post.autoExposure}, {t.shadows, &frame.shadows},
                                {t.localShadows, &frame.localShadows}, {t.softShadows, &frame.softShadows},
                                {t.depthOfField, &frame.depthOfField}, {t.motionBlur, &frame.motionBlur},
                                {t2.motionBlurTimeSamples, &frame.motionBlurTimeSamples},
                                {t2.motionBlurResidual, &frame.motionBlurResidual},
                                {t.depthPrepass, &frame.depthPrepass}, {t.frustumCulling, &frame.frustumCulling},
                                {t.localLights, &frame.localLights}, {t.clusteredLights, &frame.clusteredLights},
                                {t.imageBasedLighting, &frame.imageBasedLighting}, {t.normalMaps, &frame.normalMaps},
                                {t.materialGraphs, &frame.materialGraphs}, {t.objectMotion, &frame.objectMotion},
                                {t.translucency, &frame.translucency},
                                {t.proceduralTextures, &frame.proceduralTextures},
                                {t.lensDistortion, &frame.lensDistortion},
                                {t.blendShapeMotion, &frame.blendShapeMotion},
                                {t.extraAovs, &frame.extraAovs}, {t.renderProducts, &frame.renderProducts}, {t.shareFrame, &frame.shareFrame},
                                {t.specularWorkflow, &frame.specularWorkflow}, {t.clearcoat, &frame.clearcoat},
                                {t.sheen, &frame.sheen}, {t.thinFilm, &frame.thinFilm},
                                {t.anisotropicSpecular, &frame.anisotropicSpecular}, {t.toon, &frame.toon},
                                {t.hairShading, &frame.hairShading}, {t.texturedLobes, &frame.texturedLobes},
                                {t.texturedTransmission, &frame.texturedTransmission}, {t.rtShadows, &frame.rtShadows},
                                {t.rtReflections, &frame.rtReflections}, {t.gi, &frame.gi}, {t.ssgi, &frame.ssgi},
                                {t.emissiveLights, &frame.emissiveLights}, {t.oit, &frame.oit},
                                {t.refraction, &frame.refraction}, {t.holdoutAlpha, &frame.holdoutAlpha}, {t.multiRefraction, &frame.multiRefraction}, {t.refractionBackFaces, &frame.refractionBackFaces},
                                {t.translucentShadows, &frame.translucentShadows},
                                {t.subsurface, &frame.subsurface}, {t.holdouts, &frame.holdouts},
                                {t.softSunShadows, &frame.softSunShadows}, {t.fog, &frame.fog},
                                {t.fogZUp, &frame.fogZUp}, {t.reflectionProbe, &frame.reflectionProbe},
                                {t.planarReflections, &frame.planarReflections},
                                {t.volumes, &frame.volumes},
                                {t.volumeUnderTransparency, &frame.volume.underTransparency},
                                {t.volumeShadows, &frame.volume.shadows},
                                {t.displacement, &frame.displacement},
                                {t.lod, &frame.lod}, {t.filmAccumulation, &frame.filmAccumulation},
                                {t.finalQuality, &frame.finalQuality},
                                {t.sampleDimensions, &frame.sampleDimensions},
                                {t.giPreconverge, &frame.giPreconverge}, {t.fogFilmJitter, &frame.fogFilmJitter},
                                {t.pipelineCache, &frame.pipelineCache},
                                {t.pipelinePrewarm, &frame.pipelinePrewarm},
                                {t2.ssgiThickness, &frame.ssgiThickness},
                                {t2.ssgiBitmask, &frame.ssgiBitmask}, {t2.rtDisplacement, &frame.rtDisplacement},
                                {t2.ssrDenoise, &frame.ssrDenoise}, {t2.shadowJitter, &frame.shadowJitter},
                                {t2.rtTranslucentShadows, &frame.rtTranslucentShadows},
                                {t2.dofJitter, &frame.dofJitter},
                                {t2.authoredProbes, &frame.authoredProbes},
                                {t2.probeNormalize, &frame.probeNormalize},
                                {t2.domeSun, &frame.domeSun},
                                {t2.fogGiAmbient, &frame.fogGiAmbient},
                                {t2.lightTextures, &frame.lightTextures}, {t2.productLinear, &frame.linearBeauty},
                                {t2.displacementGraphs, &frame.displacementGraphs},
                                {t2.lodFade, &frame.lodFade},
                                {t2.splitDomeDirect, &frame.splitDomeDirect},
                                {t2.cryptomatteSoft, &frame.cryptomatteSoft},
                                {t2.deformationBlur, &frame.deformationBlur},
                                {t2.dofResidualBlur, &frame.dofResidualBlur},
                                {t2.productResolution, &frame.productResolution},
                                {t2.localVolumeShadows, &frame.localVolumeShadows},
                                {t2.lightShapesVisible, &frame.lightShapesVisible},
                                {t2.screenBackface, &frame.screenBackface},
                                {t2.ssrHitFilter, &frame.ssrHitFilter},
                                {t2.ssgiTileOrder, &frame.ssgiTileOrder}}) {
        *value = delegate->GetRenderSetting<bool>(token, *value);
    }
    // Render products at their own framing (eldr:productResolution): the
    // first raster product's resolution, pixel aspect ratio and data window
    // when they aren't the buffers' size (usdrecord sizes them from -w and
    // the camera; husk from the product). The app's colour, depth and ids
    // then get the frame resampled (nearest), the extra AOVs and Cryptomatte too.
    HdVkRenderProducts::Framing framing;
    // husk --res changes its buffers, not the product: the buffers' size is
    // then the resolution (the camera conforms to its aspect by the product's
    // policy), as in Karma. Elsewhere (usdrecord -w) the product's own stands.
    const GfVec2i bufferSize(int(frame.width), int(frame.height));
    const bool hasFraming = frame.renderProducts && HdVkRenderProducts::GetFraming(
                                                        static_cast<HdVkRenderDelegate *>(delegate)->ActiveRenderSettings(),
                                                        &framing, batch ? &bufferSize : nullptr);
    const bool productSized = hasFraming && frame.productResolution &&
                              (framing.Width() != frame.width || framing.Height() != frame.height);
    const uint32_t appWidth = frame.width, appHeight = frame.height;
    if (static const bool debug = TfGetenvBool("HDVK_DEBUG_AOVS", false); debug && productSized) {
        std::fprintf(stderr, "Eldr: the product's framing %ux%u, the buffers' %ux%u\n", framing.Width(), framing.Height(),
                     appWidth, appHeight);
        std::fflush(stderr);
    }
    _products.SetFraming(hasFraming ? &framing : nullptr);
    if (productSized) {
        frame.width = framing.Width();
        frame.height = framing.Height();
    }
    // The app's pixel p (row major, appWidth wide) in the frame: the same
    // for a frame of the buffers' size, else the nearest of the product's.
    auto sourcePixel = [&](size_t p) {
        const size_t x = p % appWidth, y = p / appWidth;
        return productSized ? (y * frame.height / appHeight) * frame.width + x * frame.width / appWidth : p;
    };
    for (auto [token, value] : {std::pair{t.aoRadius, &frame.aoRadius}, {t.cullPixels, &frame.cullPixels},
                                {t.exposureCompensation, &frame.post.exposureCompensation},
                                {t.bloomIntensity, &frame.post.bloomIntensity},
                                {t.giHysteresis, &frame.giHysteresis}, {t.ssgiRadius, &frame.ssgiRadius},
                                {t.emissiveLightThreshold, &frame.emissiveLightThreshold},
                                {t.oitDepthWeight, &frame.oitDepthWeight},
                                {t.fogDensity, &frame.fogDensity}, {t.fogHeight, &frame.fogHeight},
                                {t.fogHeightFalloff, &frame.fogHeightFalloff},
                                {t.fogAnisotropy, &frame.fogAnisotropy}, {t.fogMaxDistance, &frame.fogMaxDistance},
                                {t.planarScale, &frame.planarScale},
                                {t.volumeStepSize, &frame.volume.stepSize}, {t.volumeDensity, &frame.volume.density},
                                {t.volumeEmission, &frame.volume.emission},
                                {t.volumeAnisotropy, &frame.volume.anisotropy},
                                {t.displacementScale, &frame.displacementScale}, {t.lodBias, &frame.lodBias},
                                {t2.domeSunThreshold, &frame.domeSunThreshold},
                                {t.filmFilterRadius, &frame.filmFilterRadius}, {t2.renderScale, &frame.renderScale},
                                {t.adaptiveThreshold, &frame.adaptiveThreshold},
                                {t2.adaptiveFinalThreshold, &frame.adaptiveFinalThreshold},
                                {t2.adaptivePixelFraction, &frame.adaptivePixelFraction},
                                {t2.directIntensity, &frame.directIntensity},
                                {t2.indirectIntensity, &frame.indirectIntensity},
                                {t2.ssgiBitmaskThickness, &frame.ssgiBitmaskThickness}, {t2.directClamp, &frame.directClamp}, {t2.indirectClamp, &frame.indirectClamp},
                                {t2.screenBackfaceScale, &frame.screenBackfaceScale}}) {
        *value = delegate->GetRenderSetting<float>(token, *value);
    }
    // Colours and positions: "x y z" (usdview edits strings) or a
    // GfVec3f/GfVec3d; false (and `xyz` as it was) if neither.
    auto readVec3 = [&](TfToken const &token, float xyz[3]) {
        const VtValue v = delegate->GetRenderSetting(token);
        if (v.IsHolding<std::string>()) {
            float c[3];
            if (std::sscanf(v.UncheckedGet<std::string>().c_str(), "%f %f %f", &c[0], &c[1], &c[2]) == 3) {
                std::copy(c, c + 3, xyz);
                return true;
            }
        } else if (v.CanCast<GfVec3f>()) {
            const GfVec3f c = VtValue::Cast<GfVec3f>(v).UncheckedGet<GfVec3f>();
            std::copy(c.data(), c.data() + 3, xyz);
            return true;
        }
        return false;
    };
    readVec3(t.fogColor, frame.fogColor);
    frame.probePosition[3] = readVec3(t.probePosition, frame.probePosition) ? 1.0f : 0.0f;
    for (auto [token, value] : {std::pair{t.taaSamples, &frame.taaSamples},
                                {t.shadowResolution, &frame.shadowResolution},
                                {t.shadowCascades, &frame.shadowCascades},
                                {t.localShadowResolution, &frame.localShadowResolution},
                                {t.softShadowSamples, &frame.softShadowSamples}, {t.aoSlices, &frame.aoSlices},
                                {t.ssrSteps, &frame.ssrSteps}, {t.anisotropy, &frame.anisotropy},
                                {t.rtShadowSamples, &frame.rtShadowSamples},
                                {t.rtReflectionSamples, &frame.rtReflectionSamples},
                                {t.giProbes, &frame.giProbes}, {t.giRays, &frame.giRays},
                                {t.ssgiSteps, &frame.ssgiSteps}, {t.ssgiResolution, &frame.ssgiResolution},
                                {t.refractionSamples, &frame.refractionSamples}, {t.refractionLayers, &frame.refractionLayers},
                                {t.translucentShadowDivisor, &frame.translucentShadowDivisor},
                                {t.subsurfaceSamples, &frame.subsurfaceSamples},
                                {t.fogGridSize, &frame.fogGridSize}, {t.probeResolution, &frame.probeResolution},
                                {t.volumeMaxSteps, &frame.volume.maxSteps},
                                {t.volumeShadowSteps, &frame.volume.shadowSteps},
                                {t.volumeResolution, &frame.volume.resolution},
                                {t2.planarCount, &frame.planarCount},
                                {t2.rtRefractionBounces, &frame.rtRefractionBounces},
                                {t.filmSamples, &frame.filmSamples},
                                {t.interactiveSamples, &frame.interactiveSamples},
                                {t.adaptiveMinSamples, &frame.adaptiveMinSamples}}) {
        *value = uint32_t(std::max(delegate->GetRenderSetting<int>(token, int(*value)), 1));
    }
    // 0 allowed: off.
    frame.rtRefraction =
        uint32_t(std::clamp(delegate->GetRenderSetting<int>(t2.rtRefraction, int(frame.rtRefraction)), 0, 2));
    // 0 allowed: no skinned instance blocks rays.
    frame.rtSkinnedInstances =
        uint32_t(std::max(delegate->GetRenderSetting<int>(t.rtSkinnedInstances, int(frame.rtSkinnedInstances)), 0));
    using FinalShadows = vkr::Renderer::Frame::FinalShadows;
    const std::string finalShadows = delegate->GetRenderSetting<std::string>(t2.finalShadows, "maps");
    frame.finalShadows = finalShadows == "raytraced" ? FinalShadows::Raytraced
                       : finalShadows == "auto"      ? FinalShadows::Auto
                                                     : FinalShadows::Maps;
    using Tonemapper = vkr::PostProcess::Settings::Tonemapper;
    const std::string tonemapper = delegate->GetRenderSetting<std::string>(t.tonemapper, "aces");
    frame.post.tonemapper = tonemapper == "agx"       ? Tonemapper::Agx
                          : tonemapper == "agxPunchy" ? Tonemapper::AgxPunchy
                          : tonemapper == "none"      ? Tonemapper::None
                                                      : Tonemapper::Aces;
    // Lens distortion (HdCamera's lensDistortion:*, which USD cameras carry
    // as custom attributes of those names) warps the finished frame; the
    // scene renders that much wider for the warp to sample.
    GfMatrix4d projection = state->GetProjectionMatrix();
    const HdCamera *camera = state->GetCamera();
    if (productSized) {
        projection = ProductProjection(camera ? camera->ComputeProjectionMatrix() : projection, framing);
    }
    if (camera && frame.lensDistortion && projection[0][0] > 0.0) {
        vkr::LensDistortion::Params &d = frame.distortion;
        const bool perspective = camera->GetProjection() == HdCamera::Perspective;
        d.fisheye = perspective && camera->GetLensDistortionType() == HdCameraTokens->fisheye;
        d.k1 = camera->GetLensDistortionK1();
        d.k2 = camera->GetLensDistortionK2();
        if (const auto *vkCamera = dynamic_cast<const HdVkCamera *>(camera); vkCamera && !d.fisheye) {
            const float *t = vkCamera->GetLensDistortionOpenCvTerms();
            d.k3 = t[0], d.k4 = t[1], d.k5 = t[2], d.k6 = t[3], d.p1 = t[4], d.p2 = t[5];
        }
        std::copy(camera->GetLensDistortionCenter().data(), camera->GetLensDistortionCenter().data() + 2, d.center);
        d.anaSq = camera->GetLensDistortionAnaSq() > 0.0f ? camera->GetLensDistortionAnaSq() : 1.0f;
        std::copy(camera->GetLensDistortionAsym().data(), camera->GetLensDistortionAsym().data() + 2, d.asym);
        d.scale = camera->GetLensDistortionScale() > 0.0f ? camera->GetLensDistortionScale() : 1.0f;
        d.tanHalfWidth = float(1.0 / projection[0][0]);
        if (d.Active()) {
            d.overscan = vkr::LensDistortion::Overscan(d, float(frame.width) / float(frame.height));
            GfMatrix4d widen(1.0);
            widen[0][0] = widen[1][1] = 1.0 / d.overscan;
            projection = projection * widen;
        }
    }
    const GfMatrix4d view = state->GetWorldToViewMatrix();
    const GfMatrix4f viewF(view);
    std::copy(viewF.data(), viewF.data() + 16, frame.view);
    const GfMatrix4d viewProj = view * projection;
    const GfMatrix4f viewProjF(viewProj), invViewProjF(viewProj.GetInverse());
    std::copy(viewProjF.data(), viewProjF.data() + 16, frame.viewProj);
    std::copy(invViewProjF.data(), invViewProjF.data() + 16, frame.invViewProj);

    // Final quality: asked for, or an offline renderer's convention (husk,
    // hdPrman: enableInteractive false). usdrecord sets neither, but its
    // first frame has no history, so its film starts with the first sample.
    const bool offline = batch || !delegate->GetRenderSetting<bool>(HdRenderSettingsTokens->enableInteractive, true);
    frame.finalQuality = frame.finalQuality || offline;
    // An offline frame's exposure is deterministic (like Karma's: none)
    // unless the render settings prim asks for eldr:autoExposure.
    if (offline && !_appliedSettings.count(t.autoExposure)) {
        frame.post.autoExposure = false;
    }
    // Any edit to the scene or the settings starts the film over.
    frame.sceneVersion = (uint64_t(GetRenderIndex()->GetChangeTracker().GetSceneStateVersion()) << 32) |
                         delegate->GetRenderSettingsVersion();
    frame.toonBands = uint32_t(std::max(delegate->GetRenderSetting<int>(t.toonBands, int(frame.toonBands)), 0));
    // Either may change the light table: commit once.
    const bool emissive = _scene.SetEmissiveLights(frame.emissiveLights, frame.emissiveLightThreshold);
    if (_scene.SetDomeSun(frame.domeSun, frame.domeSunThreshold) || emissive) {
        _scene.Commit(true);  // the frame's second: its motion stays
    }
    // The scene camera's lens; usdview's free camera has none.
    if (camera) {
        frame.shutter = float(camera->GetShutterClose() - camera->GetShutterOpen());
        frame.shutterOpen = float(camera->GetShutterOpen());
        if (camera->GetProjection() == HdCamera::Perspective && projection[0][0] > 0.0) {
            frame.focalLength = camera->GetFocalLength();
            frame.fStop = camera->GetFStop();
            frame.focusDistance = camera->GetFocusDistance();
            // The aperture the conformed projection shows (2 f / P[0][0]).
            frame.sensorWidth = float(2.0 * frame.focalLength / projection[0][0]);
        }
    }
    const GfMatrix4d invProjection = projection.GetInverse();
    frame.depthToDistance[0] = float(invProjection[2][2]);
    frame.depthToDistance[1] = float(invProjection[3][2]);
    frame.depthToDistance[2] = float(invProjection[2][3]);
    frame.depthToDistance[3] = float(invProjection[3][3]);
    const GfVec3d eye = view.GetInverse().ExtractTranslation();
    frame.cameraPos[0] = float(eye[0]);
    frame.cameraPos[1] = float(eye[1]);
    frame.cameraPos[2] = float(eye[2]);
    // Sampled motion: rprims sample over this camera's shutter (from the next
    // sync on; a first frame's take the open camera that synced), and the
    // camera's own motion comes from its samples.
    float shutterOpen, shutterClose;
    static_cast<HdVkRenderDelegate *>(delegate)->SetActiveCamera(camera ? camera->GetId() : SdfPath());
    frame.sampledMotion = frame.shutter > 0.0f &&
                          static_cast<HdVkRenderDelegate *>(delegate)->SampledMotionShutter(&shutterOpen, &shutterClose);
    std::copy(viewProjF.data(), viewProjF.data() + 16, frame.prevViewProj);
    std::copy(frame.cameraPos, frame.cameraPos + 3, frame.prevCameraPos);
    GfMatrix4d prevCamera;
    if (const auto *vkCamera = dynamic_cast<const HdVkCamera *>(camera);
        frame.sampledMotion && vkCamera && vkCamera->GetPrevTransform(&prevCamera)) {
        const GfMatrix4f prevViewProjF(prevCamera.GetInverse() * projection);
        std::copy(prevViewProjF.data(), prevViewProjF.data() + 16, frame.prevViewProj);
        const GfVec3d prevEye = prevCamera.ExtractTranslation();
        for (int i = 0; i < 3; ++i) {
            frame.prevCameraPos[i] = float(prevEye[i]);
        }
    }

    // The four come back with the frame if the app read them last frame
    // (`read`), else only once it maps them: Houdini's viewport binds the
    // ids for picking but needn't read them each frame. GPU sharing: colour
    // and depth also go to the app's OpenGL as textures.
    auto *vkDelegate = static_cast<HdVkRenderDelegate *>(delegate);
    HdVkGLInterop *interop = frame.shareFrame && color && !productSized ? vkDelegate->GLInterop() : nullptr;
    const bool lazy = interop != nullptr;
    std::vector<int32_t> huskIds;  // the frame's; the float buffer gets them resampled
    if (idForHusk &&
        (idForHusk->GetFormat() == HdFormatFloat32 ||
         static_cast<HdVkRenderBuffer *>(idForHusk)->Allocate(GfVec3i(frame.width, frame.height, 1), HdFormatFloat32, false))) {
        huskIds.resize(size_t(frame.width) * frame.height);
        primId = nullptr;
    }
    HdVkRenderBuffer *const buffers[4] = {static_cast<HdVkRenderBuffer *>(color), static_cast<HdVkRenderBuffer *>(depth),
                                          static_cast<HdVkRenderBuffer *>(primId),
                                          static_cast<HdVkRenderBuffer *>(instanceId)};
    vkr::Renderer::Targets out;
    // At a product's framing the frame's targets are read here and
    // resampled into the buffers after.
    void *mapped[4] = {};
    std::vector<uint8_t> productTargets[4];
    constexpr size_t kTargetBytes[4] = {8, 4, 4, 4};  // RGBA16F, float, int, int
    for (int i = 0; i < 4; ++i) {
        mapped[i] = buffers[i] && (productSized || read.count(buffers[i])) ? buffers[i]->Map() : nullptr;
        if (productSized && mapped[i]) {
            productTargets[i].resize(size_t(frame.width) * frame.height * kTargetBytes[i]);
        }
    }
    auto target = [&](int i) { return productSized && mapped[i] ? productTargets[i].data() : mapped[i]; };
    out.color = target(0);
    out.depth = static_cast<float *>(target(1));
    out.primId = static_cast<int32_t *>(target(2));
    out.instanceId = static_cast<int32_t *>(target(3));
    std::vector<GfHalf> colorHalf;
    if (colorFloat32) {
        colorHalf.resize(size_t(frame.width) * frame.height * 4);
        out.color = colorHalf.data();
    }
    // While the camera moves, extra AOVs the app didn't read last frame
    // aren't rendered: Houdini's viewport binds normal and primvars:st beside
    // the colour but reads them only when it shows them, and they cost a
    // G-buffer pass and a readback. One mapped then is a frame old once,
    // and rendered from the next frame on; still frames render them all.
    const bool moving = std::memcmp(frame.viewProj, _lastViewProj, sizeof(_lastViewProj)) != 0;
    std::copy(frame.viewProj, frame.viewProj + 16, _lastViewProj);
    for (HdRenderBuffer *&b : extra) {
        b = b && moving && !frame.finalQuality && !productSized && !read.count(b) ? nullptr : b;
    }
    // At a product's framing they are rendered into frame-sized copies and resampled after.
    float *extraDst[vkr::Aovs::kCount] = {};
    std::vector<float> extraFrame[vkr::Aovs::kCount];
    for (int k = 0; k < vkr::Aovs::kCount; ++k) {
        extraDst[k] = extra[k] ? static_cast<float *>(extra[k]->Map()) : nullptr;
        out.aovs[k] = extraDst[k];
        out.aovComponents[k] = extraComponents[k];
        if (extraDst[k] && productSized) {
            extraFrame[k].assign(size_t(frame.width) * frame.height * extraComponents[k], 0.0f);
            out.aovs[k] = extraFrame[k].data();
        }
    }
    // UsdRender products (renderProducts.h) of the active render settings.
    _products.Prepare(frame.renderProducts ? vkDelegate->ActiveRenderSettings() : nullptr, frame.width, frame.height,
                      &out);
    // husk writes its colour AOV itself: it gets the linear beauty too.
    const bool huskLinear = !colorHalf.empty() && frame.linearBeauty;
    frame.linearColor = frame.linearBeauty && (_products.HasBeauty() || huskLinear);

    // Cryptomatte: per primId the ids of its rprim's path and material path,
    // and their manifests (GetRenderStats).
    // ponytail: rebuilt every frame these AOVs are bound; instances share
    // their prototype's id.
    std::vector<uint32_t> cryptomatte;
    std::string objects, materials;
    if (out.aovs[vkr::Aovs::kCryptoObject] || out.aovs[vkr::Aovs::kCryptoMaterial]) {
        HdRenderIndex *index = GetRenderIndex();
        std::unordered_set<SdfPath, SdfPath::Hash> seen;  // materials in the manifest
        auto entry = [](const std::string &list, const SdfPath &name, uint32_t id) {
            return TfStringPrintf("%s\"%s\":\"%08x\"", list.empty() ? "" : ",", name.GetText(), id);
        };
        for (const SdfPath &path : index->GetRprimIds()) {
            const HdRprim *rprim = index->GetRprim(path);
            if (!rprim || rprim->GetPrimId() < 0) {
                continue;
            }
            const size_t id = size_t(rprim->GetPrimId());
            cryptomatte.resize(std::max(cryptomatte.size(), 2 * id + 2), 0u);
            const SdfPath &material = rprim->GetMaterialId();
            cryptomatte[2 * id] = vkr::CryptomatteHash(path.GetString());
            cryptomatte[2 * id + 1] = material.IsEmpty() ? 0u : vkr::CryptomatteHash(material.GetString());
            objects += entry(objects, path, cryptomatte[2 * id]);
            if (!material.IsEmpty() && seen.insert(material).second) {
                materials += entry(materials, material, cryptomatte[2 * id + 1]);
            }
        }
        frame.cryptomatte = cryptomatte.data();
        frame.cryptomatteIds = uint32_t(cryptomatte.size() / 2);
        objects = "{" + objects + "}";
        materials = "{" + materials + "}";
        static_cast<HdVkRenderDelegate *>(delegate)->SetCryptomatteManifests(objects, materials);
    }

    // husk's Cryptomatte vars: Eldr renders 6 x (id, coverage) and splits them
    // into the layers <name>00..02 husk writes (HdVkRenderBuffer::SetCrypto),
    // and copies the first components to the var's own buffer after.
    float *cryptoPrimary[vkr::Aovs::kCount] = {}, *cryptoPlane[vkr::Aovs::kCount] = {};
    std::vector<float> cryptoFrame[vkr::Aovs::kCount];
#ifdef HDVK_HOUDINI
    for (uint32_t k : {vkr::Aovs::kCryptoObject, vkr::Aovs::kCryptoMaterial}) {
        if (!extra[k] || huskName[k].empty() || !out.aovs[k]) {
            continue;
        }
        char hash[16];
        std::snprintf(hash, sizeof(hash), "%08x", vkr::CryptomatteHash(huskName[k]));
        const std::string prefix = "cryptomatte/" + std::string(hash, 7) + "/";
        cryptoPrimary[k] = extraDst[k];
        cryptoPlane[k] = static_cast<HdVkRenderBuffer *>(extra[k])->SetCrypto(
            huskName[k], {{prefix + "name", huskName[k]},
                          {prefix + "hash", "MurmurHash3_32"},
                          {prefix + "conversion", "uint32_to_float32"},
                          {prefix + "manifest", k == vkr::Aovs::kCryptoObject ? objects : materials}});
        cryptoFrame[k].assign(size_t(frame.width) * frame.height * 12, 0.0f);
        out.aovs[k] = cryptoFrame[k].data();
        out.aovComponents[k] = 12;
    }
#endif
    if (!huskIds.empty()) {
        out.primId = huskIds.data();
    }

    bool shared = false;
    try {
        _renderer->Render(_scene, frame, out);
        shared = interop && interop->Present(*_renderer);
        if (interop && !shared) {
            vkDelegate->DisableGLInterop();
        }
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk render failed: %s", e.what());
    }
    vkDelegate->SetFrameShared(shared);
    if (huskLinear) {
        try {  // the linear image over the display one Render read
            vkr::Renderer::Targets linear;
            linear.linearColor = colorHalf.data();
            _renderer->Read(linear);
        } catch (const std::exception &e) {
            TF_RUNTIME_ERROR("HdVk linear readback failed: %s", e.what());
        }
    }
    if (!colorHalf.empty()) {
        const HdFormat f = colorFloat32->GetFormat();
        const HdFormat component = HdGetComponentFormat(f);
        const size_t n = HdGetComponentCount(f), pixels = size_t(appWidth) * appHeight;
        void *dst = colorFloat32->Map();
        for (size_t p = 0; p < pixels; ++p) {
            const size_t source = sourcePixel(p);
            for (size_t c = 0; c < n; ++c) {
                const float v = colorHalf[source * 4 + c];
                if (component == HdFormatFloat32) {
                    static_cast<float *>(dst)[p * n + c] = v;
                } else if (component == HdFormatFloat16) {
                    static_cast<GfHalf *>(dst)[p * n + c] = GfHalf(v);
                } else {
                    static_cast<uint8_t *>(dst)[p * n + c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
        colorFloat32->Unmap();
    }
#ifdef HDVK_HOUDINI
    for (int k = 0; k < vkr::Aovs::kCount; ++k) {
        if (!cryptoPlane[k]) {
            continue;
        }
        // Layer L holds ranks 2L and 2L+1: floats 4L..4L+3 of the pixel's 12.
        const size_t pixels = size_t(appWidth) * appHeight, n = std::min<size_t>(extraComponents[k], 4);
        for (size_t p = 0; p < pixels; ++p) {
            const float *from = cryptoFrame[k].data() + sourcePixel(p) * 12;
            for (size_t layer = 0; layer < 3; ++layer) {
                std::copy(from + layer * 4, from + layer * 4 + 4, cryptoPlane[k] + (layer * pixels + p) * 4);
            }
            std::copy(from, from + n, cryptoPrimary[k] + p * extraComponents[k]);
        }
    }
#endif
    for (int k = 0; k < vkr::Aovs::kCount && productSized; ++k) {
        if (extraDst[k] && !extraFrame[k].empty() && !cryptoPlane[k]) {
            const size_t n = extraComponents[k], pixels = size_t(appWidth) * appHeight;
            for (size_t p = 0; p < pixels; ++p) {
                std::copy(extraFrame[k].data() + sourcePixel(p) * n, extraFrame[k].data() + sourcePixel(p) * n + n,
                          extraDst[k] + p * n);
            }
        }
    }
    if (!huskIds.empty()) {
        // 0 is the background, ids count from 1.
        auto *dst = static_cast<float *>(idForHusk->Map());
        for (size_t p = 0; p < size_t(appWidth) * appHeight; ++p) {
            const int32_t id = huskIds[sourcePixel(p)];
            dst[p] = id < 0 ? 0.0f : float(id + 1);
        }
        idForHusk->Unmap();
    }
    for (int i = 0; i < 4 && productSized; ++i) {
        if (!mapped[i]) {
            continue;
        }
        const size_t b = kTargetBytes[i];
        for (uint32_t y = 0; y < appHeight; ++y) {
            const size_t sy = size_t(y) * frame.height / appHeight;
            for (uint32_t x = 0; x < appWidth; ++x) {
                const size_t sx = size_t(x) * frame.width / appWidth;
                std::memcpy(static_cast<uint8_t *>(mapped[i]) + (size_t(y) * appWidth + x) * b,
                            productTargets[i].data() + (sy * frame.width + sx) * b, b);
            }
        }
    }
    try {
        _products.Write(*_renderer, frame, objects, materials, frame.linearColor);
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk render products failed: %s", e.what());
    }
    for (int i = 0; i < 4; ++i) {
        if (!buffers[i]) {
            continue;
        }
        buffers[i]->SetTexture(shared && i == 0 ? interop->Color() : shared && i == 1 ? interop->Depth()
                                                                                    : HgiTextureHandle());
        if (mapped[i]) {
            buffers[i]->Unmap();
            buffers[i]->TakeMapped();  // the pass's own map
            buffers[i]->SetFetch(nullptr);
            continue;
        }
        buffers[i]->SetFetch([renderer = _renderer, i](void *data) {
            vkr::Renderer::Targets t;
            t.color = i == 0 ? data : nullptr;
            t.depth = i == 1 ? static_cast<float *>(data) : nullptr;
            t.primId = i == 2 ? static_cast<int32_t *>(data) : nullptr;
            t.instanceId = i == 3 ? static_cast<int32_t *>(data) : nullptr;
            try {
                renderer->Read(t);
            } catch (const std::exception &e) {
                TF_RUNTIME_ERROR("HdVk readback failed: %s", e.what());
            }
        });
    }
    if (static const bool debug = TfGetenvBool("HDVK_DEBUG_AOVS", false); debug) {
        static int frames = 0;
        if (++frames % 60 == 1 && color) {  // reads them back like the app would
            const size_t centre = size_t(frame.height / 2) * frame.width + frame.width / 2;
            const GfHalf *c = static_cast<const GfHalf *>(color->Map()) + centre * 4;
            const float d = depth ? static_cast<const float *>(depth->Map())[centre] : -1.0f;
            std::fprintf(stderr, "Eldr frame %d: centre colour %g %g %g %g depth %g, shared %d, camera %s\n", frames,
                         float(c[0]), float(c[1]), float(c[2]), float(c[3]), d, int(shared),
                         camera ? camera->GetId().GetText() : "(none)");
            std::fflush(stderr);
            color->Unmap();
            static_cast<HdVkRenderBuffer *>(color)->TakeMapped();  // not the app's
            if (depth) {
                depth->Unmap();
                static_cast<HdVkRenderBuffer *>(depth)->TakeMapped();
            }
        }
    }
    for (HdRenderBuffer *b : extra) {
        if (b) {
            b->Unmap();
            static_cast<HdVkRenderBuffer *>(b)->TakeMapped();  // the pass's own map
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
