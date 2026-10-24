#include "field.h"

#include "scene.h"  // vkr::VolumeGrid

#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hioOpenVDB/utils.h"
#include "pxr/usd/sdf/assetPath.h"

#include <nanovdb/util/OpenToNanoVDB.h>

#include <cstring>
#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(_tokens, (fieldIndex));

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

std::vector<uint32_t> ToNano(const openvdb::GridBase::Ptr &base)
{
    using nanovdb::openToNanoVDB;
    if (const auto grid = openvdb::GridBase::grid<openvdb::FloatGrid>(base)) {
        return Words(openToNanoVDB(*grid));
    }
    if (const auto grid = openvdb::GridBase::grid<openvdb::DoubleGrid>(base)) {
        return Words(openToNanoVDB(*Cast<openvdb::FloatTree>(*grid)));
    }
    if (const auto grid = openvdb::GridBase::grid<openvdb::Vec3fGrid>(base)) {
        return Words(openToNanoVDB(*grid));
    }
    if (const auto grid = openvdb::GridBase::grid<openvdb::Vec3dGrid>(base)) {
        return Words(openToNanoVDB(*Cast<openvdb::Vec3fTree>(*grid)));
    }
    TF_WARN("HdVk: grid '%s' is a %s grid; volumes read float and vec3 grids", base->getName().c_str(),
            base->valueType().c_str());
    return {};
}

} // namespace

HdVkField::HdVkField(SdfPath const &id)
    : HdField(id)
    , _grid(std::make_shared<vkr::VolumeGrid>())
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
        // UsdVol: fieldIndex picks among the file's grids named fieldName
        // (all of them when it has none).
        std::vector<uint32_t> words;
        try {
            int match = 0;
            const openvdb::GridPtrVecPtr grids = path.empty() ? nullptr : HioOpenVDBGridsFromAsset(path);
            for (const openvdb::GridBase::Ptr &grid : grids ? *grids : openvdb::GridPtrVec()) {
                if ((name.IsEmpty() || grid->getName() == name.GetString()) && match++ == index) {
                    words = ToNano(grid);
                    break;
                }
            }
            if (words.empty() && match <= index) {
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
