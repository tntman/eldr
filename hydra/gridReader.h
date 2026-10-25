#pragma once

#include <openvdb/openvdb.h>

#include <string>

// A field's grid: the index-th grid named name (of any name when name is
// empty) at path, or null. Throws on unreadable files. Implemented in
// hydra/field.cpp through USD's hioOpenVDB, or in the Houdini build
// (HDVK_HOUDINI) by houdini/field.cpp through Houdini's own OpenVDB.
openvdb::GridBase::ConstPtr HdVkReadGrid(const std::string &path, const std::string &name, int index);

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
