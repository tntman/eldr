#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderPass.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"

#include "renderProducts.h"

#include <array>
#include <cstdint>
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
    GfMatrix4d _lastView, _lastProjection;  // the last real camera's (a missing camera keeps it)
    bool _hasLastCamera = false;
    // eldr:overlapReadback: the buffers (colour, depth, ids) and size last
    // frame read back into, and its depth AOV's constants.
    std::array<uintptr_t, 6> _lastReadback = {};
    std::array<float, 4> _lastDepthConstants = {};
    bool _lastDeferred = false;  // last frame was left to Collect
    bool _extraStale = false;    // unread extra AOVs were left out of the last frame (eldr:lazyExtraAovs)
    bool _failed = false;        // the last frame's Render threw: not converged, no products
};

PXR_NAMESPACE_CLOSE_SCOPE
