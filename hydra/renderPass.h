#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderPass.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"

#include "renderProducts.h"
#include "subdivision.h"

#include <array>
#include <cstdint>
#include <map>
#include <chrono>
#include <optional>

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
    SdfPath _lastCameraId;  // the camera the last frame looked through (a switch: no camera motion)
    // eldr:overlapReadback: the buffers (colour, depth, ids) and size last
    // frame read back into, and its depth AOV's constants.
    std::array<uintptr_t, 6> _lastReadback = {};
    std::array<float, 4> _lastDepthConstants = {};
    bool _lastDeferred = false;  // last frame was left to Collect
    bool _extraStale = false;    // unread extra AOVs were left out of the last frame (eldr:lazyExtraAovs)
    bool _failed = false;        // the last frame's Render threw: not converged, no products
    std::optional<std::chrono::steady_clock::time_point> _lastMove;  // the camera last moved (eldr:stillDelay)
    bool _holding = false;       // stopped less than eldr:stillDelay ago: counts as moving
    bool _redicing = false;      // eldr:screenDicing: the meshes re-dice for the last frame's view
    std::optional<HdVkDicingView> _lastDicing;  // the last frame's view (still or moving)
};

PXR_NAMESPACE_CLOSE_SCOPE
