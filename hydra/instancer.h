#pragma once

#include "pxr/pxr.h"
#include "pxr/base/vt/types.h"
#include "pxr/imaging/hd/instancer.h"

#include <map>
#include <vector>

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
    // Instance primvar `name` for each instance of `prototypeId`, in
    // ComputeInstanceTransforms' order (a parent instancer's value repeats
    // over its child's instances, the child's own wins), `components` floats
    // each. False if no level has it.
    bool ComputeInstancePrimvar(SdfPath const &prototypeId, TfToken const &name, std::vector<float> *values,
                                uint32_t *components);

private:
    std::map<TfToken, VtValue> _primvars;
    bool _visible = true;
};

// A numeric primvar value (a scalar or vector, or an array of them) as
// floats, `components` (1..4) per element. False for anything else.
bool HdVkToFloats(VtValue const &value, std::vector<float> *out, uint32_t *components);

// World transforms of an rprim's instances (16 floats each, USD row-vector
// order): its own transform, then each instance's.
std::vector<float> HdVkWorldTransforms(HdSceneDelegate *delegate, SdfPath const &id,
                                       SdfPath const &instancerId);

PXR_NAMESPACE_CLOSE_SCOPE
