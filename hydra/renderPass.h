#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderPass.h"

#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"

#include "renderProducts.h"

#include <map>

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
    HdVkRenderProducts _products;  // UsdRender products (EXR files)
    std::map<TfToken, VtValue> _appliedSettings;  // eldr:* values taken from the settings prim
    float _lastViewProj[16] = {};  // the last frame's camera: moving or still
};

PXR_NAMESPACE_CLOSE_SCOPE
