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
    // Sampled motion: the camera's transform one frame before, as its
    // samples at shutter open and close continue linearly; false when it
    // doesn't move through its shutter (or sampled motion is off).
    bool GetPrevTransform(GfMatrix4d *prev) const
    {
        *prev = _prevTransform;
        return _moving;
    }

private:
    float _terms[6] = {};
    GfMatrix4d _prevTransform{1.0};
    bool _moving = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
