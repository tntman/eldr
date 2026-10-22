#pragma once

#include "pxr/pxr.h"
#include "pxr/imaging/hd/mesh.h"

namespace vkr {
class Scene;
struct Mesh;
}

PXR_NAMESPACE_OPEN_SCOPE

// Sync() only copies scene data into its vkr::Mesh; no Vulkan here.
class HdVkMesh final : public HdMesh
{
public:
    HdVkMesh(SdfPath const &id, vkr::Scene *scene);
    ~HdVkMesh() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits, TfToken const &reprToken) override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &, HdDirtyBits *) override {}

private:
    vkr::Scene *_scene;
    vkr::Mesh *_mesh;
};

PXR_NAMESPACE_CLOSE_SCOPE
