#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/basisCurves.h"
#include "pxr/imaging/hd/points.h"

namespace vkr {
class Scene;
struct Mesh;
}

PXR_NAMESPACE_OPEN_SCOPE

// BasisCurves and Points, drawn as triangles built when they sync: curves
// as tubes (flat ribbons facing their normals when they have normals),
// points as spheres, `widths` across. hdvk:curveRibbons (or a curve's
// primvars:eldr:ribbons) draws curves without normals as camera-facing
// ribbons instead: 2 triangles per polyline step instead of 12.
// ponytail: world-space tubes and spheres, 12 triangles per polyline step
// and 20 per point.
constexpr bool kHdVkCurveRibbons = false;
template <typename Base>
class HdVkTessellated final : public Base
{
public:
    HdVkTessellated(SdfPath const &id, vkr::Scene *scene);
    ~HdVkTessellated() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;
    void UpdateRenderTag(HdSceneDelegate *delegate, HdRenderParam *renderParam) override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &, HdDirtyBits *) override {}

private:
    vkr::Scene *_scene;
    vkr::Mesh *_mesh;
};

using HdVkBasisCurves = HdVkTessellated<HdBasisCurves>;
using HdVkPoints = HdVkTessellated<HdPoints>;

PXR_NAMESPACE_CLOSE_SCOPE
