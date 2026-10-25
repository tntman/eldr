// HdVkReadGrid for the Houdini build: Houdini's USD has no hioOpenVDB, so
// .vdb files are read with the OpenVDB Houdini ships (openvdb_sesi).
#include "gridReader.h"

#include <openvdb/io/File.h>

openvdb::GridBase::ConstPtr HdVkReadGrid(const std::string &path, const std::string &name, int index)
{
    openvdb::initialize();  // idempotent; Houdini has usually done it already
    openvdb::io::File file(path);
    file.open();
    const openvdb::GridBase::ConstPtr grid = HdVkPickGrid(*file.getGrids(), name, index);
    file.close();
    return grid;
}
