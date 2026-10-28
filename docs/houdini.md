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
| `HdVkRendererPlugin_Geometry.ds` | Render Geometry Settings LOP tab: Eldr's own primvars in collapsible sections (Subdivision and dicing, Visibility, Motion, Curves and points, Reflections, Volumes), each with a tooltip, and a line on the order eldr, Karma, default |
| `HdVkRendererPlugin_Viewport.ds` | Viewport Display Options |

Nodes made before Eldr was installed need **Rebuild Renderer-Specific Parameters**.

`UsdRenderers.json` also sets `"complexitymultiplier": 1.3`, as Houdini's own entry for Karma does: the viewport's Level of Detail (1 by default) times 1.3 is Hydra's "very high" complexity, refine level 3. Without it Houdini asks for level 0 and subdivision surfaces show their cage (not checked in the GUI; see below).

## Render Geometry Settings

Per-object controls are primvars on the prim. Every Karma control Eldr reads has an `eldr:object:` twin on Eldr's tab: **Eldr's setting wins when set, then Karma's (its tab), then the USD default**. The twins have their own parameter names, so Houdini's rule of keeping only the first parameter of a name (`loputils.addDialogScriptFolder` drops a second one with a "Duplicate" warning) does not apply. Each control authors the attribute below (checked with hython: SOP Import into a Render Geometry Settings LOP).

Eldr's tab, by section (`houdini/gen_ui.py`, SECTIONS):

