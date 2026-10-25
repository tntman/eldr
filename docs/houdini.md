# Eldr in Houdini 22 (Solaris)

Eldr is standalone; the Houdini build is the same sources compiled against
Houdini's own USD (26.05 in Houdini 22.0.466), because Houdini refuses plugins
built against another USD. The normal build never reads `houdini/houdini.cmake`
or compiles anything under `houdini/`.

## Build

From a VS 2022 x64 prompt (`vcvars64.bat`), in a separate build directory:

    cmake -S . -B build-h22 -G Ninja -DCMAKE_BUILD_TYPE=Release -DHDVK_HOUDINI=ON ^
          "-DHOUDINI_ROOT=C:/Program Files/Side Effects Software/Houdini 22.0.466"
    cmake --build build-h22 --target hdVk

`houdini/houdini.cmake` loads Houdini's `HoudiniConfig.cmake` and exposes its
`Houdini::Dep::pxr_*` targets under the names the rest of `CMakeLists.txt`
uses (`hd`, `usd`, ...). garch, hgi and hio have no such target and are linked
by path. Houdini's USD has no OpenImageIO, so EXR render products
(`HDVK_EXR`) are off in this build; husk writes its own images instead.

USD 26.05 vs 26.08: the only difference Eldr hit is
`HdRendererPlugin::IsSupported` (26.05 takes `HdRendererCreateArgs`, 26.08
`HdRendererCreateArgsSchema`), guarded by `HD_API_VERSION >= 103` in
`hydra/rendererPlugin.cpp`. Everything else compiles unchanged.

## Volumes

Volumes (`HDVK_VOLUMES`) are on in this build too. Houdini's USD has no
hioOpenVDB, so `houdini/field.cpp` implements the one grid reader
(`HdVkReadGrid`, `hydra/gridReader.h`) with the OpenVDB Houdini ships
(`openvdb_sesi`, OpenVDB 13); the normal build's implementation in
`hydra/field.cpp` uses hioOpenVDB. Both pick grids the same way (UsdVol's
`fieldName`/`fieldIndex`) and share the NanoVDB conversion and the renderer
path. Houdini's NanoVDB headers are 32.6 (the normal build's are 32.4):
`createNanoGrid` replaces `openToNanoVDB`, the grid buffer layout (ABI 32)
is the same, and the GPU reader (`third_party/nanovdb/PNanoVDB.h`, 32.4)
reads it unchanged.

- `OpenVDBAsset` fields with `.vdb` files: as in the normal build.

## Install

The build writes `build-h22/houdini/eldr.json`, a Houdini package that adds
`build-h22/houdini` to `HOUDINI_PATH` (its `UsdRenderers.json` makes Solaris
list the renderer as "Eldr") and appends the build directory to
`PXR_PLUGINPATH_NAME` (finds `plugInfo.json` next to `hdVk.dll`). Either
copy `eldr.json` to `Documents/houdini22.0/packages/`, or set
`HOUDINI_PACKAGE_DIR=<build-h22>\houdini`. Then pick Eldr in the Solaris
viewport's renderer menu or on a Render Settings LOP.

## Tested (Houdini 22.0.466)

- The build succeeds.
- `hython` with the package: `UsdImagingGL.Engine.GetRendererPlugins()` lists
  `HdVkRendererPlugin`, display name "Eldr".
- `hython tests\houdini_render.py tests\scenes\step4.usda /cam out.png`
  (with `PXR_PLUGINPATH_NAME=<build-h22>\`) creates an engine with Eldr and
  renders a correct image of the scene.
- Volumes: `hython tests\houdini_render.py` of `tests\scenes\volume.usda`
  and `volume_material.usda` gives 8-bit images identical to the same
  script run with the normal build under `usdenv.cmd python`. Set
  `PXR_AR_DEFAULT_SEARCH_PATH` to Houdini's
  `bin\usd_plugins\hdx\resources\textures` so `@StinsonBeach.hdr@` resolves.
- husk (needs a licence above Apprentice): `husk -R HdVkRendererPlugin -c /cam
  --res 256 256 -o out.png tests\scenes\volume.usda` renders the volumes.

## Not verified

- The Solaris viewport (GUI) was not tried.
- The Python ctest suite uses `usdenv.cmd` (USD 26.08) and doesn't apply to
  this build.
