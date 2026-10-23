#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/volume.h"

namespace vkr {
class Scene;
struct Volume;
}

PXR_NAMESPACE_OPEN_SCOPE

// UsdVolVolume: its field bindings by name (density, temperature, Cd or
// color, emission; without a density binding the first field is the
// density) to openvdbAsset fields (HdVkField), raymarched by vkr's
// VolumePass.
// ponytail: the volume's material isn't read (no field-reader remapping,
// fixed albedo and phase from the render settings), and volumes aren't
// pickable.
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
