#include "camera.h"

#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/sceneDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

void HdVkCamera::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits)
{
    // Before HdCamera::Sync, which cleans the bits.
    if (*dirtyBits & DirtyParams) {
        static const TfToken names[6] = {TfToken("lensDistortion:k3"), TfToken("lensDistortion:k4"),
                                         TfToken("lensDistortion:k5"), TfToken("lensDistortion:k6"),
                                         TfToken("lensDistortion:p1"), TfToken("lensDistortion:p2")};
        for (int i = 0; i < 6; ++i) {
            const VtValue v = sceneDelegate->GetCameraParamValue(GetId(), names[i]);
            _terms[i] = v.IsHolding<float>()    ? v.UncheckedGet<float>()
                      : v.IsHolding<double>() ? float(v.UncheckedGet<double>())
                                              : 0.0f;
        }
    }
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
}

PXR_NAMESPACE_CLOSE_SCOPE
