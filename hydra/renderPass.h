#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderPass.h"

namespace vkr {
class Renderer;
class Scene;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdVkRenderPass final : public HdRenderPass
{
public:
    HdVkRenderPass(HdRenderIndex *index, HdRprimCollection const &collection,
                   vkr::Scene &scene, vkr::Renderer *renderer);

    // Not converged while auto-exposure adapts, so the app keeps drawing.
    bool IsConverged() const override;

protected:
    void _Execute(HdRenderPassStateSharedPtr const &renderPassState,
                  TfTokenVector const &renderTags) override;

private:
    vkr::Scene &_scene;
    vkr::Renderer *_renderer;
};

PXR_NAMESPACE_CLOSE_SCOPE
