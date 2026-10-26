# Eldr in Houdini 22 (Solaris)

Eldr is the same sources compiled against Houdini's own USD (26.05 in Houdini 22.0.466); Houdini refuses plugins built against another USD. The normal build never reads `houdini/houdini.cmake`.

## Build

From a VS 2022 x64 prompt (`vcvars64.bat`), in a separate build directory:

```
cmake -S . -B build-h22 -G Ninja -DCMAKE_BUILD_TYPE=Release -DHDVK_HOUDINI=ON ^
      "-DHOUDINI_ROOT=C:/Program Files/Side Effects Software/Houdini 22.0.466"
cmake --build build-h22 --target hdVk
```

| Difference from the normal build | Detail |
|---|---|
| Targets | `houdini/houdini.cmake` maps Houdini's `Houdini::Dep::pxr_*` targets to the usual names; garch, hgi, hio are linked by path |
| EXR products | Off (`HDVK_EXR`): Houdini's USD has no OpenImageIO; husk writes the images |
| USD 26.05 vs 26.08 | Only `HdRendererPlugin::IsSupported` differs, guarded by `HD_API_VERSION >= 103` |
| Volumes | On; `houdini/field.cpp` reads grids with Houdini's OpenVDB (`openvdb_sesi`) instead of hioOpenVDB. NanoVDB 32.6 headers, same GPU layout. |

## Install

The build writes `build-h22/houdini/eldr.json`, a Houdini package. It adds `build-h22/houdini` to `HOUDINI_PATH` (its `UsdRenderers.json` lists the renderer as "Eldr" in Solaris) and the build directory to `PXR_PLUGINPATH_NAME`, and Houdini's `$HFS/bin/usd_plugins/hdx/resources/textures` to `PXR_AR_DEFAULT_SEARCH_PATH`, as `usdenv.cmd` does for USD's: the test scenes' `@StinsonBeach.hdr@` domes resolve in Solaris and husk, for Karma too. Without it the dome falls back to white and its reflections and lighting with it (`tests/houdini_dome.py` checks this headless).

1. Copy `eldr.json` to `Documents/houdini22.0/packages/`, or set `HOUDINI_PACKAGE_DIR=<build-h22>\houdini`.
2. Pick Eldr in the Solaris viewport's renderer menu or on a Render Settings LOP.

## Solaris UI

`houdini/gen_ui.py` (CMake target `eldrHoudiniUi`, run with hython) writes the parameter files into `build-h22/houdini/soho/parameters` from the renderer's settings list, grouped as in usdview's settings window.

| File | Where it shows |
|---|---|
| `HdVkRendererPlugin_Global.ds` | Render Settings LOP tab: every `eldr:*` setting, each with Karma's Do Nothing / Set or Create / Block menu |
| `HdVkRendererPlugin_Geometry.ds` | Render Geometry Settings LOP tab: per-prim primvars (`primvars:eldr:reflectionProbe`, `planarReflection`, `hair`, `ribbons`, `sprites`) |
| `HdVkRendererPlugin_Viewport.ds` | Viewport Display Options |

Nodes made before Eldr was installed need **Rebuild Renderer-Specific Parameters**.

## Render statistics

Houdini reads `GetRenderStats()` through `houdini/UsdRenderers.json` (`statsdatapaths`) for the viewport text and the Render Statistics pane.

| Name | Shown as |
|---|---|
| `rendererStage` | Word before the percentage ("Accumulating 24%") |
| `percentDone` (0-100) | Percentage, progress bar, ETA |
| `totalClockTime` (s) | Elapsed; frozen at convergence |
| `totalMemory`, `peakMemory` (bytes) | Memory, Peak Memory |
| `renderProgressAnnotation`, `renderStatsAnnotation` | Free text lines |
| `viewstats` entries | Labels like "GPU passes: Shading (ms)"; all of usdview's Render Statistics are included, bytes raw |

