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

The build writes `build-h22/houdini/eldr.json`, a Houdini package. It adds `build-h22/houdini` to `HOUDINI_PATH` (its `UsdRenderers.json` lists the renderer as "Eldr" in Solaris) and the build directory to `PXR_PLUGINPATH_NAME`.

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
| rect | 1.000-1.003 (was 1.03-1.04 before the exact form factor) | 0.05, 0.10 | Both within 0.5% of the analytic irradiance |
| hdri | R 0.96, G 1.02, B 0.90 | 2.72, 7.28 (white patch) | Both load the HDRI (white patch -58% vs the white dome). Eldr within 0.2% of the brute-force integral, Karma 4% R, -2% G, 11% B off it (below) |

The rect light was 3-4% bright in Eldr: its diffuse used the sphere approximation of the horizon-clipped form factor, which overestimates a rect wholly above the horizon. It now clips the rect to the horizon and takes Lambert's exact form factor; both renderers are within 0.5% of the analytic irradiance (numerical integral over the rect).

With the HDRI the difference is Karma's. A brute-force cosine-weighted sum over StinsonBeach.hdr's texels (UsdLux lat-long mapping) for the chart's normal gives E/pi = 0.388, 0.418, 0.474; Eldr's white patch / albedo is 0.387, 0.417, 0.473, Karma's 0.402, 0.409, 0.525. Synthetic smooth lat-longs (one per axis, the dome rotated 90 degrees) match the brute force in both renderers within 1%, so orientation and colour handling agree. The gap is all in the sun (35 texels over luminance 10): the HDR without them matches in Karma within 1%, the sun alone is 5% red, -3% green, 19% blue off. Karma is biased on hot spots a few texels wide: a uniform 5x5-texel block on black loses 10% in Karma (the same block at 4x the resolution, same solid angle, 5%), independent of samples (128 or 1024) and colour limit; StinsonBeach upsampled 4x (nearest) moves Karma to 1.015, 0.986, 1.043 of the brute force. Eldr (cube from 4x4-supersampled bilinear lookups, SH9 irradiance, the sun extracted as a distant light) stays within 0.2% of the brute force.

Rough metals differ by model, not lighting: Eldr compensates GGX multiple scattering (Fdez-Aguera's split-sum form for image lights, Filament's lobe scale for lights), Karma's UsdPreviewSurface is single scattering (a white furnace gives Karma exactly the single-scatter albedo, with alpha = roughness^2). Gold (F0 1, 0.78, 0.34) facing the camera: roughness 0.2 matches within 0.2%, 0.45 is 4-6% brighter in Eldr, 0.8 50-80% brighter (direct lights: the lobe scale; image lights: the energy Karma loses). In step4, the pillar (-13%) and the gold ball's centre (-18%) are interreflections Eldr leaves out by default: with `eldr:gi`, `eldr:reflectionProbe`, `eldr:rtShadows` and `eldr:rtReflections` on, the pillar is 1.03, 1.05, 0.97 of Karma (the HDRI pattern above) and the gold centre 1.12, 1.10, 0.98 (multiple scattering). Not explained yet: the gold ball's sun highlight sits 2-3 pixels higher in Eldr (320x180), so a box on its upper flank reads 50-70% brighter while the column through the ball sums to 0.9 of Karma's (Eldr draws spheres as Hydra's tessellated mesh; suspected, not checked).

Display transforms are separate. Eldr's colour AOV in the viewport is ACES tonemapped (`eldr:tonemapper` aces, auto exposure on), display-referred but still linear; the host applies the sRGB or OCIO transfer on top. The same scene through usdrecord (default settings) reads, after decoding sRGB: white patch 0.906 -> 0.672, neutral 5 0.192 -> 0.171, black 0.032 -> 0.010, orange red channel 0.747 -> 0.658. So a viewport or PNG can never be compared with the linear EXR numbers above; in Houdini an ACES view on top would tonemap twice (not measured).

## Solaris viewport

| Topic | Behaviour |
|---|---|
| Readback | Houdini gives an OpenGL Hgi but composites with Vulkan, so shared GL textures show nothing: Eldr reads frames back. Houdini binds colour (Float16Vec4), depth, normal, primvars:st, primId, instanceId. A buffer returns only if Houdini mapped it since the last frame; while the camera moves, unread extra AOVs are skipped. `HDVK_DEBUG_AOVS=1` prints the bindings. |
| Render Settings LOP | Looking through one changes only what its Eldr tab sets. Houdini's viewport is told by `houdini:viewport` or, when a Render Settings LOP frame arrives without it (`houdini:renderer = husk`), by `houdini:interactive`: the frame stays interactive (no husk mode, the film never starts "final", even after a camera switch) and the products' resolution doesn't apply. A missing camera (the default `/cameras/camera1`) keeps the viewport's camera. `tests/houdini_render_settings.py` checks this headless. |
| Diagnostics | `HDVK_DEBUG_AOVS=1` also prints every non-`eldr:` setting the host sets (`houdini:viewport`, `renderCameraPath`, husk's `rasterRenderProducts`) |
| Depth, navigation, guides | Houdini's tumble pivot, pan speed and its own light and camera guides (drawn over the image, depth-tested) read the depth AOV. As Karma's it is linear (`depthstyle` `linear`): distance along the view axis, 0 where nothing was hit, whatever near and far planes Houdini assumes. The depth buffer always clears to the far plane: husk binds depth with clear value 0, which used to leave the frame empty. husk's `depth` var matches Karma's within 2% on step4. `viewportBench --nav` (ctest `viewport_nav`) checks the standalone build's window depth back-projects onto the scene, also when mapped frames later and after the film converged. |
| Still viewport | Houdini's free camera is a scene prim (`/__HoudiniFreeCamera__`) that may sync again unchanged; only real camera, lens, scene or setting changes start the film over, so a still view converges and stops changing. `HDVK_DEBUG_AOVS=1` prints every 60 frames how many frames moved the camera or changed the scene, and the near and far planes. |
| Selection outline | `UsdRenderers.json` sets `"needsselection": true`; Houdini draws the outline itself |
| Ids | primId is the render index's prim id, instanceId the instance index, as Houdini's picking expects |
| Frame times | 1280x720, moving camera. Kitchen_set: colour, depth, ids read 23.7 ms; colour and depth 20.8 ms; all six 32.1 ms. step4 with ids: 12.3 ms. usdview (GPU sharing): about 15 ms (Kitchen_set), 11 ms (step4). |

## Not checked

- Interactive Solaris use in the GUI: navigation speed, the outline, Render Settings LOP options.
- The Python ctest suite uses `usdenv.cmd` (USD 26.08) and doesn't apply to this build.
