// Karma's lens distortion for Eldr's cameras. Solaris embeds the lens in the
// camera: karma:camera:lensshader holds the Karma Physical Lens VOP as
// "opdef:/Vop/kma_physicallens?VflCode <parm value> ..." (read below). Houdini's Karma also binds a lens
// material to a camera (karma:camera:material:binding, used when
// karma:camera:use_lensshader is on); its Karma Physical Lens shader
// (kma_physicallens) holds OpenCV's model: opencv_enabled, opencv_k1..k6,
// opencv_p1, opencv_p2. UsdImaging doesn't pass relationships to Hydra, so
// this keyless API schema adapter (every prim; only cameras answer) reads
// them and hands them to the camera as lensDistortion:k1 .. p2, which
// HdVkCamera and HdCamera read. Only names are shared with Houdini: nothing
// of it is linked or needed.
// ponytail: edits to the lens material reach the camera with its next
// change (no dependency on the material); the shader is the first prim
// under the material with an opencv_enabled input.
#include "pxr/pxr.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/type.h"
#include "pxr/imaging/hd/cameraSchema.h"
#include "pxr/imaging/hd/lensDistortionSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usdImaging/usdImaging/apiSchemaAdapter.h"
#include "pxr/usdImaging/usdImaging/dataSourceAttribute.h"
#include "pxr/usdImaging/usdImaging/dataSourceStageGlobals.h"

#include <cstdlib>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((useLensShader, "karma:camera:use_lensshader"))
    ((lensMaterial, "karma:camera:material:binding"))
    ((lensShader, "karma:camera:lensshader"))
    ((karmaCamera, "karma:camera:"))
    ((enabled, "inputs:opencv_enabled")));

// An int or bool attribute's value now.
bool IsOn(const UsdAttribute &attr, UsdTimeCode time)
{
    VtValue v;
    if (!attr || !attr.Get(&v, time)) {
        return false;
    }
    return v.IsHolding<bool>() ? v.UncheckedGet<bool>() : v.IsHolding<int>() ? v.UncheckedGet<int>() != 0 : false;
}

} // namespace

class HdVkKarmaLensAdapter : public UsdImagingAPISchemaAdapter
{
public:
    using BaseAdapter = UsdImagingAPISchemaAdapter;

    HdContainerDataSourceHandle GetImagingSubprimData(UsdPrim const &prim, TfToken const &subprim,
                                                      TfToken const &appliedInstanceName,
                                                      const UsdImagingDataSourceStageGlobals &stageGlobals) override
    {
        if (!subprim.IsEmpty() || !prim.IsA<UsdGeomCamera>() ||
            !IsOn(prim.GetAttribute(_tokens->useLensShader), stageGlobals.GetTime())) {
            return nullptr;
        }
        // What Solaris authors: the lens VOP compiled into a string,
        // "opdef:/Vop/kma_physicallens?VflCode <parm value> ...", with the
        // parameters that aren't at their defaults.
        std::string code;
        if (prim.GetAttribute(_tokens->lensShader).Get(&code, stageGlobals.GetTime()) &&
            code.compare(0, 27, "opdef:/Vop/kma_physicallens") == 0) {
            const auto term = [&](const char *name, float *value) {
                const std::string key = std::string(" ") + name + " ";
                const size_t at = code.find(key);
                *value = at == std::string::npos ? 0.0f : std::strtof(code.c_str() + at + key.size(), nullptr);
                return at != std::string::npos;
            };
            float on = 0.0f;
            if (!term("opencv_enabled", &on) || on == 0.0f) {
                return nullptr;
            }
            std::vector<TfToken> names = {HdLensDistortionSchemaTokens->type};
            std::vector<HdDataSourceBaseHandle> values = {
                HdRetainedTypedSampledDataSource<TfToken>::New(HdLensDistortionSchemaTokens->standard)};
            for (const char *name : {"k1", "k2", "k3", "k4", "k5", "k6", "p1", "p2"}) {
                float value;
                if (term((std::string("opencv_") + name).c_str(), &value)) {
                    names.push_back(TfToken(name));
                    values.push_back(HdRetainedTypedSampledDataSource<float>::New(value));
                }
            }
            return HdRetainedContainerDataSource::New(
                HdCameraSchemaTokens->camera,
                HdRetainedContainerDataSource::New(
                    HdLensDistortionSchemaTokens->lensDistortion,
                    HdRetainedContainerDataSource::New(names.size(), names.data(), values.data())));
        }
        SdfPathVector targets;
        const UsdRelationship binding = prim.GetRelationship(_tokens->lensMaterial);
        if (!binding || !binding.GetForwardedTargets(&targets) || targets.empty()) {
            return nullptr;
        }
        const UsdPrim material = prim.GetStage()->GetPrimAtPath(targets[0]);
        if (!material) {
            return nullptr;
        }
        for (const UsdPrim &shader : UsdPrimRange(material)) {
            if (!shader.HasAttribute(_tokens->enabled)) {
                continue;
            }
            if (!IsOn(shader.GetAttribute(_tokens->enabled), stageGlobals.GetTime())) {
                return nullptr;
            }
            static const std::pair<const char *, TfToken> terms[8] = {
                {"inputs:opencv_k1", HdLensDistortionSchemaTokens->k1}, {"inputs:opencv_k2", HdLensDistortionSchemaTokens->k2},
                {"inputs:opencv_k3", TfToken("k3")}, {"inputs:opencv_k4", TfToken("k4")},
                {"inputs:opencv_k5", TfToken("k5")}, {"inputs:opencv_k6", TfToken("k6")},
                {"inputs:opencv_p1", TfToken("p1")}, {"inputs:opencv_p2", TfToken("p2")}};
            std::vector<TfToken> names = {HdLensDistortionSchemaTokens->type};
            std::vector<HdDataSourceBaseHandle> values = {
                HdRetainedTypedSampledDataSource<TfToken>::New(HdLensDistortionSchemaTokens->standard)};
            const HdDataSourceLocator locator = HdLensDistortionSchema::GetDefaultLocator();
            for (const auto &[input, name] : terms) {
                if (const UsdAttribute attr = shader.GetAttribute(TfToken(input))) {
                    names.push_back(name);
                    values.push_back(UsdImagingDataSourceAttributeNew(attr, stageGlobals, prim.GetPath(),
                                                                      locator.Append(name)));
                }
            }
            return HdRetainedContainerDataSource::New(
                HdCameraSchemaTokens->camera,
                HdRetainedContainerDataSource::New(
                    HdLensDistortionSchemaTokens->lensDistortion,
                    HdRetainedContainerDataSource::New(names.size(), names.data(), values.data())));
        }
        return nullptr;
    }

    HdDataSourceLocatorSet InvalidateImagingSubprim(UsdPrim const &prim, TfToken const &subprim,
                                                    TfToken const &appliedInstanceName, TfTokenVector const &properties,
                                                    UsdImagingPropertyInvalidationType invalidationType) override
    {
        for (const TfToken &p : properties) {
            if (TfStringStartsWith(p.GetString(), _tokens->karmaCamera.GetString())) {
                return HdDataSourceLocatorSet(HdLensDistortionSchema::GetDefaultLocator());
            }
        }
        return HdDataSourceLocatorSet();
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    using Adapter = HdVkKarmaLensAdapter;
    TfType t = TfType::Define<Adapter, TfType::Bases<Adapter::BaseAdapter>>();
    t.SetFactory<UsdImagingAPISchemaAdapterFactory<Adapter>>();
}

PXR_NAMESPACE_CLOSE_SCOPE
