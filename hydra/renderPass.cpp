#include "renderPass.h"

#include "renderDelegate.h"
#include "renderer.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cstdio>
#include <exception>

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
    for (const HdRenderPassAovBinding &b : state->GetAovBindings()) {
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
    // Formats are the ones GetDefaultAovDescriptor asks for; a buffer with
    // another format or size is left alone rather than overrun.
    if (!color || color->GetFormat() != HdFormatFloat16Vec4) {
        return;
    }
    frame.width = color->GetWidth();
    frame.height = color->GetHeight();
    auto usable = [&](HdRenderBuffer *b, HdFormat format) {
        return b && b->GetFormat() == format && b->GetWidth() == frame.width &&
               b->GetHeight() == frame.height ? b : nullptr;
    };
    depth = usable(depth, HdFormatFloat32);
    primId = usable(primId, HdFormatInt32);
    instanceId = usable(instanceId, HdFormatInt32);

    const GfMatrix4d view = state->GetWorldToViewMatrix();
    const GfMatrix4d viewProj = view * state->GetProjectionMatrix();
    const GfMatrix4f viewProjF(viewProj), invViewProjF(viewProj.GetInverse());
    std::copy(viewProjF.data(), viewProjF.data() + 16, frame.viewProj);
    std::copy(invViewProjF.data(), invViewProjF.data() + 16, frame.invViewProj);
    HdRenderDelegate *delegate = GetRenderIndex()->GetRenderDelegate();
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
                                {t.translucency, &frame.translucency}, {t.softSunShadows, &frame.softSunShadows},
                                {t.fog, &frame.fog}, {t.fogZUp, &frame.fogZUp}}) {
        *value = delegate->GetRenderSetting<bool>(token, *value);
    }
    for (auto [token, value] : {std::pair{t.aoRadius, &frame.aoRadius}, {t.cullPixels, &frame.cullPixels},
                                {t.exposureCompensation, &frame.post.exposureCompensation},
                                {t.bloomIntensity, &frame.post.bloomIntensity}, {t.fogDensity, &frame.fogDensity},
                                {t.fogHeight, &frame.fogHeight}, {t.fogHeightFalloff, &frame.fogHeightFalloff},
                                {t.fogAnisotropy, &frame.fogAnisotropy}, {t.fogMaxDistance, &frame.fogMaxDistance}}) {
        *value = delegate->GetRenderSetting<float>(token, *value);
    }
    // Colours: "r g b" (usdview edits strings) or a GfVec3f/GfVec3d.
    auto readColor = [&](TfToken const &token, float rgb[3]) {
        const VtValue v = delegate->GetRenderSetting(token);
        if (v.IsHolding<std::string>()) {
            float c[3];
            if (std::sscanf(v.UncheckedGet<std::string>().c_str(), "%f %f %f", &c[0], &c[1], &c[2]) == 3) {
                std::copy(c, c + 3, rgb);
            }
        } else if (v.CanCast<GfVec3f>()) {
            const GfVec3f c = VtValue::Cast<GfVec3f>(v).UncheckedGet<GfVec3f>();
            std::copy(c.data(), c.data() + 3, rgb);
        }
    };
    readColor(t.fogColor, frame.fogColor);
    for (auto [token, value] : {std::pair{t.taaSamples, &frame.taaSamples},
                                {t.shadowResolution, &frame.shadowResolution},
                                {t.shadowCascades, &frame.shadowCascades},
                                {t.localShadowResolution, &frame.localShadowResolution},
                                {t.softShadowSamples, &frame.softShadowSamples}, {t.aoSlices, &frame.aoSlices},
                                {t.ssrSteps, &frame.ssrSteps}, {t.anisotropy, &frame.anisotropy},
                                {t.fogGridSize, &frame.fogGridSize}}) {
        *value = uint32_t(std::max(delegate->GetRenderSetting<int>(token, int(*value)), 1));
    }
    // The scene camera's lens; usdview's free camera has none.
    const GfMatrix4d projection = state->GetProjectionMatrix();
    if (const HdCamera *camera = state->GetCamera()) {
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

    vkr::Renderer::Targets out;
    out.color = color->Map();
    out.depth = depth ? static_cast<float *>(depth->Map()) : nullptr;
    out.primId = primId ? static_cast<int32_t *>(primId->Map()) : nullptr;
    out.instanceId = instanceId ? static_cast<int32_t *>(instanceId->Map()) : nullptr;
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
}

PXR_NAMESPACE_CLOSE_SCOPE
