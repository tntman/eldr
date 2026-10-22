#pragma once

#include "pxr/pxr.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/material.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace vkr {
struct Material;
struct Texture;
}

PXR_NAMESPACE_OPEN_SCOPE

// Decodes image files with Hio and shares them between materials while any
// material still uses them. CPU only; Scene::Commit uploads.
class HdVkTextureCache
{
public:
    std::shared_ptr<vkr::Texture> Get(std::string const &path, TfToken const &sourceColorSpace);

private:
    std::mutex _mutex;
    std::map<std::pair<std::string, TfToken>, std::weak_ptr<vkr::Texture>> _textures;
};

// Reads a UsdPreviewSurface network into a vkr::Material. The vkr::Material
// object stays the same for the sprim's lifetime (meshes hold it), so
// re-syncs update it in place.
class HdVkMaterial final : public HdMaterial
{
public:
    HdVkMaterial(SdfPath const &id, HdVkTextureCache *textures);

    HdDirtyBits GetInitialDirtyBitsMask() const override { return AllDirty; }
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;

    std::shared_ptr<vkr::Material> const &Get() const { return _material; }

private:
    HdVkTextureCache *_textures;
    std::shared_ptr<vkr::Material> _material;
};

PXR_NAMESPACE_CLOSE_SCOPE
