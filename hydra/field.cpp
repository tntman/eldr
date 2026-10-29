#include "field.h"
#include "renderDelegate.h"

#include "gridReader.h"
#include "scene.h"  // vkr::VolumeGrid

#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#ifndef HDVK_HOUDINI
#include "pxr/imaging/hioOpenVDB/utils.h"
#endif
#include "pxr/usd/sdf/assetPath.h"

// NanoVDB's fallback without TBB runs the conversion on raw std::threads
// (nested, ~100 per grid): an exception in one, a failed thread start
// included, terminates the process (Houdini, playing a VDB sequence).
#ifndef NANOVDB_USE_TBB
#error "build hdVk's volumes with NANOVDB_USE_TBB"
#endif
#include <nanovdb/util/OpenToNanoVDB.h>

#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>

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

// A local file's path, grid and modification time; empty for anything
// else (Houdini's op: SOP geometry, assets that aren't files).
std::string SourceKey(const std::string &path, const std::string &name, int index)
{
    std::error_code error;
    const auto time = std::filesystem::last_write_time(std::filesystem::u8path(path), error);
    return error ? std::string()
                 : path + '\n' + name + '\n' + std::to_string(index) + '\n' +
                       std::to_string(time.time_since_epoch().count());
}

} // namespace

PXR_NAMESPACE_CLOSE_SCOPE

namespace {
std::mutex filesMutex;
std::map<std::string, std::shared_ptr<const void>> files;  // HdVkCachedFile's, until the next commit
} // namespace

std::shared_ptr<const void> HdVkCachedFile(const std::string &path,
                                           const std::function<std::shared_ptr<const void>()> &load)
{
    std::lock_guard<std::mutex> lock(filesMutex);
    std::shared_ptr<const void> &file = files[path];
    if (!file) {
        file = load();
    }
    return file;
}

void HdVkReleaseFiles()
{
    std::lock_guard<std::mutex> lock(filesMutex);
    files.clear();
}

#ifndef HDVK_HOUDINI
openvdb::GridBase::ConstPtr HdVkReadGrid(const std::string &path, const std::string &name, int index, bool)
{
    const auto grids = std::static_pointer_cast<const openvdb::GridPtrVec>(
        HdVkCachedFile(path, [&] { return std::shared_ptr<const void>(PXR_NS::HioOpenVDBGridsFromAsset(path)); }));
    return grids ? HdVkPickGrid(*grids, name, index) : nullptr;
}
#endif

PXR_NAMESPACE_OPEN_SCOPE

HdVkField::HdVkField(SdfPath const &id, std::shared_ptr<vkr::VolumeGrid> grid, bool houdiniField)
    : HdField(id)
    , _grid(std::move(grid))
    , _houdiniField(houdiniField)
{
}

HdVkField::~HdVkField() = default;

void HdVkField::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *, HdDirtyBits *dirtyBits)
{
    HdVkRenderDelegate::NoteSceneEdit(sceneDelegate);
    if (*dirtyBits & DirtyParams) {
        const SdfPath &id = GetId();
        const SdfAssetPath asset = sceneDelegate->Get(id, HdFieldTokens->filePath).GetWithDefault(SdfAssetPath());
        const std::string path = asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
        const TfToken name = sceneDelegate->Get(id, HdFieldTokens->fieldName).GetWithDefault(TfToken());
        const int index = sceneDelegate->Get(id, _tokens->fieldIndex).GetWithDefault(0);
        // The same file, grid and time as the words came from (a field made
        // again, a sequence holding a frame): nothing to read.
        const std::string source = path.empty() ? std::string() : SourceKey(path, name, index);
        if (!source.empty() && source == _grid->source && !_grid->words.empty()) {
            *dirtyBits = Clean;
            return;
        }
        std::vector<uint32_t> words;
        try {
            const openvdb::GridBase::ConstPtr grid =
                path.empty() ? nullptr : HdVkReadGrid(path, name, index, _houdiniField);
            // The same grid object (a SOP volume no edit touched): nothing
            // to convert.
            // ponytail: trusts Houdini's copy-on-write VDBs: a grid edited in
            // place under the same object would stay stale.
            if (grid && grid == _grid->origin.lock() && !_grid->words.empty()) {
                _grid->source = source;
                *dirtyBits = Clean;
                return;
            }
            if (grid) {
                words = ToNano(grid);
            } else {
                TF_WARN("HdVk: <%s>: no grid '%s' (index %d) in '%s'", id.GetText(), name.GetText(), index,
                        path.c_str());
            }
            _grid->origin = grid;
        } catch (const std::exception &e) {
            TF_WARN("HdVk: <%s>: reading '%s' failed: %s", id.GetText(), path.c_str(), e.what());
            words.clear();
            _grid->origin.reset();
        }
        _grid->source = words.empty() ? std::string() : source;
        _grid->words = std::move(words);
        ++_grid->version;
    }
    *dirtyBits = Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
