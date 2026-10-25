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
- SOP volumes in Solaris, without files: a SOP Import LOP turns VDB primitives into `OpenVDBAsset` fields and Houdini
  volume primitives into `HoudiniFieldAsset` fields, both with an
  `op:<sop>.sop.volumes:SDF_FORMAT_ARGS:...` `filePath`. Eldr takes the
  `houdiniFieldAsset` Bprim (Houdini's `HD_HoudiniFieldAssetAdapter` makes
  it) as well as `openvdbAsset`, and `houdini/field.cpp` gets the detail
  from `XUSD_LockedGeoRegistry::getGeometry` (key: the layer path without
  `HUSD_Constants::getVolumeSopSuffix()`, plus the format arguments). That
  is the geometry the LOP already cooked and holds while its stage refers
  to it: no cooking from the render thread. A VDB primitive's grid is used
  in place; a Houdini volume is converted with
  `GU_PrimVDB::buildFromPrimVolume`. Both then go through the same NanoVDB
  conversion as files.
- Field numbering follows what SOP Import writes: a `HoudiniFieldAsset`'s
  `fieldIndex` is the primitive number; an `OpenVDBAsset`'s counts the VDB
  primitives named `fieldName` (UsdVol's rule).
- Saved stages: a USD ROP writes these volumes to
  `<layer>.volumes/N.vdb` (`OpenVDBAsset`) and `N.bgeo.sc`
  (`HoudiniFieldAsset`). Eldr reads the `.bgeo.sc` files with
  `GU_Detail::load`, so husk renders saved scenes too.
- Eldr binds fields by name and shades `density` (and `temperature`, see
  docs/features.md). SOP Import names a nameless volume's field `field_0`,
  so give Houdini volumes a `name` (Name SOP, Pyro's `density`).

Limitations:

- `op:` paths only resolve in the Houdini session whose LOP network holds
  the geometry (the Solaris viewport, hython). husk on a USD file that
  still has `op:` paths has no SOP geometry; Eldr warns "no locked SOP
  geometry" and skips the field. Save with a USD ROP instead.
- Scalar fields only: a vector Houdini volume (`vel.x/.y/.z` primitives)
  reads as three float fields; Eldr doesn't use velocity anyway.
- Each Houdini volume is converted on every field sync (a SOP change);
  large dense volumes cost a conversion each time.
- Tf warnings from Eldr don't reach hython's console (Houdini takes Tf
  diagnostics), so a missing field shows up as a missing volume.

## Install

The build writes `build-h22/houdini/eldr.json`, a Houdini package that adds
`build-h22/houdini` to `HOUDINI_PATH` (its `UsdRenderers.json` makes Solaris
list the renderer as "Eldr") and appends the build directory to
`PXR_PLUGINPATH_NAME` (finds `plugInfo.json` next to `hdVk.dll`). Either
copy `eldr.json` to `Documents/houdini22.0/packages/`, or set
`HOUDINI_PACKAGE_DIR=<build-h22>\houdini`. Then pick Eldr in the Solaris
viewport's renderer menu or on a Render Settings LOP.

## Solaris UI (Eldr tabs)

Solaris builds a renderer's tab on the Render Settings LOP from
`<HOUDINI_PATH>/soho/parameters/<RendererPlugin>_Global.ds`, on the Render
Geometry Settings LOP from `..._Geometry.ds` (`loputils.addRendererParmFolders`,
run when the node is created or "Rebuild Renderer-Specific Parameters" is
picked), and the viewport's Display Options from `..._Viewport.ds`
(`uiscope viewport`). For Eldr that is `HdVkRendererPlugin_*.ds`.

`houdini/gen_ui.py` writes the three files into `build-h22/houdini/soho/parameters`
(CMake target `eldrHoudiniUi`, part of the Houdini build, run with hython) from
the renderer's own `GetRendererSettingsList()`, grouped as usdview's settings
window (`GROUPS` in `usdview/hdvkUsdview/settings.py`). The Render Settings tab
authors every `eldr:*` setting (each with Karma's Do Nothing / Set or Create /
Block menu); the Geometry tab the per-prim primvars (`primvars:eldr:reflectionProbe`,
`planarReflection`, `hair`, `ribbons`, `sprites`). `hython tests\houdini_ui.py`
creates both LOPs, sets parameters and checks the authored attributes. Nodes
made before Eldr was installed need "Rebuild Renderer-Specific Parameters".

## Render statistics in Solaris

Houdini reads `GetRenderStats()` (a dictionary, JSON-converted, plus
`__delegate`) and maps it through `houdini/UsdRenderers.json`'s
`statsdatapaths` (jsonpath into the stats) to its standard names. The viewport
text and the Render Statistics pane (`renderstats`, QML templates in
`$HFS/qt/qml/houdini/renderstats`) read these:

| Name | Type | Shown as |
|---|---|---|
| `rendererStage` | string | the word before the percentage in the viewport ("Accumulating 24%") and in the pane |
| `percentDone` | number, 0-100 | the percentage; the pane's progress bar and ETA |
| `totalClockTime` | seconds | the pane's Elapsed (ETA = elapsed / fraction) |
| `totalMemory`, `peakMemory` | bytes | the pane's Memory and Peak Memory |
| `renderProgressAnnotation`, `renderStatsAnnotation` | string | free text lines (Karma uses them for its progress and stats lines) |
| `viewstats` entries | any | the labels listed in `viewstats` ("GPU Time (ms)", "Triangles", ...) |

Eldr reports: `rendererStage` `accumulating` (film), `interactive` (TAA),
`converged`; `percentDone` as the film's samples over target (with adaptive
sampling the share of pixels that stopped, whichever is further), 100 while
interactive (TAA settles in a blink after each camera move, counting it up
would flicker) and when converged; `totalClockTime` as the wall time since the
image last changed, frozen at convergence (the time to converge);
`renderProgressAnnotation` "film 24/64 samples, 12% of pixels done";
`renderStatsAnnotation` "GPU 12.3 ms, 1442 triangles, 1 draws, 0.14 GB". No
Python API reads a delegate's stats; Houdini polls them itself.
`hython` with an offscreen GL context confirms the stats dictionary and the
`statsdatapaths` (through `renderstats.widgets.findStats`).

## Volumes, tested

- `hython tests\houdini_render.py` of `tests\scenes\volume.usda`
  and `volume_material.usda` gives 8-bit images identical to the same
  script run with the normal build under `usdenv.cmd python`. Set
  `PXR_AR_DEFAULT_SEARCH_PATH` to Houdini's
  `bin\usd_plugins\hdx\resources\textures` so `@StinsonBeach.hdr@` resolves.
- husk (needs a licence above Apprentice): `husk -R HdVkRendererPlugin -c /cam
  --res 256 256 -o out.png tests\scenes\volume.usda` renders the volumes.
- SOP volumes: `hython tests\houdini_sop_volumes.py out.png` builds a VDB
  and a Houdini volume in SOPs, imports them with SOP Import (no files) and
  renders the LOP stage with Eldr; it fails unless both volumes show.
- The same network saved by a USD ROP (`.vdb` and `.bgeo.sc` volume files)
  renders both volumes in husk.

## Tested (Houdini 22.0.466, FX licence)

Run husk and hython with `HOUDINI_USER_PREF_DIR` set to an empty folder
(its name must end in `houdini__HVER__`) and `HOUDINI_PACKAGE_DIR` to
`<build-h22>\houdini`: a user-installed `eldr.json` would otherwise load
another checkout's Eldr beside yours.

- The build succeeds, tests included (`testPlugin` builds against 26.05).
- `husk --list-renderers` lists `HdVkRendererPlugin (Eldr)` and
  `husk -R HdVkRendererPlugin` (or `-R Eldr`) renders. step4, glass, lights
  and volume were rendered to EXR with it and with `BRAY_HdKarma` from the
  same Solaris-authored settings (a Karma Render Settings LOP exported with
  `hython`).
- **Render products and AOVs.** husk writes the EXR itself from the AOV
  buffers (Eldr's own writer needs OpenImageIO and is off here). husk binds
  each render var by its own channel name ("C", "depth", "primid") and puts
  UsdRender's `sourceName`/`sourceType` in the binding's settings; Eldr
  resolves the AOV from that source, so a Karma Render Settings LOP's vars
  fill: `C.*[LO]` (beauty, Float32Vec4, converted from half), `C<RD>L`,
  `C<RD>.+L`, `C<RG>L`, `C<RG>.+L`, `C.*O`, `CDA` (albedo), `ray:hitP`,
  `ray:hitPz`, `ray:hitN`. The layer layout of the EXR is the same as
  Karma's (husk's): `C` RGBA, `<var>.R/G/B`, `depth.z`, `N.x/y/z`,
  `P.x/y/z`, `primid.id`, so one comp template works for both.
- **The beauty is linear, unexposed.** When the settings prim is husk's
  (`husk:*` attributes), Eldr renders the film in final quality, gives husk
  the linear beauty (`eldr:productLinear`) and leaves auto exposure off
  unless the prim authors `eldr:autoExposure`. step4's floor: Karma 0.653
  0.650 0.751, Eldr 0.665 0.687 0.729.
- **`eldr:` attributes on the UsdRenderSettings prim** are render settings
  (`custom int eldr:filmSamples = 7`; the type follows the setting's
  default). They apply when the prim's value changes, so they also work with
  `usdrecord --renderSettingsPrimPath`.
- **Several frames** (`husk -f 1 -n 3`, `$F4` in the product name) and motion
  blur over the camera shutter work. Karma's blur is smooth; Eldr's is made
  of discrete time samples (visible steps across a fast move).
- **Lens distortion.** Solaris authors a Karma Physical Lens as a compiled
  string in `karma:camera:lensshader` ("opdef:/Vop/kma_physicallens?VflCode
  ... opencv_k1 0.12 ..."); Eldr reads opencv_k1..k6, p1, p2 from it (and
  still from a bound lens material). Fence posts at x = 0.469 and 0.938
  render at 0.4555 and 0.852 in Eldr and 0.4547 and 0.850 in Karma (k1 .12,
  k2 .02): the same r (x across the width = 1). The EXR's linear beauty is
  warped too (before, only the display image was).
- `hython tests\houdini_render.py ...` renders through UsdImagingGL in
  Houdini's USD, as before.

## Karma comparison (same scenes, husk, Karma CPU)

Within a few percent: step4 (floor, sphere highlight, normals, P, depth: 1-5%),
glass (0.99), the lit walls of lights.usda (1.00-1.05), albedo (0.500 against
0.487), direct diffuse and glossy splits. Mismatches:

| What | Karma | Eldr | Action |
|---|---|---|---|
| Rect-light texture orientation | the file's top row at t = 0, so its light is Eldr's flipped vertically. Seen along the light's axis with +X right and +Y up (corner-marked texture, light 0.25 above a floor), the image corners land at TL/TR/BL/BR as: Karma TR/TL/BR/BL, Eldr BR/BL/TR/TL | st (0,0) at +X +Y with the image's bottom row at t = 0, as UsdUVTexture reads images | None: UsdLux's text gives the corner, not the image flip. Left as is; flagged for the user. |
| `primvars:displayColor` | read as sRGB (0.5 becomes 0.214 linear) | linear, as USD defines colour attributes | None, USD followed. A floor of displayColor 0.5 is 0.43 as bright in Karma. |
| Dome in the camera | not drawn by default | drawn (`domeLightCameraVisibility`, true) | None; Hydra's own setting. |
| Glossy highlights of small lights on a rough 0.15 floor | | up to 2.4x brighter peaks (lights.usda reflections) | Open: Eldr's representative-point specular. |
| `ray:primid` | float id/coverage | Hydra int primId; husk writes it as a float reinterpretation, so it reads 0 | Open: use Cryptomatte-like ids from Karma, not comparable. |
| Cryptomatte vars | rank layers `<name>00..` and manifest metadata (written by Karma) | a plain `<name>` RGB layer, no manifest | Open: Eldr's writer needs OpenImageIO; use the standalone build (usdrecord) for Cryptomatte. |
| Volumes | rendered | rendered (OpenVDB files and SOP volumes, see above) | Matches. |
| Time to converge | step4 33 s, lights 29 s (32 spp) | 10-25 s | |

## Not checked

- Interactive Solaris use beyond a first render: camera moves, picking,
  Render Settings LOP options. The viewport (Houdini VK) does show Eldr's
  image: Houdini hands delegates an OpenGL Hgi but composites with Vulkan, so
  this build reads frames back instead of sharing GL textures.
- The Python ctest suite uses `usdenv.cmd` (USD 26.08) and doesn't apply to
  this build.
