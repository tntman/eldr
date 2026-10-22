#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/light.h"

namespace vkr {
class Scene;
struct Dome;
struct Light;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdVkTextureCache;

// UsdLuxDistantLight (also usdview's camera light, which Hydra hands
// non-Storm renderers as a distantLight) and UsdLuxDomeLight.
class HdVkLight final : public HdLight
{
public:
    HdVkLight(SdfPath const &id, TfToken const &type, vkr::Scene *scene,
              HdVkTextureCache *textures);
    ~HdVkLight() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;

private:
    vkr::Scene *_scene;
    HdVkTextureCache *_textures;
    vkr::Light *_light = nullptr;  // distant
    vkr::Dome *_dome = nullptr;    // dome
};

PXR_NAMESPACE_CLOSE_SCOPE
