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

// UsdLux distant (also usdview's camera light, which Hydra hands non-Storm
// renderers as a distantLight), dome, sphere, rect, disk and cylinder lights.
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
    TfToken _type;
    vkr::Scene *_scene;
    HdVkTextureCache *_textures;
    vkr::Light *_light = nullptr;  // all but dome
    vkr::Dome *_dome = nullptr;    // dome
};

PXR_NAMESPACE_CLOSE_SCOPE
