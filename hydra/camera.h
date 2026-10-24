#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/camera.h"

PXR_NAMESPACE_OPEN_SCOPE

// HdCamera with the lens distortion terms of OpenCV's model it lacks: the
// camera params lensDistortion:k3 .. k6, p1, p2 (a USD camera's custom
// attributes of those names, or a Karma lens material's, see
// karmaLensAdapter.cpp), read the way HdCamera reads k1 and k2.
class HdVkCamera final : public HdCamera
{
public:
    explicit HdVkCamera(SdfPath const &id) : HdCamera(id) {}

    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) override;

    // k3, k4, k5, k6, p1, p2 (0: none).
    const float *GetLensDistortionOpenCvTerms() const { return _terms; }

private:
    float _terms[6] = {};
};

PXR_NAMESPACE_CLOSE_SCOPE