| `rendererStage` | Meaning |
|---|---|
| `final` | Film in final quality (`eldr:finalQuality`, or a fresh film as in husk and usdrecord) |
| `accumulating` | Film started from the interactive image |
| `interactive` | TAA (`percentDone` 100) |
| `converged` | Done |

`percentDone` is film samples over target (with adaptive sampling, the share of stopped pixels, whichever is further). Annotations read like "final 24/64 samples, 12% of pixels done" and "GPU 12.3 ms, 1442 triangles, 1 draws, 0.14 GB". A pass that did not run has a blank row.

## Volumes

| Source | Support |
|---|---|
| `OpenVDBAsset` with `.vdb` files | As in the normal build |
| SOP volumes in Solaris (SOP Import LOP, no files) | VDB primitives become `OpenVDBAsset`, Houdini volumes `HoudiniFieldAsset`, with `op:<sop>.sop.volumes:...` paths. Eldr reads the geometry the LOP already cooked; Houdini volumes convert with `GU_PrimVDB::buildFromPrimVolume`. |
| Saved stages (USD ROP) | Volumes go to `<layer>.volumes/N.vdb` and `N.bgeo.sc`; husk renders them |

- Field numbering follows SOP Import: a `HoudiniFieldAsset`'s `fieldIndex` is the primitive number; an `OpenVDBAsset`'s counts VDB primitives named `fieldName`.
- Eldr binds fields by name and shades `density` and `temperature`. SOP Import names a nameless volume's field `field_0`: give volumes a `name` (Name SOP, Pyro's `density`).

| Limitation | Detail |
|---|---|
| `op:` paths | Resolve only in the Houdini session holding the LOP network. husk on a USD file with `op:` paths warns "no locked SOP geometry" and skips the field: save with a USD ROP. |
| Vector volumes | `vel.x/.y/.z` read as three float fields |
| Conversion cost | Each Houdini volume converts on every field sync |
| Diagnostics | Tf warnings don't reach hython's console; a missing field shows as a missing volume |

## husk, AOVs and Cryptomatte

Run husk with `-R HdVkRendererPlugin` (or `-R Eldr`); `husk --list-renderers` shows `HdVkRendererPlugin (Eldr)`. husk needs a licence above Apprentice.

```
husk -R HdVkRendererPlugin -c /cam --res 256 256 -o out.png tests\scenes\volume.usda
```

| Topic | Behaviour |
|---|---|
| AOVs | husk writes the EXR from Eldr's buffers. Eldr resolves each var from UsdRender's `sourceName`/`sourceType`, so a Karma Render Settings LOP's vars fill: `C.*[LO]` (beauty), `C<RD>L`, `C<RD>.+L`, `C<RG>L`, `C<RG>.+L`, `C.*O`, `CDA`, `ray:hitP`, `ray:hitPz`, `ray:hitN`. |
| EXR layers | Same as Karma's: `C` RGBA, `<var>.R/G/B`, `depth.z`, `N.x/y/z`, `P.x/y/z`, `primid.id`, so one comp template serves both |
| Cryptomatte | Layers `<name>00`, `01`, `02` (6 ranks, soft edges from the film) with manifest metadata, as Karma. Always three layers whatever `cryptomatterank`. |
| `ray:primid` | Float: 0 off objects, Hydra primId + 1 on objects (no coverage). usdview and usdrecord keep Int32 for picking. |
| Product size | When the product's framing differs from husk's buffers (`husk --res`), Eldr renders at the product's framing and resamples (nearest) into all buffers |
| Beauty | Under husk (it sets `rasterRenderProducts`), whatever the settings prim: final-quality film, linear unexposed beauty (`eldr:productLinear`), auto exposure off unless `eldr:autoExposure` is authored |
| `eldr:` attributes | On the UsdRenderSettings prim they are render settings (`custom int eldr:filmSamples = 7`); they also work with `usdrecord --renderSettingsPrimPath` |
| Sequences | `husk -f 1 -n 3` with `$F4` in the product name works; husk waits for each frame to converge. Motion blur over the camera shutter is smooth (`eldr:motionBlurResidual`). |
| Lens distortion | A Karma Physical Lens in `karma:camera:lensshader` (or bound lens material): `opencv_k1..k6`, `p1`, `p2` are read; fence posts land within 0.002 of Karma. The EXR beauty is warped too. |

