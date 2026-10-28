#include "camera.h"
#include "renderDelegate.h"
#include "timing.h"

#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/timeSampleArray.h"

PXR_NAMESPACE_OPEN_SCOPE

void HdVkCamera::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits)
{
    HdVkTimedCall timed(HdVkCallTimes::kCameraSync);
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
    const HdDirtyBits bits = *dirtyBits;
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
    if (!(bits & (DirtyTransform | DirtyParams))) {
        return;
    }
    auto *delegate = static_cast<HdVkRenderDelegate *>(sceneDelegate->GetRenderIndex().GetRenderDelegate());
    const float open = float(GetShutterOpen()), close = float(GetShutterClose());
    delegate->NoteCameraShutter(GetId(), open, close);
    if (getenv("HDVK_DEBUG_MOTION")) {  // TEMP
        fprintf(stderr, "MOTION camera %s shutter %g %g\n", GetId().GetText(), open, close);
    }
    _moving = false;
    // Houdini's viewport camera (/__HoudiniFreeCamera__) is where the user
    // looks from, not a shot camera: its samples can come from other frames
    // than the current one, and blurring the whole view along them smeared
    // it more with every film sample. Objects still blur under it.
    const bool viewportCamera = TfStringStartsWith(GetId().GetName(), "__HoudiniFreeCamera");
    float a, b;
    if (!viewportCamera && delegate->SampledMotionShutter(&a, &b) && close > open) {
        HdTimeSampleArray<GfMatrix4d, 4> samples;
        sceneDelegate->SampleTransform(GetId(), open, close, &samples);
        if (samples.count > 1) {
            const GfMatrix4d at = samples.Resample(open), bt = samples.Resample(close);
            _moving = at != bt;
            _prevTransform = GetTransform() - (bt - at) * (1.0 / double(close - open));
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
