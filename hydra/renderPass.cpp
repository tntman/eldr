#include "renderPass.h"

#include "renderDelegate.h"
#include "renderer.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <exception>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

HdVkRenderPass::HdVkRenderPass(HdRenderIndex *index, HdRprimCollection const &collection,
                               vkr::Scene &scene, vkr::Renderer *renderer)
    : HdRenderPass(index, collection)
    , _scene(scene)
    , _renderer(renderer)
{
}

bool HdVkRenderPass::IsConverged() const
{
    return !_renderer || _renderer->IsConverged();
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
        _scene.Commit();
    }

    vkr::Renderer::Frame frame;
    HdRenderBuffer *color = nullptr, *depth = nullptr, *primId = nullptr, *instanceId = nullptr;
    // Extra AOVs (vkr::Aovs) by their Hydra names.
    const std::pair<TfToken, vkr::Aovs::Kind> extraNames[] = {
        {HdAovTokens->normal, vkr::Aovs::kNormal},
        {HdAovTokens->Neye, vkr::Aovs::kNormalEye},
        {HdAovTokens->Peye, vkr::Aovs::kPositionEye},
        {HdAovTokens->cameraDepth, vkr::Aovs::kCameraDepth},
        {HdAovTokensMakePrimvar(TfToken("st")), vkr::Aovs::kSt},
        {HdVkAovTokens->albedo, vkr::Aovs::kAlbedo},
        {HdVkAovTokens->emission, vkr::Aovs::kEmission},
        {HdVkAovTokens->cryptoObject, vkr::Aovs::kCryptoObject},
        {HdVkAovTokens->cryptoMaterial, vkr::Aovs::kCryptoMaterial}};
    HdRenderBuffer *extra[vkr::Aovs::kCount] = {};
    for (const HdRenderPassAovBinding &b : state->GetAovBindings()) {
        for (const auto &[name, kind] : extraNames) {
            extra[kind] = b.aovName == name ? b.renderBuffer : extra[kind];
        }
        if (b.aovName == HdAovTokens->color) {
            color = b.renderBuffer;
            if (b.clearValue.IsHolding<GfVec4f>()) {
                const GfVec4f &c = b.clearValue.UncheckedGet<GfVec4f>();
                std::copy(c.data(), c.data() + 4, frame.clearColor);
            }
        } else if (b.aovName == HdAovTokens->depth) {
            depth = b.renderBuffer;
            if (b.clearValue.IsHolding<float>()) {
                frame.clearDepth = b.clearValue.UncheckedGet<float>();
            }
        } else if (b.aovName == HdAovTokens->primId) {
            primId = b.renderBuffer;
        } else if (b.aovName == HdAovTokens->instanceId) {
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
    color = usable(color, HdFormatFloat16Vec4);
    depth = usable(depth, HdFormatFloat32);
    primId = usable(primId, HdFormatInt32);
    instanceId = usable(instanceId, HdFormatInt32);

    HdRenderDelegate *delegate = GetRenderIndex()->GetRenderDelegate();
    for (const auto &[name, kind] : extraNames) {
        extra[kind] = usable(extra[kind], delegate->GetDefaultAovDescriptor(name).format);
    }
    frame.showEnvironment = delegate->GetRenderSetting<bool>(
        HdRenderSettingsTokens->domeLightCameraVisibility, true);
    // Each setting over the renderer's default (renderDelegate.cpp lists
    // them); counts are ints in Hydra, at least 1.
    const auto &t = *HdVkRenderSettingsTokens;
    for (auto [token, value] : {std::pair{t.postProcess, &frame.postProcess}, {t.taa, &frame.taa},
                                {t.ambientOcclusion, &frame.ambientOcclusion}, {t.ssr, &frame.ssr},
                                {t.autoExposure, &frame.post.autoExposure}, {t.shadows, &frame.shadows},
                                {t.localShadows, &frame.localShadows}, {t.softShadows, &frame.softShadows},
                                {t.depthOfField, &frame.depthOfField}, {t.motionBlur, &frame.motionBlur},
                                {t.depthPrepass, &frame.depthPrepass}, {t.frustumCulling, &frame.frustumCulling},
                                {t.localLights, &frame.localLights}, {t.clusteredLights, &frame.clusteredLights},
                                {t.imageBasedLighting, &frame.imageBasedLighting}, {t.normalMaps, &frame.normalMaps},
                                {t.materialGraphs, &frame.materialGraphs}, {t.objectMotion, &frame.objectMotion},
                                {t.translucency, &frame.translucency},
                                {t.lensDistortion, &frame.lensDistortion},
                                {t.blendShapeMotion, &frame.blendShapeMotion},
                                {t.extraAovs, &frame.extraAovs}}) {
        *value = delegate->GetRenderSetting<bool>(token, *value);
    }
    for (auto [token, value] : {std::pair{t.aoRadius, &frame.aoRadius}, {t.cullPixels, &frame.cullPixels},
                                {t.exposureCompensation, &frame.post.exposureCompensation},
                                {t.bloomIntensity, &frame.post.bloomIntensity}}) {
        *value = delegate->GetRenderSetting<float>(token, *value);
    }
    for (auto [token, value] : {std::pair{t.taaSamples, &frame.taaSamples},
                                {t.shadowResolution, &frame.shadowResolution},
                                {t.shadowCascades, &frame.shadowCascades},
                                {t.localShadowResolution, &frame.localShadowResolution},
                                {t.softShadowSamples, &frame.softShadowSamples}, {t.aoSlices, &frame.aoSlices},
                                {t.ssrSteps, &frame.ssrSteps}, {t.anisotropy, &frame.anisotropy}}) {
        *value = uint32_t(std::max(delegate->GetRenderSetting<int>(token, int(*value)), 1));
    }
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
    if (camera && frame.lensDistortion && projection[0][0] > 0.0) {
        vkr::LensDistortion::Params &d = frame.distortion;
        const bool perspective = camera->GetProjection() == HdCamera::Perspective;
        d.fisheye = perspective && camera->GetLensDistortionType() == HdCameraTokens->fisheye;
        d.k1 = camera->GetLensDistortionK1();
        d.k2 = camera->GetLensDistortionK2();
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

    // The scene camera's lens; usdview's free camera has none.
    if (camera) {
        frame.shutter = float(camera->GetShutterClose() - camera->GetShutterOpen());
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

    // Cryptomatte: per primId the ids of its rprim's path and material path,
    // and their manifests (GetRenderStats).
    // ponytail: rebuilt every frame these AOVs are bound; instances share
    // their prototype's id.
    std::vector<uint32_t> cryptomatte;
    if (extra[vkr::Aovs::kCryptoObject] || extra[vkr::Aovs::kCryptoMaterial]) {
        HdRenderIndex *index = GetRenderIndex();
        std::string objects, materials;
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
        static_cast<HdVkRenderDelegate *>(delegate)->SetCryptomatteManifests("{" + objects + "}",
                                                                            "{" + materials + "}");
    }

    vkr::Renderer::Targets out;
    out.color = color ? color->Map() : nullptr;
    out.depth = depth ? static_cast<float *>(depth->Map()) : nullptr;
    out.primId = primId ? static_cast<int32_t *>(primId->Map()) : nullptr;
    out.instanceId = instanceId ? static_cast<int32_t *>(instanceId->Map()) : nullptr;
    for (int k = 0; k < vkr::Aovs::kCount; ++k) {
        out.aovs[k] = extra[k] ? static_cast<float *>(extra[k]->Map()) : nullptr;
    }
    try {
        _renderer->Render(_scene, frame, out);
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk render failed: %s", e.what());
    }
    for (HdRenderBuffer *b : {color, depth, primId, instanceId}) {
        if (b) {
            b->Unmap();
        }
    }
    for (HdRenderBuffer *b : extra) {
        if (b) {
            b->Unmap();
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
