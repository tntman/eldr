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
// see it without syncing again. The delegate hands a field made again at
// the same path (UsdImaging's legacy delegate remakes fields on a filePath
// change, as Houdini's viewport plays a sequence) the grid the volumes
// already hold. Float and double grids load as float,
// Vec3f and Vec3d ones as Vec3f.
// ponytail: the field prim's own transform is ignored (as in Storm): the
// grid's transform and the volume's place it.
class HdVkField final : public HdField
{
public:
    // houdiniField: a houdiniFieldAsset (Houdini build), whose fieldIndex is
    // a primitive number.
    HdVkField(SdfPath const &id, std::shared_ptr<vkr::VolumeGrid> grid, bool houdiniField = false);
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
