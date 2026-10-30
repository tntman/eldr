#pragma once

#include "pxr/pxr.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/token.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/imaging/hd/material.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
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
    // `compress`: 8-bit textures block-compressed (vkr::CompressTexture),
    // cached on disk; materials pass `compress` below, lights false (the
    // dome is read on the CPU).
    std::shared_ptr<vkr::Texture> Get(std::string const &path, TfToken const &sourceColorSpace,
                                      bool compress = false);
    // A path with <UDIM>: vkr::kUdimTiles tiles, null where none exists.
    std::vector<std::shared_ptr<vkr::Texture>> GetUdim(std::string const &path,
                                                       TfToken const &sourceColorSpace, bool compress = false);

    // eldr:textureCompression, set by the render delegate.
    std::atomic<bool> compress{true};

    // Milliseconds spent decoding since the last call (the load summary).
    double TakeLoadMs()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const double ms = _loadMs;
        _loadMs = 0;
        return ms;
    }

private:
    std::mutex _mutex;
    double _loadMs = 0;
    std::map<std::tuple<std::string, TfToken, bool>, std::weak_ptr<vkr::Texture>> _textures;
};

// An unconnected input of `node`: authored (colours converted to linear
// Rec.709), else its node definition's default, else 0; scalars splat.
GfVec4f HdVkParameter(HdMaterialNode2 const &node, TfToken const &input);

// A Houdini ramp parameter as Karma's kma_rampconst holds it: keys, values
// (colours with alpha 1, floats splat) and a basis per key (the segment
// after it), sorted by key. Evaluated on the CPU (material programs and
// volumes bake tables of it).
struct HdVkRamp {
    std::vector<float> keys;
    std::vector<GfVec4f> values;
    std::vector<TfToken> basis;

    // kma_rampconst's fkeys/fvalues/fbasis (vkeys/... with `color`), the
    // first framp/vramp keys where that count is authored.
    static HdVkRamp Read(HdMaterialNode2 const &node, bool color);
    // At t, as Houdini's ramps (hou.Ramp.lookup, checked to 1e-6): the end
    // values outside the keys; constant, linear, uniform Catmull-Rom (end
    // keys repeated) and monotone cubic (flat at the end keys) between two
    // keys; bezier and bspline (uniform, end keys tripled) over each run of
    // keys with that basis as a curve through (key, value) control points.
    GfVec4f operator()(float t) const;
};

// What materials' shading graphs fell back from, by material: inputs with a
// node bypassed or dropped (or an unknown surface: 1), and those nodes as
// "node (id)". The render stats' materialFallback* (renderDelegate.cpp).
class HdVkMaterialFallbacks
{
public:
    void Set(SdfPath const &material, int inputs, std::vector<std::string> nodes);
    int Inputs() const;
    // "material: node (id)", comma separated, the first `max` of them.
    std::string Nodes(size_t max) const;

private:
    mutable std::mutex _mutex;
    std::map<SdfPath, std::pair<int, std::vector<std::string>>> _byMaterial;
};

// Reads a UsdPreviewSurface, MaterialX standard_surface, OpenPBR, glTF PBR,
// Disney principled or surface_unlit network
// into a vkr::Material. The vkr::Material
// object stays the same for the sprim's lifetime (meshes hold it), so
// re-syncs update it in place.
class HdVkMaterial final : public HdMaterial
{
public:
    HdVkMaterial(SdfPath const &id, HdVkTextureCache *textures, HdVkMaterialFallbacks *fallbacks);
    ~HdVkMaterial() override;

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
    HdVkMaterialFallbacks *_fallbacks;
    TfToken _uvPrimvar;
    std::vector<TfToken> _primvars;
    std::shared_ptr<vkr::Material> _material;
    std::shared_ptr<vkr::VolumeShading> _volume;
};

PXR_NAMESPACE_CLOSE_SCOPE
