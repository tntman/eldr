#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/light.h"

namespace vkr {
class Scene;
struct Light;
}

PXR_NAMESPACE_OPEN_SCOPE

// UsdLuxDistantLight (and usdview's camera light, which Hydra hands
// non-Storm renderers as a distantLight).
class HdVkLight final : public HdLight
{
public:
    HdVkLight(SdfPath const &id, vkr::Scene *scene);
    ~HdVkLight() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;

private:
    vkr::Scene *_scene;
    vkr::Light *_light;
};

PXR_NAMESPACE_CLOSE_SCOPE