| Section | Control | Attribute (Karma's twin: `karma:` in place of `eldr:`) | Eldr |
|---|---|---|---|
| Subdivision and dicing | Render as Subdivision | `bool primvars:eldr:renderAsSubdivision` | A polygon mesh renders as Catmull-Clark, as with Mesh Edit; Karma has no such control |
| | Subdivision Level | `int primvars:eldr:refineLevel` | The refine level instead of the viewport's or husk's, at most `eldr:maxRefineLevel` (Karma dices by screen size) |
| | Dicing Maximum Depth | `int primvars:eldr:object:dicingdepthmax` | Caps the refine level (-1: no cap) |
| Visibility | Render Visibility | `string primvars:eldr:object:rendervisibility` | Camera (`primary`), shadows (`shadow`: shadow maps and traced shadow rays) and secondary rays (`diffuse`, `reflect`, `refract` together: traced reflections, GI probes, refraction). `-primary` is a phantom: unseen, still shadows and reflects. `""` hides it. Screen-space effects (SSR, SSGI, AO) see what the camera sees. Meshes, curves, points; not volumes. |
| | Holdout Mode | `int primvars:eldr:object:holdoutmode` | Matte and Background both cut a holdout |
| Motion | Enable Motion Blur | `bool primvars:eldr:object:mblur` | Off: no transform or deformation motion of its own; camera motion still blurs it |
| | Velocity Blur, Instance Velocity Blur | `int primvars:eldr:object:vblur`, `instance_vblur` | 0 ignores `velocities` (default, as Karma); 1 blurs meshes, curves and points by `velocities`, a PointInstancer's instances by `velocities` and `angularVelocities`; 2 adds `accelerations`. Linear between the shutter's ends: accelerations move the ends, the blur doesn't curve. |
| Curves and points | Render Curves As | `int primvars:eldr:object:curve_style` | Flat and Oriented Ribbons: ribbons (camera-facing without normals); Rounded Curves: tubes; USD Default: `eldr:curveRibbons` |
| | Render Points As | `int primvars:eldr:object:point_style` | Discs and Oriented Discs: camera-facing discs; Spheres: spheres; USD Default: `eldr:pointSprites` |
| | Width Scale | `float primvars:eldr:object:width_scale` | Multiplies curve and point widths |
| | Hair Shading | `bool primvars:eldr:hair` | Eldr only |
| Reflections | Reflection Probe, Planar Reflection | `bool primvars:eldr:reflectionProbe`, `primvars:eldr:planarReflection` | Eldr only |
| Volumes | Volume Limit | `float primvars:eldr:object:volumelimit` | Above 0, the volume scatters more than once |

`primvars:eldr:ribbons` and `primvars:eldr:sprites` are still read (they win over the styles) but no longer have a control: Render Curves As and Render Points As cover them.

Not honoured from the Karma tab: Cull Backface (Eldr draws every mesh double-sided), time samples (Eldr samples points and transforms at the shutter's open and close, `eldr:motionBlurTimeSamples`), the volume step rates and filters (`eldr:volumeStepSize` is global), Dicing Quality and the other dicing controls, Displacement Style, the quality and limit controls, light-source scales, portals, LPE tags, caustics, outlines, trace sets and stencils.

## Subdivision surfaces

A mesh is a subdivision surface when its `subdivisionScheme` is `catmullClark`, `loop` or `bilinear`. SOP Import authors `none` for polygons unless **Geometry Handling > Treat Polygons as Subdivision Surfaces** is on (optionally for a **Subdivision Group** only), which authors `catmullClark`; a Mesh LOP or an Edit Properties LOP can set `subdivisionScheme` too.

Eldr refines it uniformly with OpenSubdiv to Hydra's refine level, at most the **Subdivision Max Level** render setting (`eldr:maxRefineLevel`, 3), or the **Subdivision Level** on Eldr's tab of a Render Geometry Settings LOP per mesh. husk's `--complexity` defaults to `veryhigh`: level 3; `low` draws the cage (measured with husk: a catmullClark box covered 1024 pixels at `low`, 568 at `medium` to `veryhigh`). The viewport's level is its Level of Detail (Display Options > Geometry) times the `complexitymultiplier` above.

## Render statistics

Houdini reads `GetRenderStats()` through `houdini/UsdRenderers.json` (`statsdatapaths`). Where it shows, from Houdini 22's own files (the GUI itself was not opened to check):

- The viewport's **Show/Hide Render Stats** toolbar button (`renderstatsOn` in the viewer toolbar; `hou.SceneViewer.setShowRenderStats`: "renderer time and progress when viewing LOPs") draws the overlay: renderer name, stage, percentage, progress bar and the two annotation lines.
- The stats panel (`python3.13libs/renderstats/widgets.py`, `qt/qml/houdini/renderstats/GenericStats.qml`) lists only Houdini's fixed token set: renderer name and stage, progress, **Memory** and **Peak Memory**. It has no per-pass rows; names outside that set (`HUSD_RENDERSTATS_TOKENS` in the QML) are not shown by the generic template.
- All of Eldr's own numbers (GPU passes, draws, memory by kind, the last load) are in usdview's **Eldr > Render Statistics** and `GetRenderStats()`; the viewport carries the two annotations, which is where the numbers Houdini does not list have to go.

There is no menu entry named "Render Statistics" in Houdini 22. Unverified: which Render Gallery or snapshot view embeds the panel, and whether the overlay draws a newline inside an annotation as a second line.

| Name | Shown as |
|---|---|
| `rendererStage` | Word before the percentage ("Accumulating 24%") |
| `percentDone` (0-100) | Percentage, progress bar, ETA |
| `totalClockTime` (s) | Elapsed; frozen at convergence |
| `totalMemory`, `peakMemory` (bytes) | Memory, Peak Memory |
| `renderProgressAnnotation`, `renderStatsAnnotation` | Free text lines. The stats annotation ends with a second line, the last load's summary ("loaded in 6.1 s: shaders 4.8, textures 0.9, BVH 0.4, sync 0.3"), also `loadLine` and printed to stdout as `Eldr: loaded in ...` (husk's log). Written after a frame that followed over 0.25 s of load work; stays until the next load |
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
- Eldr binds fields by name. Without a volume material it shades `density` only, as Karma (`eldr:volumeDefaultFields`: also `temperature`, `Cd`, `emission`). Material Library networks of `mtlxvolume`/`anisotropic_vdf` or `kma_volume` with constants, `geompropvalue` and `multiply` are read, and ramps and remaps of a field: MaterialX's `mtlxremap`, `mtlxrange`, `mtlxramp`, `mtlxramplr`/`tb`, math, and Houdini's MtlX Color/Float Ramp and Cubic Ramp and Karma Ramp Const (see the features page). Feed a ramp's `texcoord` from the field through MtlX Combine2. The Karma Pyro Shader is not read (Eldr renders the volume as without a material and warns). SOP Import names a nameless volume's field `field_0`: give volumes a `name` (Name SOP, Pyro's `density`).

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
| Dome in the camera | Hidden unless `renderlightgeo` is 1 (schema default 0) | Drawn unless `inputs:karma:light:renderlightgeo` is authored 0 on the dome, `eldr:domeCameraVisible` is off or Hydra's `domeLightCameraVisibility` is false; lighting and reflections keep the dome |
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

Rough metals differed by model, not lighting (since resolved: UsdPreviewSurface now shades single scattering as Storm and Karma do, standard_surface and OpenPBR keep the multiple scattering MaterialX and Karma give them; the numbers below are from before): Eldr compensates GGX multiple scattering (Fdez-Aguera's split-sum form for image lights, Filament's lobe scale for lights), Karma's UsdPreviewSurface is single scattering (a white furnace gives Karma exactly the single-scatter albedo, with alpha = roughness^2). Gold (F0 1, 0.78, 0.34) facing the camera: roughness 0.2 matches within 0.2%, 0.45 is 4-6% brighter in Eldr, 0.8 50-80% brighter (direct lights: the lobe scale; image lights: the energy Karma loses). In step4, the pillar (-13%) and the gold ball's centre (-18%) are interreflections Eldr leaves out by default: with `eldr:gi`, `eldr:reflectionProbe`, `eldr:rtShadows` and `eldr:rtReflections` on, the pillar is 1.03, 1.05, 0.97 of Karma (the HDRI pattern above) and the gold centre 1.12, 1.10, 0.98 (multiple scattering). Not explained yet: the gold ball's sun highlight sits 2-3 pixels higher in Eldr (320x180), so a box on its upper flank reads 50-70% brighter while the column through the ball sums to 0.9 of Karma's (Eldr draws spheres as Hydra's tessellated mesh; suspected, not checked).

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
| Frame times | 1280x720, moving camera. Kitchen_set: colour, depth, ids read 23.7 ms; colour and depth 20.8 ms; all six 32.1 ms. step4 with ids: 12.3 ms. usdview (GPU sharing): about 15 ms (Kitchen_set), 11 ms (step4). step4 at 1866x737 (`viewportBench --app-ms 11`, 11 ms standing in for Houdini's own work, which matches the 47 fps seen in the viewport): colour and depth read 20.1 ms a frame, of which Eldr 9.1 (GPU 3.7 render plus 2.7 copying to the CPU, waited for, 1.3 memcpy, 1 Hydra); with ids 25.8. With `eldr:overlapReadback` (a moving camera's frame finishes on the GPU while Houdini draws the one before) 13.6 and 14.4 ms. Houdini's bindings (`viewportBench --houdini`: colour, depth and ids mapped every frame, the scene camera edited with shutter -0.25 to 0.25), 1866x737, 6 ms standing in for Houdini, 300 frames: macbeth 14.2 ms median (p95 18.5, p99 26.2, latency camera to image 22.6 ms; Eldr 7.4 ms of which 6.2 copying) before `eldr:inPlaceReadback`, 8.0 ms (p95 9.0, p99 14.0, latency 10.1 ms; Eldr 1.1 ms) after; step4 14.4 (p95 18.0, p99 19.5, latency 23.1) to 8.2 ms (p95 9.2, p99 9.6, latency 10.7). Without overlap: macbeth 25.4 to 14.6 ms, step4 20.1 to 15.5 ms. |
| Overlapped readback | While the camera moves, Eldr returns as soon as a frame is submitted and hands it over at the next one (`eldr:overlapReadback`): Houdini shows the frame before, a still camera's frame is its own. Only for colour, depth and ids read back into the same buffers as last frame; extra AOVs, products and husk read at once. `viewportBench --overlap` (ctest `viewport_overlap`) checks the frames arrive unchanged. |
| Readback in place | Houdini maps colour, depth, primId and instanceId every frame: 27 MB at 1866x737. Copying them into the buffers' own memory took 5 to 8 ms a frame on the CPU (memory bound: more threads don't help), with hitches to 30 ms. Now the buffers map the frame where the GPU wrote it (`eldr:inPlaceReadback`, two buffers in turn, so the app reads one while the next frame renders into the other); the linear depth conversion runs in parallel in that memory. Left: the GPU's copy to host memory (4.6 ms of macbeth's 6 GPU ms, overlapped while moving) and Houdini's own upload of the four. |
| Frame pacing | Houdini's free camera has a shutter (-0.25 to 0.25) but no time samples: Eldr takes it as sampled motion without camera motion, so tumbling isn't motion blurred (no `motionBlur` pass); moving objects still are. The open shutter does keep `eldr:renderScale` from lowering the resolution while moving. `houdini:interactive` (tumbling) isn't used. |
| Frame timing CSV | `HDVK_TIMING=2` writes a row per frame to `HDVK_TIMING_CSV` (else `%TEMP%\eldr_timing_<unix time>.csv`; the path is printed to the console at the first frame): `interval_ms` between frames, `app_ms` Houdini's time between Eldr's executes (its sync, maps, upload and drawing), Eldr's `setup_ms`, `collect_ms` (a moving frame's handover), `wait_ms` (for the GPU), `render_ms` (recording and submitting), `after_ms` (depth conversion, products), `execute_ms`, `fetch_ms` (buffers read back lazily in Houdini's maps), `gpu_ms`, and whether the frame moved, was deferred or collected. Any `HDVK_TIMING` also times teardown. | `HDVK_TIMING=1` prints startup laps: device, the renderer's construction, the first three frames' passes over 5 ms (pipelines compile on first use). Then the time Houdini spent in Eldr between two executes, with call counts: `stats_ms` (GetRenderStats), `map_ms` (render buffer maps), `alloc_ms`, `isconverged_ms`, `camsync_ms`, `sync_ms` (meshes, lights, materials, instancers), and `app_outside_eldr_ms`, the rest of `app_ms`: Houdini's own. `HDVK_TIMING` also prints the readback memory's type once (host cached: Houdini reads it at memory speed; on the GTX 1080 Ti type 9, flags 0xe). `HDVK_NO_STATS=1` makes GetRenderStats return the renderer name only (A/B).
| Viewport stats while moving | "TAA 10/18" was TAA's history weight (a moving camera's frame blends in at 1/10, `kTaaMoving`) over `eldr:taaSamples`, not samples a frame works through: each frame renders one sample (`eldr:interactiveSamples` only loops the noise pattern over 8 frames). A moving frame now reads "TAA interactive (moving)". macbeth's moving frame: 6 ms GPU, of which 4.6 copying to the CPU. |
| Still frames | Before `eldr:lazyExtraAovs`, every still frame also rendered and read back the normal and st AOVs Houdini binds but doesn't read: macbeth 1866x737 accumulating the film took 52 ms a frame (GPU 19.2, of which the AOV pass 8.1), now 16 ms (GPU 6.9, `viewportBench --houdini --still`, machine shared with other jobs). They render once, when the film converged (one more frame). Film frames cost more than moving ones: they wait for their own frame (no overlap) and add the film pass. |
| Guides and depth | With `eldr:overlapReadback` a moving camera's colour and depth are the frame before, while Houdini draws its guides with the current camera: guides slide over the image and depth-test against the old depth by one frame of motion, about 47 px per degree turned between redraws for macbeth's camera at 1866 px wide (12 px at the bench's 0.25 degree a frame). Off: aligned, macbeth 14.6 instead of 8.0 ms a frame and step4 15.5 instead of 8.2 (6 ms of Houdini work), camera-to-image latency 8.6 instead of 10.1 ms. Depth and ids are jittered (TAA, half a pixel) only while moving; once still they settle within two frames (the film keeps the sample nearest the pixel centre): 9845 then 1950 then 0 pixels change. |
| Startup | macbeth 1866x737 (`viewportBench --houdini --startup`; a new exe name makes the NVIDIA driver's shader cache cold, an empty `HDVK_PIPELINE_CACHE_DIR` Eldr's): cold, plugin to converged 53 s before (delegate 15.2 s: ray tracing pipelines 9.0, volume pipelines 3.1; first frame 34.3 s: a whole shading set of 120 pipelines 23.6, the extra AOVs' 24 pipelines one at a time 10.0), 9.6 s after (delegate 2.1, first frame 6.1: shading 4.8, AOVs 0.9). Warm: 2.2 s (delegate 0.55 of which the Vulkan device 0.3-0.4, first frame 0.5, then 64 film frames). `eldr:lazyPipelines` compiles only the shading variants drawn; ray tracing and volume pipelines wait for their first use; every pipeline goes through Eldr's cache file (`%LOCALAPPDATA%\Eldr\pipelines-*.bin`, shared by usdview and Houdini). |
| Render Outputs menu | The viewport's Render Outputs menu lists the vars of the Render Settings LOP's products (Render Var LOP, then Render Product LOP, then Render Settings LOP). The viewport binds a var by its prim name (`rendervar1`) without the source; Eldr looks the name up in the active settings prim's products and uses that var's `sourceName`/`sourceType` (also for the buffer format it is asked about), so a var picked there fills like it does in husk. Put a beauty var (`C.*[LO]`, color4f) in the product first: Houdini shows only the product's vars, so without one there is no `color` entry (Karma does the same). Supported sources: the list under husk above (`C.*[LO]`, `C<RD>L`, `C<RD>.+L`, `C<RG>L`, `C<RG>.+L`, `C.*O`, `CO`, `CDA`, `C<RG>.*[LO]`, `C<RD>.*L`) plus `ray:hitP`, `ray:hitPz`, `ray:hitN` and the other Karma raw vars Eldr lists; other LPEs stay black (Karma-only light groups, per-object LPEs). Tested headlessly (ctest `aovs`: a var bound by name alone); the menu itself is not clicked through. |
| Viewport channel buttons (RGB, R, G, B, A, L) | Do nothing in Eldr's view as far as known. Houdini's Python API, HDAs, config and help don't expose or describe them for Hydra delegates (the help only mentions channel toggles for the Copernicus and Unreal viewers), so they are applied inside Houdini's compiled viewport, and nothing a delegate reports (AOV format, colour space, settings) is documented as controlling them. Eldr gives the same Float16Vec4 colour as the descriptor asks for. Not fixed on the Eldr side and not verified in the GUI (no Karma comparison made): use Render Outputs to view single AOVs, or the Cop viewer for channels. |

## Not checked

- Interactive Solaris use in the GUI: navigation speed, the outline, Render Settings LOP options.
- The viewport's refine level with `complexitymultiplier` 1.3 (headless checks reach husk only).
- The Python ctest suite uses `usdenv.cmd` (USD 26.08) and doesn't apply to this build.
