#pragma once

#include <openvdb/openvdb.h>

#include <functional>
#include <memory>
#include <string>

// A field's grid: the index-th grid named name (of any name when name is
// empty) at path, or null. Throws on unreadable files. Implemented in
// hydra/field.cpp through USD's hioOpenVDB, or in the Houdini build
// (HDVK_HOUDINI) by houdini/field.cpp through Houdini's own OpenVDB, where
// houdiniField (a HoudiniFieldAsset) makes index a primitive number.
openvdb::GridBase::ConstPtr HdVkReadGrid(const std::string &path, const std::string &name, int index,
                                         bool houdiniField);

// What `load` makes of file `path`, shared by every field that syncs
// until the delegate's next commit (HdVkReleaseFiles, field.h): a pyro
// frame's density, temperature and vel fields read the file once.
std::shared_ptr<const void> HdVkCachedFile(const std::string &path,
                                           const std::function<std::shared_ptr<const void>()> &load);

// UsdVol's fieldIndex: the index-th of grids named name (all when empty).
inline openvdb::GridBase::ConstPtr HdVkPickGrid(const openvdb::GridPtrVec &grids, const std::string &name, int index)
{
    for (const openvdb::GridBase::Ptr &grid : grids) {
        if ((name.empty() || grid->getName() == name) && index-- == 0) {
            return grid;
        }
    }
    return nullptr;
}
