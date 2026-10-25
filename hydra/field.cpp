#include "field.h"

#include "gridReader.h"
#include "scene.h"  // vkr::VolumeGrid

#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#ifndef HDVK_HOUDINI
#include "pxr/imaging/hioOpenVDB/utils.h"
#endif
#include "pxr/usd/sdf/assetPath.h"

#include <nanovdb/util/OpenToNanoVDB.h>

#include <cstring>
#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(_tokens, (fieldIndex));

// NanoVDB 32.6 (Houdini 22's) replaced openToNanoVDB(grid) by createNanoGrid.
template <typename Grid>
auto OpenToNano(const Grid &grid)
{
#ifdef NANOVDB_CREATE_NANOGRID_H_HAS_BEEN_INCLUDED
    return nanovdb::createNanoGrid(grid);
#else
    return nanovdb::openToNanoVDB(grid);
#endif
}

// The grid's NanoVDB bytes, or nothing (with a warning) for types the
// volumes don't read.
template <typename Handle>
std::vector<uint32_t> Words(const Handle &handle)
{
    std::vector<uint32_t> words(handle.size() / 4);
    std::memcpy(words.data(), handle.data(), words.size() * 4);
    return words;
}

// A grid with its values cast to another tree's type (double to float).
template <typename Tree, typename Grid>
typename openvdb::Grid<Tree>::Ptr Cast(const Grid &grid)
{
    const auto out = openvdb::Grid<Tree>::create(std::make_shared<Tree>(grid.tree()));
    out->setTransform(grid.transform().copy());
    return out;
}

std::vector<uint32_t> ToNano(const openvdb::GridBase::ConstPtr &base)
{
    if (const auto grid = openvdb::GridBase::constGrid<openvdb::FloatGrid>(base)) {
        return Words(OpenToNano(*grid));
    }
    if (const auto grid = openvdb::GridBase::constGrid<openvdb::DoubleGrid>(base)) {
        return Words(OpenToNano(*Cast<openvdb::FloatTree>(*grid)));
    }
    if (const auto grid = openvdb::GridBase::constGrid<openvdb::Vec3fGrid>(base)) {
        return Words(OpenToNano(*grid));
    }
    if (const auto grid = openvdb::GridBase::constGrid<openvdb::Vec3dGrid>(base)) {
        return Words(OpenToNano(*Cast<openvdb::Vec3fTree>(*grid)));
    }
    TF_WARN("HdVk: grid '%s' is a %s grid; volumes read float and vec3 grids", base->getName().c_str(),
            base->valueType().c_str());
    return {};
}

} // namespace

PXR_NAMESPACE_CLOSE_SCOPE

#ifndef HDVK_HOUDINI
openvdb::GridBase::ConstPtr HdVkReadGrid(const std::string &path, const std::string &name, int index, bool)
{
    const openvdb::GridPtrVecPtr grids = PXR_NS::HioOpenVDBGridsFromAsset(path);
    return grids ? HdVkPickGrid(*grids, name, index) : nullptr;
}
#endif

PXR_NAMESPACE_OPEN_SCOPE

HdVkField::HdVkField(SdfPath const &id, bool houdiniField)
    : HdField(id)
    , _grid(std::make_shared<vkr::VolumeGrid>())
    , _houdiniField(houdiniField)
{
}

HdVkField::~HdVkField() = default;

void HdVkField::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *, HdDirtyBits *dirtyBits)
{
    if (*dirtyBits & DirtyParams) {
        const SdfPath &id = GetId();
        const SdfAssetPath asset = sceneDelegate->Get(id, HdFieldTokens->filePath).GetWithDefault(SdfAssetPath());
        const std::string path = asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
        const TfToken name = sceneDelegate->Get(id, HdFieldTokens->fieldName).GetWithDefault(TfToken());
        const int index = sceneDelegate->Get(id, _tokens->fieldIndex).GetWithDefault(0);
        std::vector<uint32_t> words;
        try {
            const openvdb::GridBase::ConstPtr grid =
                path.empty() ? nullptr : HdVkReadGrid(path, name, index, _houdiniField);
            if (grid) {
                words = ToNano(grid);
            } else {
                TF_WARN("HdVk: <%s>: no grid '%s' (index %d) in '%s'", id.GetText(), name.GetText(), index,
                        path.c_str());
            }
        } catch (const std::exception &e) {
            TF_WARN("HdVk: <%s>: reading '%s' failed: %s", id.GetText(), path.c_str(), e.what());
            words.clear();
        }
        _grid->words = std::move(words);
        ++_grid->version;
    }
    *dirtyBits = Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
