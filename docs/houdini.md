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
| Volumes | rendered | not in this build (no OpenVDB reader): volume.usda shows no cloud | By design, see Build. |
| Time to converge | step4 33 s, lights 29 s (32 spp) | 10-25 s | |

## Not checked

- The Solaris viewport (GUI): hython cannot create a viewer. The renderer
  menu entry comes from `UsdRenderers.json`; husk lists it.
- The Python ctest suite uses `usdenv.cmd` (USD 26.08) and doesn't apply to
  this build.
