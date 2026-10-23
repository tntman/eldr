#include "renderPass.h"

#include "renderDelegate.h"
#include "renderer.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/tokens.h"

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
// and the collection/render tags. Fine for usdview; revisit for Houdini (step 7).
void HdVkRenderPass::_Execute(HdRenderPassStateSharedPtr const &state,
                              TfTokenVector const &)
{
    if (!_renderer) {
        return;
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
    frame.postProcess = delegate->GetRenderSetting<bool>(HdVkRenderSettingsTokens->postProcess, true);
    frame.post.autoExposure = delegate->GetRenderSetting<bool>(
        HdVkRenderSettingsTokens->autoExposure, frame.post.autoExposure);
    frame.post.exposureCompensation = delegate->GetRenderSetting<float>(
        HdVkRenderSettingsTokens->exposureCompensation, frame.post.exposureCompensation);
    frame.post.bloomIntensity = delegate->GetRenderSetting<float>(
        HdVkRenderSettingsTokens->bloomIntensity, frame.post.bloomIntensity);
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
