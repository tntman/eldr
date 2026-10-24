#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/volume.h"

namespace vkr {
class Scene;
struct Volume;
struct VolumeShading;
}

PXR_NAMESPACE_OPEN_SCOPE

struct HdMaterialNetwork2;

// A material's volume terminal (MaterialX volume shading) into `shading`;
// leaves it alone without one. See volume.cpp.
void HdVkReadVolumeShading(HdMaterialNetwork2 const &net, vkr::VolumeShading *shading);

// UsdVolVolume: its field bindings by name to openvdbAsset fields
// (HdVkField) and its material's volume shading (HdVkMaterial::Volume(),
// which picks the fields by role: density, temperature, Cd, emission by
// default), raymarched by vkr's VolumePass.
// ponytail: volumes aren't pickable.
class HdVkVolume final : public HdVolume
{
public:
    HdVkVolume(SdfPath const &id, vkr::Scene *scene);
    ~HdVkVolume() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;
    void UpdateRenderTag(HdSceneDelegate *delegate, HdRenderParam *renderParam) override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &, HdDirtyBits *) override {}

private:
    vkr::Scene *_scene;
    vkr::Volume *_volume;
};

PXR_NAMESPACE_CLOSE_SCOPE
