#include "renderPass.h"

#include "renderer.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/imaging/hd/renderBuffer.h"
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

// ponytail: renders the whole buffer and ignores the framing's data window
// and the collection/render tags. Fine for usdview; revisit for Houdini (step 7).
void HdVkRenderPass::_Execute(HdRenderPassStateSharedPtr const &state,
                              TfTokenVector const &)
{
    if (!_renderer) {
        return;
    }

    vkr::Renderer::Frame frame;
    HdRenderBuffer *color = nullptr, *depth = nullptr;
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
        }
    }
    // Formats are the ones GetDefaultAovDescriptor asks for.
    if (!color || color->GetFormat() != HdFormatFloat16Vec4) {
        return;
    }
    frame.width = color->GetWidth();
    frame.height = color->GetHeight();
    if (depth && (depth->GetFormat() != HdFormatFloat32 ||
                  depth->GetWidth() != frame.width || depth->GetHeight() != frame.height)) {
        depth = nullptr;
    }

    const GfMatrix4d view = state->GetWorldToViewMatrix();
    const GfMatrix4f viewProj(view * state->GetProjectionMatrix());
    std::copy(viewProj.data(), viewProj.data() + 16, frame.viewProj);
    const GfVec3d eye = view.GetInverse().ExtractTranslation();
    frame.cameraPos[0] = float(eye[0]);
    frame.cameraPos[1] = float(eye[1]);
    frame.cameraPos[2] = float(eye[2]);

    void *colorData = color->Map();
    float *depthData = depth ? static_cast<float *>(depth->Map()) : nullptr;
    try {
        _renderer->Render(_scene, frame, colorData, depthData);
    } catch (const std::exception &e) {
        TF_RUNTIME_ERROR("HdVk render failed: %s", e.what());
    }
    color->Unmap();
    if (depth) {
        depth->Unmap();
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
