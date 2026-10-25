#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/field.h"

#include <memory>

namespace vkr {
struct VolumeGrid;
}

PXR_NAMESPACE_OPEN_SCOPE

// An openvdbAsset field (UsdVolOpenVDBAsset): one grid of a .vdb file
// (filePath, fieldName, fieldIndex) converted to NanoVDB for the GPU. The
// Houdini build also makes them of houdiniFieldAsset fields (houdini/field.cpp).
// Volumes hold Grid(); a change of the field reloads it in place, so they
// see it without syncing again. Float and double grids load as float,
// Vec3f and Vec3d ones as Vec3f.
// ponytail: the field prim's own transform is ignored (as in Storm): the
// grid's transform and the volume's place it.
class HdVkField final : public HdField
{
public:
    // houdiniField: a houdiniFieldAsset (Houdini build), whose fieldIndex is
    // a primitive number.
    explicit HdVkField(SdfPath const &id, bool houdiniField = false);
    ~HdVkField() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;

    const std::shared_ptr<vkr::VolumeGrid> &Grid() const { return _grid; }

private:
    std::shared_ptr<vkr::VolumeGrid> _grid;
    bool _houdiniField;
};

PXR_NAMESPACE_CLOSE_SCOPE
