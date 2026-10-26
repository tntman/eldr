#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/material.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vkr {
struct Material;
struct Texture;
struct VolumeShading;
}

PXR_NAMESPACE_OPEN_SCOPE

// Decodes image files with Hio and shares them between materials while any
// material still uses them. CPU only; Scene::Commit uploads.
class HdVkTextureCache
{
public:
    std::shared_ptr<vkr::Texture> Get(std::string const &path, TfToken const &sourceColorSpace);
    // A path with <UDIM>: vkr::kUdimTiles tiles, null where none exists.
    std::vector<std::shared_ptr<vkr::Texture>> GetUdim(std::string const &path,
                                                       TfToken const &sourceColorSpace);

private:
    std::mutex _mutex;
    std::map<std::pair<std::string, TfToken>, std::weak_ptr<vkr::Texture>> _textures;
};

// An unconnected input of `node`: authored (colours converted to linear
// Rec.709), else its node definition's default, else 0; scalars splat.
GfVec4f HdVkParameter(HdMaterialNode2 const &node, TfToken const &input);

// Reads a UsdPreviewSurface, MaterialX standard_surface or OpenPBR network
// into a vkr::Material. The vkr::Material
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
    // Its volume terminal, for the volumes bound to it (defaults without
    // one); updated in place too.
    std::shared_ptr<vkr::VolumeShading> const &Volume() const { return _volume; }
    // The primvar its textures' coordinates come from; empty: no textures.
    TfToken const &UvPrimvar() const { return _uvPrimvar; }
    // The primvars its program reads (vkr::MaterialProgram::primvars).
    std::vector<TfToken> const &Primvars() const { return _primvars; }

private:
    HdVkTextureCache *_textures;
    TfToken _uvPrimvar;
    std::vector<TfToken> _primvars;
    std::shared_ptr<vkr::Material> _material;
    std::shared_ptr<vkr::VolumeShading> _volume;
};

PXR_NAMESPACE_CLOSE_SCOPE