## Eldr vs Karma (same scenes, husk)

Within a few percent: step4 floor, highlight, normals, P, depth (1-5%); glass (0.99); lights.usda walls (1.00-1.05); albedo (0.500 vs 0.487); direct diffuse and glossy splits.

step4 with StinsonBeach.hdr loaded in both (husk, 320x180, linear, Eldr's auto exposure off): floor strip 0.665/0.687/0.730 vs Karma 0.653/0.651/0.749, the chrome ball's sky reflection 0.018/0.034/0.073 vs 0.017/0.033/0.072, gold ball centre 0.43 vs 0.50 red (-13%), pillar -13%.

| What | Karma | Eldr |
|---|---|---|
| Rect-light texture orientation | File's top row at +Y, left edge at +X | Same |
| `primvars:displayColor` | Read as sRGB (0.5 = 0.214 linear) | Linear, as USD defines; a 0.5 floor is 0.43 as bright in Karma |
| Dome in the camera | Not drawn by default | Drawn (`domeLightCameraVisibility`, Hydra's setting) |
| Small-light highlights on rough floors | Colour limit 20 clips by 10%; with it off, within 4% of a GGX integral | Within 5% of Karma |
| `ray:primid` | Id with coverage | Id + 1, no coverage |
| Cryptomatte | `<name>00..02` + manifest | Same layout |
| Volumes | Rendered | Rendered (OpenVDB files and SOP volumes) |
| Time to converge | step4 33 s, lights 29 s (32 spp) | 10-25 s |

### ColorChecker, Eldr vs Karma (linear EXR, no tonemap)

`tests/scenes/macbeth.usda` is a ColorChecker Classic (BabelColor 2005 average, D50 xyY Bradford-adapted to D65, linear Rec.709; flat diffuse, roughness 1, specular 0; cyan clamped, it is outside Rec.709) with a `light` variant set: `hdri` (StinsonBeach.hdr), `white` (uniform dome, intensity 1), `distant`, `rect`. `tests/macbeth_compare.py --exrinfo <exrinfo>` renders each with Eldr and husk (Karma CPU) to linear EXRs and prints per patch the albedo, Eldr, Karma and their ratio, plus a CIEDE2000 summary (Lab white = Karma's white patch). Eldr is forced to `eldr:tonemapper` none, `eldr:autoExposure` off, `eldr:exposureCompensation` 0, `eldr:bloomIntensity` 0, `eldr:productLinear` on. ctest `macbeth` checks the white dome (radiance = albedo) for Eldr alone.

| Lighting | Eldr / Karma | dE2000 Eldr vs Karma (mean, worst) | Against albedo |
|---|---|---|---|
| white dome | 1.004-1.005 | 0.09, 0.11 | Eldr within 0.4%, Karma 0.4% low |
| distant | 1.003-1.005 | 0.08, 0.10 | |
| rect | 1.03 (dark red-ish) to 1.04 (white): Eldr brighter | 0.77, 0.95 | |
| hdri | R 0.96, G 1.02, B 0.90 | 2.72, 7.28 (white patch) | Both load the HDRI (white patch -58% vs the white dome) |

With the HDRI Karma's sky is bluer: Eldr's blue is 10% lower on every patch, red 4% lower, green 2% higher. Not yet explained (candidates: Eldr's dome filtering or lat-long orientation).

Display transforms are separate. Eldr's colour AOV in the viewport is ACES tonemapped (`eldr:tonemapper` aces, auto exposure on), display-referred but still linear; the host applies the sRGB or OCIO transfer on top. The same scene through usdrecord (default settings) reads, after decoding sRGB: white patch 0.906 -> 0.672, neutral 5 0.192 -> 0.171, black 0.032 -> 0.010, orange red channel 0.747 -> 0.658. So a viewport or PNG can never be compared with the linear EXR numbers above; in Houdini an ACES view on top would tonemap twice (not measured).

## Solaris viewport

| Topic | Behaviour |
|---|---|
| Readback | Houdini gives an OpenGL Hgi but composites with Vulkan, so shared GL textures show nothing: Eldr reads frames back. Houdini binds colour (Float16Vec4), depth, normal, primvars:st, primId, instanceId. A buffer returns only if Houdini mapped it since the last frame; while the camera moves, unread extra AOVs are skipped. `HDVK_DEBUG_AOVS=1` prints the bindings. |
| Render Settings LOP | Looking through one changes only what its Eldr tab sets. Houdini's viewport is told by `houdini:viewport` or, when a Render Settings LOP frame arrives without it (`houdini:renderer = husk`), by `houdini:interactive`: the frame stays interactive (no husk mode, the film never starts "final", even after a camera switch) and the products' resolution doesn't apply. A missing camera (the default `/cameras/camera1`) keeps the viewport's camera. `tests/houdini_render_settings.py` checks this headless. |
| Diagnostics | `HDVK_DEBUG_AOVS=1` also prints every non-`eldr:` setting the host sets (`houdini:viewport`, `renderCameraPath`, husk's `rasterRenderProducts`) |
| Depth, navigation, guides | Houdini's tumble pivot, pan speed and its own light and camera guides (drawn over the image, depth-tested) read the depth AOV. As Karma's it is linear (`depthstyle` `linear`): distance along the view axis, 0 where nothing was hit, whatever near and far planes Houdini assumes. The depth buffer always clears to the far plane: husk binds depth with clear value 0, which used to leave the frame empty. husk's `depth` var matches Karma's within 2% on step4. `viewportBench --nav` (ctest `viewport_nav`) checks the standalone build's window depth back-projects onto the scene, also when mapped frames later and after the film converged. |
| Still viewport | Houdini's free camera is a scene prim (`/__HoudiniFreeCamera__`) that may sync again unchanged; only real camera, lens, scene or setting changes start the film over, so a still view converges and stops changing. `HDVK_DEBUG_AOVS=1` prints every 60 frames how many frames moved the camera or changed the scene, and the near and far planes. |
| Colour | Linear scene colour, the same values as the EXR's linear beauty (`eldr:productLinear`, on): no tonemap, bloom or auto exposure of Eldr's own (auto exposure only if the settings prim authors `eldr:autoExposure`; `eldr:exposureCompensation` applies). The display transform is Houdini's, from its own settings and environment (OCIO config, view, gamma, LUT), as for Karma; Houdini 22's default view is Un-tone-mapped (sRGB). Eldr never reads `$OCIO`. `eldr:productLinear` off brings back Eldr's ACES and auto exposure, under Houdini's view. Before, Houdini's view applied over Eldr's ACES and auto exposure: ACES alone took step4's floor from 0.67 linear to 0.48, and auto exposure darkened it further under a bright (white fallback) dome. |
| Selection outline | `UsdRenderers.json` sets `"needsselection": true`; Houdini draws the outline itself |
| Ids | primId is the render index's prim id, instanceId the instance index, as Houdini's picking expects |
| Frame times | 1280x720, moving camera. Kitchen_set: colour, depth, ids read 23.7 ms; colour and depth 20.8 ms; all six 32.1 ms. step4 with ids: 12.3 ms. usdview (GPU sharing): about 15 ms (Kitchen_set), 11 ms (step4). |

## Not checked

- Interactive Solaris use in the GUI: navigation speed, the outline, Render Settings LOP options.
- The Python ctest suite uses `usdenv.cmd` (USD 26.08) and doesn't apply to this build.
