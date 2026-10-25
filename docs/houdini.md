# Eldr in Houdini 22 (Solaris)

Eldr is standalone; the Houdini build is the same sources compiled against
Houdini's own USD (26.05 in Houdini 22.0.466), because Houdini refuses plugins
built against another USD. The normal build never reads `houdini/houdini.cmake`.

## Build

From a VS 2022 x64 prompt (`vcvars64.bat`), in a separate build directory:

    cmake -S . -B build-h22 -G Ninja -DCMAKE_BUILD_TYPE=Release -DHDVK_HOUDINI=ON ^
          "-DHOUDINI_ROOT=C:/Program Files/Side Effects Software/Houdini 22.0.466"
    cmake --build build-h22 --target hdVk

`houdini/houdini.cmake` loads Houdini's `HoudiniConfig.cmake` and exposes its
`Houdini::Dep::pxr_*` targets under the names the rest of `CMakeLists.txt`
uses (`hd`, `usd`, ...). garch, hgi and hio have no such target and are linked
by path. Houdini has no OpenVDB-as-hioOpenVDB and no OpenImageIO, so volumes
(`HDVK_VOLUMES`) and EXR render products (`HDVK_EXR`) are off in this build.

USD 26.05 vs 26.08: the only difference Eldr hit is
`HdRendererPlugin::IsSupported` (26.05 takes `HdRendererCreateArgs`, 26.08
`HdRendererCreateArgsSchema`), guarded by `HD_API_VERSION >= 103` in
`hydra/rendererPlugin.cpp`. Everything else compiles unchanged.

## Install

The build writes `build-h22/houdini/eldr.json`, a Houdini package that adds
`build-h22/houdini` to `HOUDINI_PATH` (its `UsdRenderers.json` makes Solaris
list the renderer as "Eldr") and appends the build directory to
`PXR_PLUGINPATH_NAME` (finds `plugInfo.json` next to `hdVk.dll`). Either
copy `eldr.json` to `Documents/houdini22.0/packages/`, or set
`HOUDINI_PACKAGE_DIR=<build-h22>\houdini`. Then pick Eldr in the Solaris
viewport's renderer menu or on a Render Settings LOP.

## Tested (Houdini 22.0.466, Apprentice)

- The build succeeds.
- `hython` with the package: `UsdImagingGL.Engine.GetRendererPlugins()` lists
  `HdVkRendererPlugin`, display name "Eldr".
- `hython tests\houdini_render.py tests\scenes\step4.usda /cam out.png`
  (with `PXR_PLUGINPATH_NAME=<build-h22>\`) creates an engine with Eldr and
  renders a correct image of the scene.

## Limited by Apprentice / not verified

- `husk -R HdVkRendererPlugin` (or `-R Eldr`) is refused: "Render engine
  not permitted". Apprentice allows only Karma in husk; this needs Indie.
- The Solaris viewport (GUI) was not tried.
- The Python ctest suite uses `usdenv.cmd` (USD 26.08) and doesn't apply to
  this build.
