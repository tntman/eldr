#include "renderPass.h"

#include "camera.h"
#include "glInterop.h"
#include "instancer.h"
#include "renderBuffer.h"
#include "renderDelegate.h"
#include "renderer.h"
#include "timing.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/work/loops.h"
#include "pxr/imaging/cameraUtil/conformWindow.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <sstream>
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

TfToken BoundAov(HdRenderPassAovBinding const &b, const HdRenderSettings *settings = nullptr)
{
    const auto text = [&](const char *key) { return BindingText(b, key); };
    std::string name = text("sourceName"), type = text("sourceType");
    // Only a name Eldr doesn't know is a render var's (usdrecord binds Hydra's
    // "depth" while a product's var of that name reads ray:hitPz).
    if (name.empty() && settings &&
        HdVkRenderDelegate::KnownAovDescriptor(b.aovName).format == HdFormatInvalid) {
        HdVkRenderVarSource(settings, b.aovName, &name, &type);
    }
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

// An AOV's sample filter (vkr::Aovs::Filter) and radius (pixels, 0: the
// filter's own) from its render var's text: Eldr's (beauty, nearest, gauss,
// box, min, max, closest or farthest, then an optional radius, e.g.
// "gauss 1") or Karma's sample blending (["minmax",{"mode":"zmin"}]...;
// "ubox", Karma's default that Houdini authors on every var, is Eldr's
// default: colour like the beauty, data nearest). False: not one Eldr
// knows (default then).
bool ParseAovFilter(const std::string &text, uint8_t *filter, float *radius)
{
    using A = vkr::Aovs;
    *filter = A::kFilterDefault;
    *radius = 0.0f;
    if (text.empty()) {
        return true;
    }
    if (text[0] == '[') {
        if (text.find("\"ubox\"") != std::string::npos) {
            return true;
        }
        if (text.find("\"minmax\"") == std::string::npos) {
            return false;
        }
        for (auto [mode, f] : {std::pair{"\"zmin\"", A::kFilterClosest}, {"\"zmax\"", A::kFilterFarthest},
                               {"\"min\"", A::kFilterMin}, {"\"max\"", A::kFilterMax}}) {
            if (text.find(mode) != std::string::npos) {
                *filter = f;
                return true;
            }
        }
        return false;
    }
    std::istringstream in(text);
    std::string name;
    float r = 0.0f;
    in >> name;
    if (in >> r) {
        *radius = std::max(r, 0.0f);
    }
    for (auto [n, f] : {std::pair{"beauty", A::kFilterBeauty}, {"nearest", A::kFilterNearest}, {"gauss", A::kFilterGauss},
                        {"box", A::kFilterBox}, {"min", A::kFilterMin}, {"max", A::kFilterMax},
                        {"closest", A::kFilterClosest}, {"farthest", A::kFilterFarthest}}) {
        if (name == n) {
            *filter = f;
            return true;
        }
    }
    return false;
}

// HDVK_TIMING=2: a CSV row per render pass execute, for frame pacing in
// an app (Houdini's viewport): HDVK_TIMING_CSV, else %TEMP%/eldr_timing_<unix time>.csv.
// interval: since the last execute began; app: since it ended (the app's
// sync, maps and drawing); fetch: lazy readbacks in the app's maps since.
// ponytail: one file per process, passes interleave (one viewport is the case).
struct FrameTiming {
    using Clock = std::chrono::steady_clock;
    static FrameTiming *Get()
    {
        static FrameTiming *timing = [] {
            const char *level = std::getenv("HDVK_TIMING");
            if (!level || std::atoi(level) < 2) {
                return static_cast<FrameTiming *>(nullptr);
            }
            std::string path = TfGetenv("HDVK_TIMING_CSV");
            if (path.empty()) {
                const std::string temp = TfGetenv("TEMP", TfGetenv("TMPDIR", "."));
                path = TfStringPrintf("%s/eldr_timing_%lld.csv", temp.c_str(), (long long)std::time(nullptr));
            }
            FILE *csv = std::fopen(path.c_str(), "w");
            if (!csv) {
                TF_WARN("Eldr: can't write the timing CSV %s", path.c_str());
                return static_cast<FrameTiming *>(nullptr);
            }
            std::fprintf(stderr, "Eldr: frame timings to %s\n", path.c_str());
            std::fputs("frame,t_ms,interval_ms,app_ms,commit_ms,setup_ms,collect_ms,wait_ms,render_ms,after_ms,execute_ms,"
                       "fetch_ms,gpu_ms,render_cpu_ms,moving,deferred,collected,read_mask,width,height,converged,"
                       "stats_ms,stats_calls,map_ms,map_calls,alloc_ms,alloc_calls,isconverged_ms,isconverged_calls,"
                       "camsync_ms,camsync_calls,sync_ms,sync_calls,app_outside_eldr_ms,sync_wall_ms,mesh_topology_ms,"
                       "mesh_points_ms,mesh_instancer_ms,mesh_primvars_ms,mesh_skin_ms,mesh_shutter_ms,mesh_program_ms,"
                       "mesh_flatten_ms,mesh_transforms_ms,mesh_joints_ms,mesh_share_ms,shutter_hits\n",
                       csv);
            auto *t = new FrameTiming;
            t->csv = csv;
            return t;
        }();
        return timing;
    }
    static double Ms(Clock::time_point a, Clock::time_point b)
    {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
    FILE *csv = nullptr;
    Clock::time_point start = Clock::now(), lastBegin = start, lastEnd = start;
    std::atomic<int64_t> fetchUs{0};
    int frame = 0;
};

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
    HdVkTimedCall timed(HdVkCallTimes::kConverged);
    return !_renderer || (_renderer->IsConverged() && !_scene.LodsPending() && !_extraStale && !_failed && !_redicing);
}

// ponytail: renders the whole buffer and ignores the framing's data window
// and the collection. Fine for usdview; revisit for Houdini (step 7).
void HdVkRenderPass::_Execute(HdRenderPassStateSharedPtr const &state,
                              TfTokenVector const &renderTags)
{
    if (!_renderer) {
        return;
    }
    FrameTiming *const timing = FrameTiming::Get();
    const FrameTiming::Clock::time_point tBegin = FrameTiming::Clock::now();
    FrameTiming::Clock::time_point tCollect = tBegin, tRender = tBegin, tRendered = tBegin;

    // Only the pass's render tags (purposes) are drawn; no tags: all.
    uint32_t tagMask = renderTags.empty() ? ~0u : 0u;
    for (const TfToken &tag : renderTags) {
        tagMask |= HdVkRenderTagBit(tag);
    }
    if (_scene.SetTagMask(tagMask)) {
        _scene.Commit(true);  // the frame's second: its motion stays
    }

    vkr::Renderer::Frame frame;
    // The scene's time (HdSceneGlobalsSchema; 0 without), for MaterialX's
    // frame and time nodes.
    if (HdSceneIndexBaseRefPtr si = GetRenderIndex()->GetTerminalSceneIndex()) {
        const HdSceneGlobalsSchema globals = HdSceneGlobalsSchema::GetFromSceneIndex(si);
        const HdDoubleDataSourceHandle current = globals.GetCurrentFrame(), fps = globals.GetTimeCodesPerSecond();
        // NaN or -DBL_MAX: the default or earliest time code.
        const float now = current ? float(current->GetTypedValue(0.0f)) : 0.0f;
        frame.sceneTime[0] = std::isfinite(now) ? float(now) : 0.0f;
        const double rate = fps ? fps->GetTypedValue(0.0f) : 24.0;
        frame.sceneTime[1] = frame.sceneTime[0] / float(rate > 0.0 ? rate : 24.0);
    }
    HdRenderBuffer *color = nullptr, *depth = nullptr, *primId = nullptr, *instanceId = nullptr;
    // The depth AOV where nothing was hit (husk binds 0). Only the readback's:
    // the depth buffer itself clears to the far plane.
    float clearDepth = 1.0f;
    // Extra AOVs (vkr::Aovs) by their Hydra names (HdVkExtraAovs).
    // ponytail: one buffer per kind; of two names for the same (normal and
    // ray:hitN) bound at once, the last one gets it.
    HdRenderBuffer *extra[vkr::Aovs::kCount] = {};
    // husk's var name of the extra AOVs it binds ("driver:parameters:aov:husk:name").
    std::string huskName[vkr::Aovs::kCount];
    // Their sample filters (eldr:aovFilters): the binding's settings (husk,
    // Houdini), else the active settings prim's render var (usdrecord).
    std::string filterText[vkr::Aovs::kCount], filterVar[vkr::Aovs::kCount];
    bool huskPrimId = false;
    for (const HdRenderPassAovBinding &b : state->GetAovBindings()) {
        const TfToken aovName = BoundAov(b, static_cast<HdVkRenderDelegate *>(GetRenderIndex()->GetRenderDelegate())->RenderSettingsWithVar(b.aovName));
        if (HdVkExtraAov aov; HdVkFindAov(aovName, &aov)) {
            extra[aov.kind] = b.renderBuffer;
            huskName[aov.kind] = BindingText(b, "driver:parameters:aov:husk:name");
            for (const char *key : {"driver:parameters:aov:eldr:filter", "driver:parameters:aov:karma:filter"}) {
                if (filterText[aov.kind].empty()) {
                    filterText[aov.kind] = BindingText(b, key);
                    filterVar[aov.kind] = b.aovName.GetString();
                }
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
                clearDepth = b.clearValue.UncheckedGet<float>();
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
    // Who renders. Houdini's viewport sets houdini:viewport; husk sets no
    // enableInteractive but hands its raster products over as a setting
    // (rasterRenderProducts). Attributes of the settings prim (husk:*) don't
    // tell: the viewport looks through the same prims.
    // (HdVkRenderDelegate::HoudiniViewport, Offline.)
    const bool houdiniViewport = static_cast<HdVkRenderDelegate *>(delegate)->HoudiniViewport();
    const bool batch = !houdiniViewport && !delegate->GetRenderSetting(TfToken("rasterRenderProducts")).IsEmpty();
    bool resync = false;  // a setting from the prim changes the meshes
    if (const HdRenderSettings *prim = static_cast<HdVkRenderDelegate *>(delegate)->ActiveRenderSettings()) {
        for (const auto &[key, value] : prim->GetNamespacedSettings()) {
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
            // eldr:textureCompression: a bool means 0 or 2 (a cast would make true 1).
            const VtValue typed = token == HdVkRenderSettingsTokens->textureCompression && value.IsHolding<bool>()
                                      ? VtValue(value.UncheckedGet<bool>() ? 2 : 0)
                                      : VtValue::CastToTypeOf(value, current);
            if (!typed.IsEmpty()) {
                delegate->SetRenderSetting(token, typed);
                // These re-sync the meshes (SetRenderSetting): wait for that.
                const auto &ts = *HdVkRenderSettingsTokens;
                resync = resync || (typed != current && (token == ts.subdivision || token == ts.maxRefineLevel ||
                                                         token == ts.finalQuality ||
                                                         token == HdVkRenderSettingsTokens->screenDicing ||
                                                         token == HdVkRenderSettingsTokens->textureCompression));
            }
        }
    }
    // Each setting over the renderer's default (renderDelegate.cpp lists
    // them); counts are ints in Hydra, at least 1.
    const auto &t = *HdVkRenderSettingsTokens;
    const auto &t2 = *HdVkRenderSettingsTokens;
    for (auto [token, value] : {std::pair{t.postProcess, &frame.postProcess}, {t.taa, &frame.taa},
                                {t.ambientOcclusion, &frame.ambientOcclusion}, {t.ssr, &frame.ssr},
                                {t.autoExposure, &frame.post.autoExposure}, {t.shadows, &frame.shadows},
                                {t.localShadows, &frame.localShadows}, {t.softShadows, &frame.softShadows},
                                {t.depthOfField, &frame.depthOfField}, {t.motionBlur, &frame.motionBlur},
                                {t2.motionBlurTimeSamples, &frame.motionBlurTimeSamples},
                                {t2.motionBlurResidual, &frame.motionBlurResidual},
                                {t2.planarMotionBlur, &frame.planarMotionBlur},
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
                                {t.gi, &frame.gi}, {t.ssgi, &frame.ssgi},
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
                                {t2.rtCompaction, &frame.rtCompaction},
                                {t2.rtRefractionDisplacement, &frame.rtRefractionDisplacement},
                                {t2.rtRefractionClip, &frame.rtRefractionClip}, {t2.opacityMode, &frame.opacityMode},
                                {t2.giRelocation, &frame.giRelocation},
                                {t2.rtShadowDenoise, &frame.rtShadowDenoise},
                                {t2.rtReflectionDenoise, &frame.rtReflectionDenoise},
                                {t2.rtReflectionRoulette, &frame.rtReflectionRoulette},
                                {t2.ssrDenoise, &frame.ssrDenoise}, {t2.shadowJitter, &frame.shadowJitter},
                                {t2.rtTranslucentShadows, &frame.rtTranslucentShadows},
                                {t2.dofJitter, &frame.dofJitter},
                                {t2.authoredProbes, &frame.authoredProbes},
                                {t2.probeNormalize, &frame.probeNormalize},
                                {t2.domeSun, &frame.domeSun},
                                {t2.fogGiAmbient, &frame.fogGiAmbient},
                                {t2.lightTextures, &frame.lightTextures}, {t2.productLinear, &frame.linearBeauty},
                                {t2.lightLinking, &frame.lightLinking}, {t2.lightIes, &frame.lightIes},
                                {t2.displacementGraphs, &frame.displacementGraphs},
                                {t2.lodFade, &frame.lodFade},
                                {t2.splitDomeDirect, &frame.splitDomeDirect},
                                {t2.cryptomatteSoft, &frame.cryptomatteSoft},
                                {t2.deformationBlur, &frame.deformationBlur},
                                {t2.dofResidualBlur, &frame.dofResidualBlur},
                                {t2.productResolution, &frame.productResolution},
                                {t2.localVolumeShadows, &frame.localVolumeShadows},
                                {t2.volumeMutualShadows, &frame.volume.mutualShadows},
                                {t2.volumeDefaultFields, &frame.volume.defaultFields},
                                {t2.volumeMultipleScattering, &frame.volume.multipleScattering},
                                {t2.lightShapesVisible, &frame.lightShapesVisible},
                                {t2.domeCameraVisible, &frame.showEnvironment},
                                {t2.multiScatter, &frame.multiScatter},
                                {t2.overlapReadback, &frame.overlapReadback},
                                {t2.inPlaceReadback, &frame.inPlaceReadback},
                                {t2.staleMovingIds, &frame.staleMovingIds},
                                {t2.lazyPipelines, &frame.lazyPipelines},
                                {t2.lazyExtraAovs, &frame.lazyExtraAovs},
                                {t2.movingGiPause, &frame.movingGiPause},
                                {t2.screenBackface, &frame.screenBackface},
                                {t2.ssrHitFilter, &frame.ssrHitFilter},
                                {t2.ssgiTileOrder, &frame.ssgiTileOrder}, {t2.denoise, &frame.denoise},
                                {t2.adaptiveTiles, &frame.adaptiveTiles}}) {
        *value = delegate->GetRenderSetting<bool>(token, *value);
    }
    // eldr:denoise, else Karma's: its viewport toggle (karma:hydra:denoise)
    // or a "denoise" image filter (karma:global:imagefilter, its final
    // renders), from the delegate's settings or the settings prim's. An
    // eldr:denoise authored on the prim (false too) overrides them.
    if (!frame.denoise && !_appliedSettings.count(t2.denoise)) {
        const HdRenderSettings *prim = static_cast<HdVkRenderDelegate *>(delegate)->ActiveRenderSettings();
        auto karma = [&](const char *name) {
            VtValue v = delegate->GetRenderSetting(TfToken(name));
            if (v.IsEmpty() && prim) {
                const VtDictionary &settings = prim->GetNamespacedSettings();
                const auto it = settings.find(name);
                v = it != settings.end() ? it->second : VtValue();
            }
            return v;
        };
        const VtValue toggle = VtValue::Cast<bool>(karma("karma:hydra:denoise"));
        const VtValue filter = VtValue::Cast<std::string>(karma("karma:global:imagefilter"));
        frame.denoise = (!toggle.IsEmpty() && toggle.UncheckedGet<bool>()) ||
                        (!filter.IsEmpty() && filter.UncheckedGet<std::string>().find("\"denoise\"") != std::string::npos);
    }
    // Volume Limit, the largest on a volume (HdVkVolume::Sync) or in the
    // render settings (eldr:object:volumelimit set before
    // karma:object:volumelimit): above 0 volumes scatter more than once.
    {
        static const TfToken eldrLimit("eldr:object:volumelimit"), karmaLimit("karma:object:volumelimit");
        auto number = [](const VtValue &v) {
            return v.IsHolding<float>() ? double(v.UncheckedGet<float>()) : v.IsHolding<double>() ? v.UncheckedGet<double>()
                 : v.IsHolding<int>() ? double(v.UncheckedGet<int>()) : -1.0;
        };
        // husk hands the settings prim's attributes over as settings; usdview
        // and usdrecord don't, so read the active prim too.
        const HdRenderSettings *prim = static_cast<HdVkRenderDelegate *>(delegate)->ActiveRenderSettings();
        auto setting = [&](const TfToken &key) {
            VtValue v = delegate->GetRenderSetting(key);
            if (v.IsEmpty() && prim) {
                const VtDictionary &d = prim->GetNamespacedSettings();
                const auto it = d.find(key.GetString());
                v = it != d.end() ? it->second : v;
            }
            return v;
        };
        double limit = number(setting(eldrLimit));
        if (limit < 0.0) {
            limit = number(setting(karmaLimit));
        }
        frame.volume.volumeLimit = std::max(float(limit), _scene.MaxVolumeLimit());
    }
    // Hydra's own switch (usdview's Lights menu) also hides the dome.
    frame.showEnvironment = frame.showEnvironment &&
        delegate->GetRenderSetting<bool>(HdRenderSettingsTokens->domeLightCameraVisibility, true);
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
    // Houdini's viewport frames the settings prim's products itself.
    const bool hasFraming = frame.renderProducts && !houdiniViewport && HdVkRenderProducts::GetFraming(
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
                                {t.shadowCullPixels, &frame.shadowCullPixels},
                                {t.exposureCompensation, &frame.post.exposureCompensation},
                                {t.bloomIntensity, &frame.post.bloomIntensity},
                                {t.giHysteresis, &frame.giHysteresis}, {t.ssgiRadius, &frame.ssgiRadius},
                                {t.emissiveLightThreshold, &frame.emissiveLightThreshold},
                                {t.oitDepthWeight, &frame.oitDepthWeight},
                                {t.fogDensity, &frame.fogDensity}, {t.fogHeight, &frame.fogHeight},
                                {t.fogHeightFalloff, &frame.fogHeightFalloff},
                                {t.fogAnisotropy, &frame.fogAnisotropy}, {t.fogMaxDistance, &frame.fogMaxDistance},
                                {t.planarScale, &frame.planarScale}, {t2.planarBlur, &frame.planarBlur},
                                {t.volumeStepSize, &frame.volume.stepSize}, {t.volumeDensity, &frame.volume.density},
                                {t.volumeEmission, &frame.volume.emission},
                                {t.volumeAnisotropy, &frame.volume.anisotropy},
                                {t.displacementScale, &frame.displacementScale}, {t.lodBias, &frame.lodBias},
                                {t2.domeSunThreshold, &frame.domeSunThreshold},
                                {t.filmFilterRadius, &frame.filmFilterRadius}, {t2.renderScale, &frame.renderScale},
                                {t2.movingRenderScale, &frame.movingRenderScale},
                                {t.adaptiveThreshold, &frame.adaptiveThreshold},
                                {t2.adaptiveFinalThreshold, &frame.adaptiveFinalThreshold},
                                {t2.adaptivePixelFraction, &frame.adaptivePixelFraction},
                                {t2.directIntensity, &frame.directIntensity},
                                {t2.indirectIntensity, &frame.indirectIntensity},
                                {t2.ssgiBitmaskThickness, &frame.ssgiBitmaskThickness}, {t2.directClamp, &frame.directClamp},
                                {t2.screenBackfaceScale, &frame.screenBackfaceScale},
                                {t2.finalGatherDistance, &frame.finalGatherDistance},
                                {t2.finalGatherDenoise, &frame.finalGatherDenoise},
                                {t2.causticsRadius, &frame.causticsRadius}}) {
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
                                {t.giProbes, &frame.giProbes}, {t.giRaysPerProbe, &frame.giRays},
                                {t.ssgiSteps, &frame.ssgiSteps}, {t.ssgiResolution, &frame.ssgiResolution},
                                {t.refractionSamples, &frame.refractionSamples}, {t.refractionLayers, &frame.refractionLayers},
                                {t.translucentShadowDivisor, &frame.translucentShadowDivisor},
                                {t.subsurfaceSamples, &frame.subsurfaceSamples},
                                {t.fogGridSize, &frame.fogGridSize}, {t.probeResolution, &frame.probeResolution},
                                {t.volumeMaxSteps, &frame.volume.maxSteps},
                                {t.volumeShadowSteps, &frame.volume.shadowSteps},
                                {t.volumeLocalShadowStride, &frame.volume.localShadowStride},
                                {t.volumeResolution, &frame.volume.resolution},
                                {t2.planarCount, &frame.planarCount},
                                {t2.rtRefractionBounces, &frame.rtRefractionBounces},
                                {t.filmSamples, &frame.filmSamples},
                                {t.interactiveSamples, &frame.interactiveSamples},
                                {t.adaptiveMinSamples, &frame.adaptiveMinSamples}}) {
        *value = uint32_t(std::max(delegate->GetRenderSetting<int>(token, int(*value)), 1));
    }
    frame.movingShadowRays = uint32_t(std::clamp(delegate->GetRenderSetting<int>(t2.movingShadowRays, 1), 0, 16));
    frame.volume.shadowGrid =
        uint32_t(std::clamp(delegate->GetRenderSetting<int>(t2.volumeShadowGrid, int(frame.volume.shadowGrid)), 0, 128));
    // Colour limit (Karma's per-sample brightness limit, default 20): the indirect clamp, from
    // eldr:indirectClamp, else (colour limit not shared) karma:global:colorlimitindirect, else the
    // shared limit eldr:colorLimit, else karma:global:colorlimit, else 20. 0 = off.
    {
        static const TfToken karmaLimit("karma:global:colorlimit"), karmaIndirect("karma:global:colorlimitindirect"),
            karmaLinked("karma:global:colorlimitindlinked");
        // Karma's names also from the active settings prim (usdview, usdrecord): only eldr: ones are
        // copied into the delegate's settings above.
        const HdRenderSettings *prim = static_cast<HdVkRenderDelegate *>(delegate)->ActiveRenderSettings();
        auto number = [&](const TfToken &token, float fallback) {
            VtValue v = delegate->GetRenderSetting(token);
            if (v.IsEmpty() && prim) {
                const VtDictionary &d = prim->GetNamespacedSettings();
                const auto it = d.find(token.GetString());
                v = it != d.end() ? it->second : v;
            }
            return v.IsHolding<double>() ? float(v.UncheckedGet<double>())
                   : v.IsHolding<float>() ? v.UncheckedGet<float>()
                   : v.IsHolding<int>()   ? float(v.UncheckedGet<int>()) : fallback;
        };
        // The settings' descriptors default to -1: not set, so Karma's apply.
        auto set = [&](const TfToken &token, float fallback) {
            const float v = number(token, -1.0f);
            return v >= 0.0f ? v : fallback;
        };
        const float shared = set(t2.colorLimit, set(karmaLimit, 20.0f));
        const VtValue sharedSetting = delegate->GetRenderSetting(t2.colorLimitShared);
        const int sharedValue = sharedSetting.IsHolding<bool>() ? int(sharedSetting.UncheckedGet<bool>())
                                                                : int(number(t2.colorLimitShared, -1.0f));
        const bool linked = sharedValue >= 0 ? sharedValue != 0 : delegate->GetRenderSetting<bool>(karmaLinked, true);
        frame.indirectClamp = set(t2.indirectClamp, linked ? shared : set(karmaIndirect, shared));
        frame.colorLimit = shared;  // camera-seen emission, as Karma's colorlimit
    }
    // Per-component quality (Karma's): eldr:*Quality, else Karma's on the
    // settings (husk) or the active settings prim (usdview), else 1. Rays
    // per sample times it; the renderer spreads fractions over film samples.
    {
        static const TfToken karmaLight("karma:global:light_sampling_quality"),
            karmaDiffuse("karma:object:diffusequality"), karmaReflect("karma:object:reflectquality"),
            karmaSss("karma:object:sssquality"), karmaVolume("karma:object:volumequality");
        const HdRenderSettings *prim = static_cast<HdVkRenderDelegate *>(delegate)->ActiveRenderSettings();
        auto number = [&](const TfToken &key) {
            VtValue v = delegate->GetRenderSetting(key);
            if (v.IsEmpty() && prim) {
                const VtDictionary &d = prim->GetNamespacedSettings();
                const auto it = d.find(key.GetString());
                v = it != d.end() ? it->second : v;
            }
            return v.IsHolding<float>() ? double(v.UncheckedGet<float>()) : v.IsHolding<double>() ? v.UncheckedGet<double>()
                 : v.IsHolding<int>()   ? double(v.UncheckedGet<int>()) : -1.0;
        };
        auto quality = [&](const TfToken &eldr, const TfToken &karma) {
            double q = number(eldr);
            q = q >= 0.0 ? q : number(karma);
            return float(q >= 0.0 ? std::clamp(q, 1.0 / 64.0, 16.0) : 1.0);
        };
        frame.lightQuality = quality(t2.lightQuality, karmaLight);
        frame.diffuseQuality = quality(t2.diffuseQuality, karmaDiffuse);
        frame.reflectQuality = quality(t2.reflectQuality, karmaReflect);
        frame.subsurfaceSamples =
            uint32_t(std::clamp(std::lround(frame.subsurfaceSamples * quality(t2.sssQuality, karmaSss)), 1l, 64l));
        frame.volume.shadowSteps =
            uint32_t(std::clamp(std::lround(frame.volume.shadowSteps * quality(t2.volumeQuality, karmaVolume)), 1l, 256l));
    }
    // 0 allowed: off. Reflections: a bool (older scenes) is 0 or 2.
    {
        const VtValue v = delegate->GetRenderSetting(t.rtReflections);
        frame.rtReflections = v.IsHolding<bool>() ? (v.UncheckedGet<bool>() ? 2u : 0u)
                                                  : uint32_t(std::clamp(delegate->GetRenderSetting<int>(t.rtReflections, int(frame.rtReflections)), 0, 2));
    }
    frame.rtRefraction =
        uint32_t(std::clamp(delegate->GetRenderSetting<int>(t2.rtRefraction, int(frame.rtRefraction)), 0, 2));
    frame.rtHitShadows =
        uint32_t(std::clamp(delegate->GetRenderSetting<int>(t2.rtHitShadows, int(frame.rtHitShadows)), 0, 2));
    frame.finalGather =
        uint32_t(std::clamp(delegate->GetRenderSetting<int>(t2.finalGather, int(frame.finalGather)), 0, 2));
    frame.finalGatherRays =
        uint32_t(std::clamp(delegate->GetRenderSetting<int>(t2.finalGatherRays, int(frame.finalGatherRays)), 1, 16));
    frame.effectDenoiseSamples = uint32_t(
        std::clamp(delegate->GetRenderSetting<int>(t2.effectDenoiseSamples, int(frame.effectDenoiseSamples)), 0, 65536));
    frame.caustics = uint32_t(std::clamp(delegate->GetRenderSetting<int>(t2.caustics, int(frame.caustics)), 0, 2));
    frame.causticsPhotons = uint32_t(
        std::clamp(delegate->GetRenderSetting<int>(t2.causticsPhotons, int(frame.causticsPhotons)), 1024, 1 << 22));
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
    // A camera path to no camera (a default Render Settings LOP's
    // /cameras/camera1) leaves identity matrices: keep the last real view,
    // the viewport's camera, instead of a unit box at the origin.
    GfMatrix4d view = state->GetWorldToViewMatrix();
    if (!camera && view == GfMatrix4d(1.0) && projection == GfMatrix4d(1.0)) {
        if (_hasLastCamera) {
            view = _lastView;
            projection = _lastProjection;
        }
    } else {
        _hasLastCamera = true;
        _lastView = view;
        _lastProjection = projection;
    }
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
    const GfMatrix4f viewF(view);
    std::copy(viewF.data(), viewF.data() + 16, frame.view);
    const GfMatrix4d viewProj = view * projection;
    const GfMatrix4f viewProjF(viewProj), invViewProjF(viewProj.GetInverse());
    std::copy(viewProjF.data(), viewProjF.data() + 16, frame.viewProj);
    std::copy(invViewProjF.data(), invViewProjF.data() + 16, frame.invViewProj);

    // Final quality: asked for, or an offline renderer's convention (husk,
    // hdPrman, usdrecord: enableInteractive false). A host's
    // first frame has no history, so its film starts with the first sample.
    frame.firstFrameFinal = !houdiniViewport;
    const bool offline = static_cast<HdVkRenderDelegate *>(delegate)->Offline();
    frame.finalQuality = frame.finalQuality || offline;
    // Nobody toggles a feature in a batch render: no prewarm (its compiles
    // only made the teardown wait, seconds).
    frame.pipelinePrewarm = frame.pipelinePrewarm && !offline;
    // An offline frame's exposure is deterministic (like Karma's: none)
    // unless the render settings prim asks for eldr:autoExposure.
    // Houdini's viewport colour-corrects every delegate's colour itself (its
    // OCIO view, gamma, LUT): with eldr:productLinear it gets the EXR's
    // linear beauty, as from Karma, not Eldr's tonemap, bloom or auto exposure.
    const bool hostLinear = houdiniViewport && frame.linearBeauty;
    if (hostLinear) {
        frame.post.tonemapper = Tonemapper::None;
        frame.post.bloomIntensity = 0.0f;
    }
    if ((offline || hostLinear) && !_appliedSettings.count(t.autoExposure)) {
        frame.post.autoExposure = false;
    }
    // Bloom mixes the blurred image in: 0-1 (past 1 it extrapolates to dark rings).
    frame.post.bloomIntensity = std::clamp(frame.post.bloomIntensity, 0.0f, 1.0f);
    // Any edit to the scene or the settings starts the film over; the
    // camera's only when its lens changed (below).
    frame.sceneVersion = (static_cast<HdVkRenderDelegate *>(delegate)->SceneEdits() << 32) |
                         delegate->GetRenderSettingsVersion();
    frame.toonBands = uint32_t(std::max(delegate->GetRenderSetting<int>(t.toonBands, int(frame.toonBands)), 0));
    // Any may change the light table: commit once.
    const bool emissive = _scene.SetEmissiveLights(frame.emissiveLights, frame.emissiveLightThreshold,
                                                    frame.indirectClamp);
    const bool lightOptions = _scene.SetLightOptions(frame.lightLinking, frame.lightIes);
    if (_scene.SetDomeSun(frame.domeSun, frame.domeSunThreshold) || emissive || lightOptions) {
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
    {
        const vkr::LensDistortion::Params &d = frame.distortion;
        const float lens[] = {frame.shutter, frame.shutterOpen, frame.fStop, frame.focusDistance, frame.focalLength,
                              d.k1, d.k2, d.k3, d.k4, d.k5, d.k6, d.p1, d.p2, d.center[0], d.center[1], d.anaSq,
                              d.asym[0], d.asym[1], d.scale, float(d.fisheye)};
        for (const float v : lens) {
            uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            frame.sceneVersion = (frame.sceneVersion ^ bits) * 1099511628211ull;
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
    // eldr:screenDicing: subdivision surfaces refine for this view. Offline
    // they re-dice whenever it changed and this frame waits for them (not
    // converged, nothing rendered: husk comes back after the sync); an
    // interactive final-quality frame keeps its levels while the camera
    // moves and re-dices once it stands still. The same wait for a
    // subdivision setting the settings prim just changed.
    _redicing = resync;
    if (resync) {
        static_cast<HdVkRenderDelegate *>(delegate)->KeepMotion();
    }
    if (resync && offline) {
        return;
    }
    if (auto *vk = static_cast<HdVkRenderDelegate *>(delegate); vk->ScreenDicing()) {
        HdVkDicingView dicing;
        dicing.viewProj = viewProj;
        dicing.eye = eye;
        dicing.ortho = projection[3][3] != 0.0;
        dicing.width = int(frame.width);
        dicing.height = int(frame.height);
        dicing.pixels = std::min(std::abs(projection[0][0]) * frame.width, std::abs(projection[1][1]) * frame.height) / 2.0;
        const bool still = _lastDicing && dicing == *_lastDicing;
        _lastDicing = dicing;
        if ((!vk->DicingView() || !(dicing == *vk->DicingView())) && (offline || still)) {
            vk->SetDicingView(dicing);
            _redicing = true;
            if (offline) {
                return;
            }
        }
    }
    // Sampled motion: rprims sample over this camera's shutter (from the next
    // sync on; a first frame's take the open camera that synced), and the
    // camera's own motion comes from its samples.
    float shutterOpen, shutterClose;
    static_cast<HdVkRenderDelegate *>(delegate)->SetActiveCamera(camera ? camera->GetId() : SdfPath());
    frame.sampledMotion = frame.shutter > 0.0f &&
                          static_cast<HdVkRenderDelegate *>(delegate)->SampledMotionShutter(&shutterOpen, &shutterClose);
    std::copy(viewProjF.data(), viewProjF.data() + 16, frame.prevViewProj);
    std::copy(frame.cameraPos, frame.cameraPos + 3, frame.prevCameraPos);
    // Without sampled motion the camera's motion is the last frame's pose:
    // none for Houdini's viewport camera, nor across a switch of cameras.
    const SdfPath cameraId = camera ? camera->GetId() : SdfPath();
    frame.stillCamera = cameraId != _lastCameraId || HdVkCamera::IsViewportCamera(cameraId);
    _lastCameraId = cameraId;
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
    const bool moving = std::memcmp(frame.viewProj, _lastViewProj, sizeof(_lastViewProj)) != 0;
    std::copy(frame.viewProj, frame.viewProj + 16, _lastViewProj);
    // eldr:staleMovingIds: while Houdini's viewport camera moves the ids (its
    // picking and selection outline, mapped every frame) keep the last still
    // frame's: nobody picks mid-tumble, it renders on once the camera stops,
    // and they are 8 of a pixel's 20 bytes read back and uploaded by the
    // app. Not elsewhere: usdview picks and tests read them right after a
    // camera change. Detached: the renderer's memory they were mapped in
    // (eldr:inPlaceReadback) will hold other outputs.
    const bool staleIds =
        frame.staleMovingIds && houdiniViewport && moving && !frame.finalQuality && !productSized;
    for (int i = 0; i < 4; ++i) {
        if (staleIds && i >= 2 && buffers[i]) {
            buffers[i]->Detach();
            continue;
        }
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
    // HDVK_DEBUG_AOVS: every 60 frames, what started the film over (a still
    // camera should show none) and the near and far planes the depth AOV
    // (window depth) is under.
    if (static const bool debug = TfGetenvBool("HDVK_DEBUG_AOVS", false); debug) {
        static uint64_t lastScene = 0;
        static int frames = 0, moved = 0, edited = 0;
        moved += moving;
        edited += frame.sceneVersion != lastScene;
        lastScene = frame.sceneVersion;
        if (++frames % 60 == 0) {
            const double n = projection[3][2] / (projection[2][2] - 1.0), f = projection[3][2] / (projection[2][2] + 1.0);
            std::fprintf(stderr, "Eldr last 60 frames: camera moved %d, scene or settings changed %d; near %g far %g, "
                         "%ux%u, final %d, product framing %d, depth cleared to %g\n", moved, edited, n, f, frame.width,
                         frame.height, int(frame.finalQuality), int(productSized), clearDepth);
            std::fflush(stderr);
            moved = edited = 0;
        }
    }
    // eldr:lazyExtraAovs: a still camera's too until its film converged,
    // then one more frame renders them (IsConverged waits for it): they cost
    // a G-buffer pass and readback a frame (4.9 ms GPU at 1866x737).
    const bool skipUnread = moving || (frame.lazyExtraAovs && !_renderer->IsConverged());
    bool skipped = false;
    for (HdRenderBuffer *&b : extra) {
        const bool skip = b && skipUnread && !frame.finalQuality && !productSized && !read.count(b);
        skipped = skipped || skip;
        b = skip ? nullptr : b;
    }
    _extraStale = skipped;
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
    // Per-AOV sample filters (eldr:aovFilters). A light split filtered
    // otherwise than the beauty no longer adds up to it: a warning.
    if (delegate->GetRenderSetting<bool>(t2.aovFilters, true)) {
        std::string text[vkr::Aovs::kCount], var[vkr::Aovs::kCount];
        HdVkRenderProducts::GetFilters(vkDelegate->ActiveRenderSettings(), text, var);
        static std::set<std::string> warned;
        for (uint32_t k = 0; k < vkr::Aovs::kCount; ++k) {
            const std::string &t = filterText[k].empty() ? text[k] : filterText[k];
            const std::string &v = filterText[k].empty() ? var[k] : filterVar[k];
            if (t.empty()) {
                continue;
            }
            using A = vkr::Aovs;
            const bool known = ParseAovFilter(t, &out.aovFilters[k], &out.aovFilterRadius[k]);
            const uint8_t f = out.aovFilters[k];
            const bool split = k == A::kEmission || (k >= A::kDirectDiffuse && k < A::kBeautyRaw &&
                                                     k != A::kShadow && k != A::kHoldoutShadow);
            const bool ids = k == A::kCryptoObject || k == A::kCryptoMaterial || k == A::kBeautyRaw;
            const char *problem = !known ? "isn't a filter Eldr knows; default used"
                                  : ids && f != A::kFilterDefault ? "is ignored: this AOV keeps its own"
                                  : split && f != A::kFilterDefault && f != A::kFilterBeauty
                                      ? "differs from the beauty's: this light split no longer adds up to it"
                                      : nullptr;
            if (problem && warned.insert(v + "|" + t).second) {
                TF_WARN("Eldr: AOV filter '%s' of %s %s", t.c_str(), v.c_str(), problem);
            }
        }
    }
    // Light groups: the tags the bindings and products asked for so far
    // (HdVkFindAov) split the lights from this frame on.
    if (_scene.SetLightGroups(HdVkLightGroups())) {
        _scene.Commit(true);  // the frame's second: its motion stays
    }
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

    // eldr:overlapReadback: while the camera moves this frame stays on the
    // GPU while the app works, and the next one delivers it: the app shows
    // the frame before (one behind; a still camera's frame is its own). Only
    // the plain readback, into the buffers last frame filled: they keep that
    // image until a deferred one is collected.
    // eldr:inPlaceReadback: colour, depth and the ids stay where the GPU
    // copied them (two buffers in turn) and the app maps them there, no copy
    // (a maximized viewport's four: 27 MB, ~5 ms). Not resampled or converted ones.
    // eldr:offlineBatchSeconds: an offline frame (husk, usdrecord) renders
    // film samples back to back for that long or until converged, and
    // reads back only the last: the app's wait between executes (husk:
    // ~100 ms a sample) and every sample's readback go. Viewports keep one
    // sample per execute.
    const float batchSeconds = offline ? delegate->GetRenderSetting<float>(t2.offlineBatchSeconds, 1.0f) : 0.0f;
    out.inPlace = frame.inPlaceReadback && !productSized && colorHalf.empty() && huskIds.empty() && batchSeconds <= 0.0f;
    auto readBuffer = [&](int i) { return uintptr_t(mapped[i] || (staleIds && i >= 2) ? buffers[i] : nullptr); };
    const std::array<uintptr_t, 6> readback = {readBuffer(0), readBuffer(1), readBuffer(2), readBuffer(3), frame.width,
                                               frame.height};
    const std::array<float, 4> depthConstants = {frame.depthToDistance[0], frame.depthToDistance[1],
                                                 frame.depthToDistance[2], frame.depthToDistance[3]};
    std::array<float, 4> collectedDepth = depthConstants;
    frame.deferReadback = frame.overlapReadback && moving && !frame.finalQuality && !productSized && !interop &&
                          colorHalf.empty() && huskIds.empty() && !frame.linearColor && readback == _lastReadback &&
                          std::none_of(out.aovs, out.aovs + vkr::Aovs::kCount, [](float *a) { return a; });
    bool collected = false;
    bool shared = false;
    _failed = false;
    try {
        tCollect = FrameTiming::Clock::now();
        if (frame.deferReadback && (collected = _renderer->Collect(out, frame.width, frame.height))) {
            collectedDepth = _lastDepthConstants;
        }
        // Ours went uncollected (another pass rendered since): this one now.
        frame.deferReadback = frame.deferReadback && (collected || !_lastDeferred);
        _lastDeferred = frame.deferReadback;
        _lastReadback = readback;
        _lastDepthConstants = depthConstants;
        tRender = FrameTiming::Clock::now();
        if (batchSeconds > 0.0f && !frame.deferReadback) {
            vkr::Renderer::Targets sample = out;
            sample.readback = false;
            const auto until = tRender + std::chrono::duration_cast<FrameTiming::Clock::duration>(
                                             std::chrono::duration<double>(batchSeconds));
            for (;;) {
                _renderer->Render(_scene, frame, sample);
                vkDelegate->NoteFrame();
                if ((_renderer->IsConverged() && !_scene.LodsPending()) || FrameTiming::Clock::now() >= until) {
                    break;
                }
                // As the app's commit between executes: last sample's motion settles.
                vkDelegate->CommitResources(nullptr);
                frame.stillCamera = HdVkCamera::IsViewportCamera(cameraId);  // the next execute's
            }
            _renderer->Read(out);
        } else {
            _renderer->Render(_scene, frame, out);
            vkDelegate->NoteFrame();
        }
        tRendered = FrameTiming::Clock::now();
        shared = interop && interop->Present(*_renderer);
        if (interop && !shared) {
            vkDelegate->DisableGLInterop();
        }
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk render failed: %s", e.what());
        _failed = true;  // the renderer's convergence describes a frame that never ran
    }
    vkDelegate->SetFrameShared(shared);
    if (out.inPlace && (!frame.deferReadback || collected)) {
        for (int i = 0; i < 4; ++i) {
            if (mapped[i] && _renderer->HostOutput(i)) {
                mapped[i] = _renderer->HostOutput(i);
                buffers[i]->SetExternal(mapped[i]);
            }
        }
    }
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
    // The depth AOV: window depth, the binding's clear value where nothing
    // was hit. Houdini: linear like Karma's (Pz: distance along the view
    // axis, 0 where nothing was hit; UsdRenderers.json "depthstyle":
    // "linear"), so its pivot, pan speed and guide compositing don't depend
    // on the near and far planes it assumes for window depth.
    const auto depthAovOf = [clearDepth](std::array<float, 4> c) {
        return [c, clearDepth](float *d, size_t n) {
            WorkParallelForN(n, [&](size_t begin, size_t end) {  // a maximized viewport's: ~1.4 M pixels
                for (size_t p = begin; p < end; ++p) {
#ifdef HDVK_HOUDINI
                    const float z = d[p] * 2.0f - 1.0f;
                    d[p] = d[p] >= 1.0f ? 0.0f : -(c[0] * z + c[1]) / (c[2] * z + c[3]);
                    (void)clearDepth;
#else
                    d[p] = d[p] >= 1.0f ? clearDepth : d[p];
                    (void)c;
#endif
                }
            }, 1 << 16);
        };
    };
    const auto depthAov = depthAovOf(depthConstants);
    // A deferred frame's buffer holds the last converted one unless collected.
    if (mapped[1] && (!frame.deferReadback || collected)) {
        depthAovOf(collectedDepth)(static_cast<float *>(mapped[1]), size_t(appWidth) * appHeight);
    }
    try {
        if (!_failed) {  // its AOVs were never read back: the products would hold zeros
            _products.Write(*_renderer, frame, objects, materials, frame.linearColor,
                            [vkDelegate] { return vkDelegate->RenderStatsJson(); });
        }
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk render products failed: %s", e.what());
    }
    for (int i = 0; i < 4; ++i) {
        if (!buffers[i]) {
            continue;
        }
        buffers[i]->SetTexture(shared && i == 0 ? interop->Color() : shared && i == 1 ? interop->Depth()
                                                                                    : HgiTextureHandle());
        if (mapped[i] || (staleIds && i >= 2)) {
            if (mapped[i]) {
                buffers[i]->Unmap();
                buffers[i]->TakeMapped();  // the pass's own map
            }
            buffers[i]->SetFetch(nullptr);
            continue;
        }
        const size_t pixels = size_t(appWidth) * appHeight;
        buffers[i]->SetFetch([renderer = _renderer, i, depthAov, pixels, timing](void *data) {
            const auto fetchStart = FrameTiming::Clock::now();
            vkr::Renderer::Targets t;
            t.color = i == 0 ? data : nullptr;
            t.depth = i == 1 ? static_cast<float *>(data) : nullptr;
            t.primId = i == 2 ? static_cast<int32_t *>(data) : nullptr;
            t.instanceId = i == 3 ? static_cast<int32_t *>(data) : nullptr;
            try {
                renderer->Read(t);
                if (t.depth) {
                    depthAov(t.depth, pixels);
                }
            } catch (const std::exception &e) {
                TF_RUNTIME_ERROR("HdVk readback failed: %s", e.what());
            }
            if (timing) {
                timing->fetchUs += int64_t(1000.0 * FrameTiming::Ms(fetchStart, FrameTiming::Clock::now()));
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
    if (timing) {
        const auto tEnd = FrameTiming::Clock::now();
        using T = FrameTiming;
        const vkr::Renderer::Stats &stats = _renderer->GetStats();
        int readMask = 0;
        for (int i = 0; i < 4; ++i) {
            readMask |= buffers[i] && read.count(buffers[i]) ? 1 << i : 0;
        }
        std::fprintf(timing->csv, "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%u,%u,%d",
                     timing->frame++, T::Ms(timing->start, tBegin), T::Ms(timing->lastBegin, tBegin),
                     T::Ms(timing->lastEnd, tBegin), vkDelegate->CommitMs(), T::Ms(tBegin, tCollect),
                     T::Ms(tCollect, tRender), stats.waitMs, T::Ms(tRender, tRendered), T::Ms(tRendered, tEnd), T::Ms(tBegin, tEnd),
                     timing->fetchUs.exchange(0) / 1000.0, stats.gpuMs, stats.cpuMs, int(moving),
                     int(frame.deferReadback), int(collected), readMask, appWidth, appHeight,
                     int(_renderer->IsConverged()));
        // Eldr's entry points the app called since the last execute; the
        // rest of app_ms is the app's own.
        HdVkCallTimes &calls = HdVkCallTimes::Get();
        double eldrMs = 0;
        for (int e = 0; e < HdVkCallTimes::kCount; ++e) {
            const double ms = calls.ns[e].exchange(0) / 1e6;
            eldrMs += ms;
            std::fprintf(timing->csv, ",%.3f,%lld", ms, (long long)calls.calls[e].exchange(0));
        }
        // The syncs' wall span, then the parts of the meshes' (thread time).
        const int64_t syncBegin = calls.syncBegin.exchange(INT64_MAX), syncEnd = calls.syncEnd.exchange(0);
        std::fprintf(timing->csv, ",%.3f,%.3f", T::Ms(timing->lastEnd, tBegin) - eldrMs,
                     syncEnd > syncBegin ? (syncEnd - syncBegin) / 1e6 : 0.0);
        for (int e = HdVkCallTimes::kCount; e < HdVkCallTimes::kAll; ++e) {
            std::fprintf(timing->csv, ",%.3f", calls.ns[e].exchange(0) / 1e6);
            calls.calls[e] = 0;
        }
        std::fprintf(timing->csv, ",%lld\n", (long long)HdVkTakeShutterHits());
        std::fflush(timing->csv);
        timing->lastBegin = tBegin;
        timing->lastEnd = tEnd;
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
