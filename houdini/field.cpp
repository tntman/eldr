// HdVkReadGrid for the Houdini build: Houdini's USD has no hioOpenVDB, so
// .vdb files are read with the OpenVDB Houdini ships (openvdb_sesi). Fields
// whose filePath is Houdini geometry come from it as well: SOP volumes in
// Solaris ("op:<sop>.sop.volumes:SDF_FORMAT_ARGS:..." paths, for
// HoudiniFieldAsset and for OpenVDBAsset) and .bgeo files.
#include "gridReader.h"

#include <GEO/GEO_PrimVDB.h>
#include <GEO/GEO_PrimVolume.h>
#include <GU/GU_Detail.h>
#include <GU/GU_PrimVDB.h>
#include <HUSD/HUSD_Constants.h>
#include <HUSD/XUSD_LockedGeoRegistry.h>
#include <UT/UT_StringHolder.h>

#include <openvdb/io/File.h>

#include "pxr/usd/sdf/layer.h"

#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// A field of a detail as an OpenVDB grid, numbered as SOP Import writes
// them: a HoudiniFieldAsset's fieldIndex is its primitive number; an
// OpenVDBAsset's counts the VDB primitives named fieldName (UsdVol's rule,
// as if they were a .vdb file's grids). A VDB's grid is shared, not copied;
// a Houdini volume is converted to a float VDB.
// ponytail: scalar fields only; vector Houdini volumes (vel.x/.y/.z prims)
// load as three float fields.
openvdb::GridBase::ConstPtr GridFromDetail(const GU_Detail &gdp, const std::string &name, int index,
                                           bool houdiniField)
{
    if (houdiniField) {
        const GEO_Primitive *prim = index >= 0 && index < gdp.getNumPrimitives()
                                        ? gdp.getGEOPrimitiveByIndex(index)
                                        : nullptr;
        if (const auto *vdb = dynamic_cast<const GEO_PrimVDB *>(prim)) {
            return vdb->getConstGridPtr();
        }
        const auto *volume = dynamic_cast<const GEO_PrimVolume *>(prim);
        GU_Detail scratch;
        const GU_PrimVDB *vdb = volume ? GU_PrimVDB::buildFromPrimVolume(scratch, *volume, name.c_str()) : nullptr;
        return vdb ? vdb->getConstGridPtr() : nullptr;  // the grid outlives scratch (shared)
    }
    for (GA_Offset off : gdp.getPrimitiveRange()) {
        const auto *vdb = dynamic_cast<const GEO_PrimVDB *>(gdp.getGEOPrimitive(off));
        if (vdb && (name.empty() || name == vdb->getGridName()) && index-- == 0) {
            return vdb->getConstGridPtr();
        }
    }
    return nullptr;
}

// The SOP geometry a LOP locked for an op: path, or an empty handle. No
// cooking: the stage's LOP holds the geometry while the stage refers to it.
// The registry's key is the layer path minus the volume suffix (".volumes":
// "op:/obj/geo1/OUT.sop") and the same file format arguments.
GU_ConstDetailHandle SopGeometry(const std::string &path)
{
    std::string layer;
    SdfLayer::FileFormatArguments args;
    if (!SdfLayer::SplitIdentifier(path, &layer, &args)) {
        return {};
    }
    const std::string suffix = HUSD_Constants::getVolumeSopSuffix().toStdString();
    if (layer.size() > suffix.size() && layer.compare(layer.size() - suffix.size(), suffix.size(), suffix) == 0) {
        layer.resize(layer.size() - suffix.size());
    }
    return XUSD_LockedGeoRegistry::getGeometry(UT_StringHolder(layer), args);
}

} // namespace

openvdb::GridBase::ConstPtr HdVkReadGrid(const std::string &path, const std::string &name, int index,
                                         bool houdiniField)
{
    openvdb::initialize();  // idempotent; Houdini has usually done it already
    if (path.rfind("op:", 0) == 0) {
        GU_ConstDetailHandle gdh = SopGeometry(path);
        const GU_DetailHandleAutoReadLock gdp(gdh);
        if (!gdp.getGdp()) {
            throw std::runtime_error("no locked SOP geometry (only a live Houdini session has it)");
        }
        return GridFromDetail(*gdp.getGdp(), name, index, houdiniField);
    }
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".vdb") == 0) {
        const auto grids = std::static_pointer_cast<const openvdb::GridPtrVec>(HdVkCachedFile(path, [&] {
            openvdb::io::File file(path);
            file.open();
            std::shared_ptr<const void> all(file.getGrids());
            file.close();
            return all;
        }));
        return HdVkPickGrid(*grids, name, index);
    }
    // .bgeo(.sc) and the other geometry formats Houdini reads.
    const auto gdp = std::static_pointer_cast<const GU_Detail>(HdVkCachedFile(path, [&] {
        auto loaded = std::make_shared<GU_Detail>();
        if (!loaded->load(path.c_str()).success()) {
            throw std::runtime_error("Houdini can't load it");
        }
        return std::shared_ptr<const void>(std::move(loaded));
    }));
    return GridFromDetail(*gdp, name, index, houdiniField);
}
