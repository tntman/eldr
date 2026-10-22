#pragma once

#include "pxr/pxr.h"
#include "pxr/base/vt/types.h"
#include "pxr/imaging/hd/instancer.h"

#include <map>

PXR_NAMESPACE_OPEN_SCOPE

// Point instancers and native instancing. Port of hdEmbree's instancer:
// rprims call ComputeInstanceTransforms() for their own prototype.
class HdVkInstancer final : public HdInstancer
{
public:
    HdVkInstancer(HdSceneDelegate *delegate, SdfPath const &id);

    void Sync(HdSceneDelegate *delegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;

    // Instancer-to-world transforms for each instance of `prototypeId`,
    // nested (parent) instancers flattened in.
    VtMatrix4dArray ComputeInstanceTransforms(SdfPath const &prototypeId);

private:
    std::map<TfToken, VtValue> _primvars;
    bool _visible = true;
};

PXR_NAMESPACE_CLOSE_SCOPE
