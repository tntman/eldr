#include "renderPass.h"

#include "camera.h"
#include "glInterop.h"
#include "renderBuffer.h"
#include "renderDelegate.h"
#include "renderer.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/stringUtils.h"
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
        _scene.Commit();
    }

    vkr::Renderer::Frame frame;
    HdRenderBuffer *color = nullptr, *depth = nullptr, *primId = nullptr, *instanceId = nullptr;
    // Extra AOVs (vkr::Aovs) by their Hydra names (HdVkExtraAovs).
    // ponytail: one buffer per kind; of two names for the same (normal and
    // ray:hitN) bound at once, the last one gets it.
    HdRenderBuffer *extra[vkr::Aovs::kCount] = {};
    for (const HdRenderPassAovBinding &b : state->GetAovBindings()) {
        for (const HdVkExtraAov &aov : HdVkExtraAovs()) {
            extra[aov.kind] = b.aovName == aov.name ? b.renderBuffer : extra[aov.kind];
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
    // Any float format will do: it gets that many components.
    uint8_t extraComponents[vkr::Aovs::kCount] = {};
    for (int k = 0; k < vkr::Aovs::kCount; ++k) {
        const HdFormat format = extra[k] ? extra[k]->GetFormat() : HdFormatInvalid;
        extra[k] = HdGetComponentFormat(format) == HdFormatFloat32 ? usable(extra[k], format) : nullptr;
        extraComponents[k] = extra[k] ? uint8_t(HdGetComponentCount(format)) : 0;
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
                                {t2.ssrDenoise, &frame.ssrDenoise}}) {
        *value = delegate->GetRenderSetting<bool>(token, *value);
    }
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
                                {t.filmFilterRadius, &frame.filmFilterRadius}, {t2.renderScale, &frame.renderScale},
                                {t.adaptiveThreshold, &frame.adaptiveThreshold},
                                {t2.directClamp, &frame.directClamp}, {t2.indirectClamp, &frame.indirectClamp}}) {
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
                                {t.filmSamples, &frame.filmSamples},
                                {t.interactiveSamples, &frame.interactiveSamples},
                                {t.adaptiveMinSamples, &frame.adaptiveMinSamples}}) {
        *value = uint32_t(std::max(delegate->GetRenderSetting<int>(token, int(*value)), 1));
    }
    // 0 allowed: no skinned instance blocks rays.
    frame.rtSkinnedInstances =
        uint32_t(std::max(delegate->GetRenderSetting<int>(t.rtSkinnedInstances, int(frame.rtSkinnedInstances)), 0));
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
    frame.finalQuality = frame.finalQuality ||
                         !delegate->GetRenderSetting<bool>(HdRenderSettingsTokens->enableInteractive, true);
    // Any edit to the scene or the settings starts the film over.
    frame.sceneVersion = (uint64_t(GetRenderIndex()->GetChangeTracker().GetSceneStateVersion()) << 32) |
                         delegate->GetRenderSettingsVersion();
    frame.toonBands = uint32_t(std::max(delegate->GetRenderSetting<int>(t.toonBands, int(frame.toonBands)), 0));
    if (_scene.SetEmissiveLights(frame.emissiveLights, frame.emissiveLightThreshold)) {
        _scene.Commit();
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

    // GPU sharing: colour and depth go to the app's OpenGL as textures, and
    // none of the four is read back unless someone maps it.
    auto *vkDelegate = static_cast<HdVkRenderDelegate *>(delegate);
    HdVkGLInterop *interop = frame.shareFrame && color ? vkDelegate->GLInterop() : nullptr;
    const bool lazy = interop != nullptr;
    HdVkRenderBuffer *const buffers[4] = {static_cast<HdVkRenderBuffer *>(color), static_cast<HdVkRenderBuffer *>(depth),
                                          static_cast<HdVkRenderBuffer *>(primId),
                                          static_cast<HdVkRenderBuffer *>(instanceId)};
    vkr::Renderer::Targets out;
    if (!lazy) {
        out.color = color ? color->Map() : nullptr;
        out.depth = depth ? static_cast<float *>(depth->Map()) : nullptr;
        out.primId = primId ? static_cast<int32_t *>(primId->Map()) : nullptr;
        out.instanceId = instanceId ? static_cast<int32_t *>(instanceId->Map()) : nullptr;
    }
    for (int k = 0; k < vkr::Aovs::kCount; ++k) {
        out.aovs[k] = extra[k] ? static_cast<float *>(extra[k]->Map()) : nullptr;
        out.aovComponents[k] = extraComponents[k];
    }
    // UsdRender products (renderProducts.h) of the active render settings.
    _products.Prepare(frame.renderProducts ? vkDelegate->ActiveRenderSettings() : nullptr, frame.width, frame.height,
                      &out);

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
    try {
        _products.Write(*_renderer, objects, materials);
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk render products failed: %s", e.what());
    }
    for (int i = 0; i < 4; ++i) {
        if (!buffers[i]) {
            continue;
        }
        buffers[i]->SetTexture(shared && i == 0 ? interop->Color() : shared && i == 1 ? interop->Depth()
                                                                                    : HgiTextureHandle());
        if (!lazy) {
            buffers[i]->Unmap();
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
    for (HdRenderBuffer *b : extra) {
        if (b) {
            b->Unmap();
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
