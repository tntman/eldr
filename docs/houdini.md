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
| | Dicing Quality | `float primvars:eldr:object:dicingquality` | With Dicing by Screen Size (`eldr:screenDicing`, on): about quality squared polygons per pixel (0.5, as Karma's) |
| | Dicing Minimum Depth | `int primvars:eldr:object:dicingdepthmin` | Lowest refine level (-1: none) |
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
| Volumes | Volume Limit | `float primvars:eldr:object:volumelimit` | Above 0, the volume scatters more than once, fitted to Karma at that limit |
| | Volume Velocity Blur Scale | `float primvars:eldr:object:volumevblurscale` | Multiplies the blur along `vel` of a volume with Velocity Blur on (default 1) |

`primvars:eldr:ribbons` and `primvars:eldr:sprites` are read too (they win over the styles) but have no control: Render Curves As and Render Points As cover them.

Not honoured from the Karma tab: Cull Backface (Eldr draws every mesh double-sided), time samples (Eldr samples points and transforms at the shutter's open and close, `eldr:motionBlurTimeSamples`), the volume step rates and filters (`eldr:volumeStepSize` is global), Dicing Quality Scale, Offscreen Quality, Dicing Camera and the other global dicing controls (Dicing Quality and the depths are honoured), Displacement Style, the limit controls (Volume Limit aside), the quality controls per object (Karma Render Settings' Diffuse, Reflection, Refraction, SSS and Volume Quality and Light Sampling Quality are honoured globally, from the settings prim: `eldr:diffuseQuality` and its twins win), light-source scales, portals, LPE tags on objects (lights' tags are light groups), fake caustics (caustics themselves are honoured: Enable Caustics, Roughness Clamp, a light's Contributes to Caustics), outlines, trace sets and stencils.

## Subdivision surfaces

A mesh is a subdivision surface when its `subdivisionScheme` is `catmullClark`, `loop` or `bilinear`. SOP Import authors `none` for polygons unless **Geometry Handling > Treat Polygons as Subdivision Surfaces** is on (optionally for a **Subdivision Group** only), which authors `catmullClark`; a Mesh LOP or an Edit Properties LOP can set `subdivisionScheme` too.

Eldr refines it uniformly with OpenSubdiv to Hydra's refine level, at most the **Subdivision Max Level** render setting (`eldr:maxRefineLevel`, 3), or the **Subdivision Level** on Eldr's tab of a Render Geometry Settings LOP per mesh. husk's `--complexity` defaults to `veryhigh`: level 3; `low` draws the cage (measured with husk: a catmullClark box covered 1024 pixels at `low`, 568 at `medium` to `veryhigh`). The viewport's level is its Level of Detail (Display Options > Geometry) times the `complexitymultiplier` above.

**Dicing by Screen Size** (`eldr:screenDicing`, on) replaces the host's level: each mesh gets the uniform level its nearest cage face needs for about Dicing Quality squared polygons per pixel of the render camera at the render resolution (Karma's `dicingquality`, 0.5; faces off screen at 0.1 of it), between Dicing Minimum and Maximum Depth and at most `eldr:maxRefineLevel`. husk's first frame syncs the cages, the render pass names its camera and the meshes re-dice before anything renders; when the camera moves between frames they re-dice again. The viewport keeps its levels while the camera moves and re-dices once it stops.

## Render statistics

Houdini reads `GetRenderStats()` through `houdini/UsdRenderers.json` (`statsdatapaths`). Where it shows, from Houdini 22's own files (the GUI itself was not opened to check):

- The viewport's **Show/Hide Render Stats** toolbar button (`renderstatsOn` in the viewer toolbar; `hou.SceneViewer.setShowRenderStats`: "renderer time and progress when viewing LOPs") draws the overlay: renderer name, stage, percentage, progress bar and the two annotation lines.
- The stats panel (`python3.13libs/renderstats/widgets.py`) loads the `qmlTemplate` of the renderer's `UsdRenderers.json` entry. Eldr's is `houdini/EldrStats.qml`, installed to `<build>/houdini/qml/EldrStats.qml` and found through `$ELDR_HOUDINI` (set by `eldr.json`; the fallback is Houdini's `$HFS/qt/qml/houdini/renderstats/GenericStats.qml`, which shows only name, stage, progress, Memory and Peak Memory). It shows: header (renderer, stage, elapsed, ETA, progress bar, annotation, samples, converged %, resolution/exposure/frame sharing/ray tracing when present), **Memory (GPU)** (GPU used, GPU peak, host-visible, GPU+host, VRAM in use/budget/size) with a donut of GPU memory by owner (textures, vertex data, indices, volumes, RT structures, targets/pools), **Time** (GPU frame, render call, scene upload, last load line) with a donut of GPU time per pass (top 3 + other), **Scene** (meshes, volumes, draw calls, lights if present, instances and triangles drawn/total) and an orange **material fallback** warning when `materialFallbackInputs` > 0. Every key is optional, so live and EXR data both render. In the standard names, `totalMemory` is GPU + host-visible memory and `peakMemory` the GPU peak: neither is process RAM. Rendered offscreen with hython for the crowd EXR and a live-style dictionary, no QML warnings; not opened in the Houdini GUI.
- All of Eldr's own numbers (GPU passes, draws, memory by kind, the last load) are in usdview's **Eldr > Render Statistics** and `GetRenderStats()`; the viewport carries the two annotations, which is where the numbers Houdini does not list have to go.

**Render Gallery and husk images.** The panel (and the gallery tiles) read an EXR's `husk:render_stats` metadata of the first AOV (`iinfo -J`), which holds the delegate's stats and a `__delegate` key naming the `UsdRenderers.json` entry; with neither the panel stays empty. Eldr's entry therefore has Karma's `husk.metadata` block (`string OpenEXR:husk:render_stats` = `$render_stats`; `renderMemory` and `renderTime` from `peakMemory` and `totalClockTime`) and `husk.stats_metadata` empty (without it husk writes every stat as its own flat EXR attribute and no `husk:render_stats`). With the block, husk's `-R HdVkRendererPlugin` EXR carries `husk:render_stats` as an object (`__delegate`, `rendererName`, `percentDone`, `totalMemory`, `gpuPassMs`, ... and husk's own keys), and the panel's `normalize_stats` and `findStats` resolve stage, progress, clock time, Memory and Peak Memory from it (checked in hython). The EXR products Eldr writes itself (UsdRender, usdrecord) carry `husk:render_stats` too: `husk:productmetadata` from husk if set, with `GetRenderStats()` over it and `__delegate`. A viewport snapshot passes the live stats dictionary through the same `statsdatapaths`. `tests/houdini_stats.py` checks the name mapping on both shapes; the panel itself was not opened.

There is no menu entry named "Render Statistics" in Houdini 22. Unverified: which Render Gallery or snapshot view embeds the panel, and whether the overlay draws a newline inside an annotation as a second line.

| Name | Shown as |
|---|---|
| `rendererStage` | Word before the percentage ("Accumulating 24%") |
| `percentDone` (0-100) | Percentage, progress bar, ETA |
| `totalClockTime` (s) | Elapsed; frozen at convergence |
| `totalMemory`, `peakMemory` (bytes) | Memory, Peak Memory |
| `renderProgressAnnotation`, `renderStatsAnnotation` | Free text lines. The stats annotation ends with a second line, the last load's summary ("loaded in 6.1 s: shaders 4.8, textures 0.9, rt pipelines 0.0, BVH 0.4, sync 0.3"; rt pipelines: the first trace's wait for the ray tracing pipelines still compiling since the delegate started, BVH: the acceleration structure builds), also `loadLine` and printed to stdout as `Eldr: loaded in ...` (husk's log). Written after a frame that followed over 0.25 s of load work; stays until the next load |
| `viewstats` entries | Labels like "GPU passes: Shading (ms)"; all of usdview's Render Statistics are included, bytes raw |

| `rendererStage` | Meaning |
|---|---|
| `accumulating` | Film |
| `interactive` | TAA (`percentDone` 100) |
| `converged` | Done |

`percentDone` is film samples over target (with adaptive sampling, the share of stopped pixels, whichever is further). Annotations read like "final 24/64 samples, 12% of pixels done" and "GPU 12.3 ms, 1442 triangles, 1 draws, 0.14 GB". A pass that did not run has a blank row.

## Materials

Houdini's Material Library and Karma-context (`kma`) networks are read as MaterialX, with Houdini's MaterialX library when `$HFS` is set. Karma's own VEX nodes that have no MaterialX definition are compiled by Eldr: Karma Ramp Const (`kma_rampconst`, every ramp basis as `hou.Ramp` evaluates it), Karma Hex-Tiled Texture (`kma_color_hextiling`, as Houdini's viewport GLSL; `direction` and the `weights` output aren't read) and Voronoi Noise 3D (`kma_voronoinoise3d`, distances with the four metrics; MaterialX's cells, so the pattern differs from Karma's). Against Karma all 121 basic and quick materials compile; median dE 1.7 (docs/materialx.md).

## Volumes

| Source | Support |
|---|---|
| `OpenVDBAsset` with `.vdb` files | As in the normal build |
| SOP volumes in Solaris (SOP Import LOP, no files) | VDB primitives become `OpenVDBAsset`, Houdini volumes `HoudiniFieldAsset`, with `op:<sop>.sop.volumes:...` paths. Eldr reads the geometry the LOP already cooked; Houdini volumes convert with `GU_PrimVDB::buildFromPrimVolume`. |
| Saved stages (USD ROP) | Volumes go to `<layer>.volumes/N.vdb` and `N.bgeo.sc`; husk renders them |

- Field numbering follows SOP Import: a `HoudiniFieldAsset`'s `fieldIndex` is the primitive number; an `OpenVDBAsset`'s counts VDB primitives named `fieldName`.
- Eldr binds fields by name. Without a volume material it shades `density` only, as Karma (`eldr:volumeDefaultFields`: also `temperature`, `Cd`, `emission`). Material Library networks of `mtlxvolume`/`anisotropic_vdf` or `kma_volume` with constants, `geompropvalue` and `multiply` are read, and ramps and remaps of a field: MaterialX's `mtlxremap`, `mtlxrange`, `mtlxramp`, `mtlxramplr`/`tb`, math, and Houdini's MtlX Color/Float Ramp and Cubic Ramp and Karma Ramp Const (see the features page). Feed a ramp's `texcoord` from the field through MtlX Combine2. The Karma Pyro Shader is not read (Eldr renders the volume as without a material and warns). SOP Import names a nameless volume's field `field_0`; without a `density` field Eldr shades it as the density.
- Velocity blur: a volume with Velocity Blur on (Render Geometry Settings, `karma:object:vblur`) blurs along its `vel` field in motion blur time samples: a vector VDB, or Houdini's `vel.x`, `vel.y`, `vel.z` volumes. Karma's Lookup Offset mode (Karma's default is Lattice Deformation); Volume Velocity Blur Scale multiplies it.

| Limitation | Detail |
|---|---|
| `op:` paths | Resolve only in the Houdini session holding the LOP network. husk on a USD file with `op:` paths warns "no locked SOP geometry" and skips the field: save with a USD ROP. |
| Vector volumes | `vel.x/.y/.z` are read as a vector for velocity blur only; other vector volumes (`Cd.x/.y/.z`) stay three float fields |
| Conversion cost | A VDB primitive the SOP edit left alone isn't converted again (the same grid); a Houdini volume converts on every field sync |
| Diagnostics | Tf warnings don't reach hython's console; a missing field shows as a missing volume |

## husk, AOVs and Cryptomatte

### Supported LPEs

The Karma Render Settings LOP's AOV toggles and Render Var LOP presets fill these; set the same vars and Eldr and Karma write the same EXR layers.

A Render Var's "Sample Blending" (Karma tab, `driver:parameters:aov:karma:filter`) is read: Sample Minimum/Maximum Value, Closest and Farthest work as in Karma; Average Sample Values (`ubox`, the default) keeps Eldr's per-AOV default; Adds Up and Stats aren't supported. Eldr's own `driver:parameters:aov:eldr:filter` (add it with an Edit Properties LOP) wins and adds `beauty`, `nearest`, `gauss` and `box` (features.md, Per-AOV sample filters).

| Karma AOV | LPE | Eldr |
|---|---|---|
| beauty | `C.*[LO]` | The beauty |
| directdiffuse, indirectdiffuse, combineddiffuse | `C<RD>L`, `C<RD>.+L`, `C<RD>.*L` | Diffuse reflection (subsurface apart) |
| directglossyreflection, indirectglossyreflection, combinedglossyreflection | `C<RG>L`, `C<RG>.+L`, `C<RG>.*L` | Glossy reflection, coat included |
| glossytransmission | `C<TG>.*L` | Refracted light |
| sss | `C<TD>.*L` | Subsurface |
| coat | `C<...'coat'>.*L` | The coat's reflection |
| combinedvolume, directvolume | `CV.*L`, `CVL` | Volumes' light (single scattering and its multiple-scattering approximation, emission) |
| combinedemission, directemission | `C.*O`, `CO` | Surface emission |
| albedo | `CDA`, `C[DV]A` | Diffuse albedo |
| shadow | `shadow;C.*[LO]` | The shadowed direct light |
| holdout_shadow ("Holdout Shadows") | `holdouts;shadow;CDL` | Shadows on holdouts (`karma:object:holdoutmode`), diffuse |
| per-light beauty ("Per LPE Tag" on beauty) | `C.*<L.'tag'>` | Light group: direct light of the lights tagged `tag` |
| caustics (no Karma preset) | `C<RD>.+<TS>L`, `C<RD><TS>.*L`, `C<RD>[<TS><RS>].*L`; raw `caustics` | Caustic photons' light (`eldr:caustics`), part of indirect diffuse |
| raw | `ray:hitP`, `ray:hitP_camera`, `ray:hitPz`, `ray:hitN`, `ray:hitN_facing`, `ray:primid`, `export_basecolor`, `primvars:st`, Cryptomatte | As listed in features.md |

Against Karma (showcase `2026-10-29_06_karma_aovs`, husk `BRAY_HdKarma` with the same vars): image means of direct diffuse, subsurface, transmission and volume within ~25% (direct diffuse 0.054 vs 0.056, sss 0.012 vs 0.012); indirect diffuse follows Eldr's brighter GI. Differences by design: Karma's light groups include their indirect light (Eldr's are direct only); Karma's `shadow;` counts occluded indirect light too (in a closed room most of the image), Eldr's only the lights' and the dome's direct shadow, which is what a shadow matte needs; Karma's coat also reflects the room, Eldr's coat only the lights and dome (as its beauty).

Not filled (they warn): `unoccluded;` and the `shadow;` variants of single splits, `CV.+L`, `C.+O`, `CL`, ambient occlusion, motion vectors, outlines, and per-light variants of anything but the beauty (`C<RD><L.'tag'>`).

**Light groups.** Tag lights with the light's LPE Tag (Karma tab, `inputs:karma:light:lpetag`; Eldr's own `inputs:eldr:light:lpetag` wins), then tick "Per LPE Tag" on the beauty in the Karma Render Settings LOP (or add a Render Var with `C.*<L.'tag'>`). Each tag's var is the direct light of its lights; a tagged dome light adds its unblocked light. Up to 8 tags per process (the first 8 asked for). Indirect light (GI, reflections) isn't split by light: for comp, rebuild the beauty as the groups + `indirectdiffuse` + `indirectglossyreflection` + `glossytransmission` + emission + volume.

Run husk with `-R HdVkRendererPlugin` (or `-R Eldr`); `husk --list-renderers` shows `HdVkRendererPlugin (Eldr)`. husk needs a licence above Apprentice.

```
husk -R HdVkRendererPlugin -c /cam --res 256 256 -o out.png tests\scenes\volume.usda
```

| Topic | Behaviour |
|---|---|
| AOVs | husk writes the EXR from Eldr's buffers. Eldr resolves each var from UsdRender's `sourceName`/`sourceType` with a small parser for the light path expressions Karma writes (its Render Settings LOP's AOV toggles, spaces ignored); the list is under "Supported LPEs" below. Others warn and stay black. |
| EXR layers | Same as Karma's: `C` RGBA, `<var>.R/G/B`, `depth.z`, `N.x/y/z`, `P.x/y/z`, `primid.id`, so one comp template serves both |
| Cryptomatte | Layers `<name>00`, `01`, `02` (6 ranks, soft edges from the film) with manifest metadata, as Karma. Always three layers whatever `cryptomatterank`. |
| `ray:primid` | Float: 0 off objects, Hydra primId + 1 on objects (no coverage). usdview and usdrecord keep Int32 for picking. |
| Product size | When the product's framing differs from husk's buffers (`husk --res`), Eldr renders at the product's framing and resamples (nearest) into all buffers |
| Beauty | Under husk (it sets `rasterRenderProducts`): the settings prim's settings, as in the viewport; the film converges to `eldr:filmSamples`; linear beauty in the products (`eldr:productLinear`) |
| `eldr:` attributes | On the UsdRenderSettings prim they are render settings (`custom int eldr:filmSamples = 7`); they also work with `usdrecord --renderSettingsPrimPath` |
| Sequences | `husk -f 1 -n 3` with `$F4` in the product name works; husk waits for each frame to converge. Motion blur over the camera shutter is smooth (`eldr:motionBlurResidual`). |
| Crowds | Set `HD_ENABLE_DEFERRED_SKINNING=1` in husk's environment for agent crowds (an animated Xform per agent above an instanceable SkelRoot, as Houdini writes them). Without it UsdSkelImaging gives every agent its own copy of the prototype; with it, one instanced mesh per prototype skinned per instance. crowd_9k at 480x270: peak 7.5 to 2.4 GB, first frame 81 to 55 s, each further frame 23 to 13 s (without, then with it); same image (8-bit max 7 levels on 7 of 518k pixels at 960x540). USD reads it once per process before Eldr loads, so Eldr cannot set it itself. Per render job: set the USD Render ROP's Render Command to `$ELDR_HUSK` (Eldr's package points it at `<build>/houdini/bin/eldr_husk.cmd`, which sets the variable and runs husk; crowd_9k 57 s / 2.8 GB measured through it). Don't set it for a whole Houdini session that also uses Karma: Karma renders the agents as garbage with it (checked in husk). Each skeleton is posed once per shutter time for all its meshes (`HDVK_NO_POSE_SHARE=1` poses it per mesh, for comparison): crowd_9k 480x270 with motion blur, further frame 5.05 s shared vs 6.15 s per mesh (sync 1.3 vs 2.4 s), same image. |
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
| Light linking (`collection:lightLink`, `collection:shadowLink`, the Light Linker LOP's) | Honoured | Honoured: tests/scenes/lightlink.usda lum 0.95, worst region 0.16 (karma_golden) |
| IES profiles (`shaping:ies:*`) | Candela as is; phi 0 at the light's -X | Same: tests/scenes/ies.usda within 3% per region, raw and normalized with `angleScale` |
| Time to converge | step4 33 s, lights 29 s (32 spp) | 10-25 s |

### ColorChecker, Eldr vs Karma (linear EXR, no tonemap)

`tests/scenes/macbeth.usda` is a ColorChecker Classic (BabelColor 2005 average, D50 xyY Bradford-adapted to D65, linear Rec.709; flat diffuse, roughness 1, specular 0; cyan clamped, it is outside Rec.709) with a `light` variant set: `hdri` (StinsonBeach.hdr), `white` (uniform dome, intensity 1), `distant`, `rect`. `tests/macbeth_compare.py --exrinfo <exrinfo>` renders each with Eldr and husk (Karma CPU) to linear EXRs and prints per patch the albedo, Eldr, Karma and their ratio, plus a CIEDE2000 summary (Lab white = Karma's white patch). Eldr is forced to `eldr:tonemapper` none, `eldr:autoExposure` off, `eldr:exposureCompensation` 0, `eldr:bloomIntensity` 0, `eldr:productLinear` on. ctest `macbeth` checks the white dome (radiance = albedo) for Eldr alone.

| Lighting | Eldr / Karma | dE2000 Eldr vs Karma (mean, worst) | Against albedo |
|---|---|---|---|
| white dome | 1.004-1.005 | 0.09, 0.11 | Eldr within 0.4%, Karma 0.4% low |
| distant | 1.003-1.005 | 0.08, 0.10 | |
| rect | 1.000-1.003 | 0.05, 0.10 | Both within 0.5% of the analytic irradiance |
| hdri | R 0.96, G 1.02, B 0.90 | 2.72, 7.28 (white patch) | Both load the HDRI (white patch -58% vs the white dome). Eldr within 0.2% of the brute-force integral, Karma 4% R, -2% G, 11% B off it (below) |

Eldr's rect light diffuse clips the rect to the horizon and takes Lambert's exact form factor; both renderers are within 0.5% of the analytic irradiance (numerical integral over the rect).

With the HDRI the difference is Karma's. A brute-force cosine-weighted sum over StinsonBeach.hdr's texels (UsdLux lat-long mapping) for the chart's normal gives E/pi = 0.388, 0.418, 0.474; Eldr's white patch / albedo is 0.387, 0.417, 0.473, Karma's 0.402, 0.409, 0.525. Synthetic smooth lat-longs (one per axis, the dome rotated 90 degrees) match the brute force in both renderers within 1%, so orientation and colour handling agree. The gap is all in the sun (35 texels over luminance 10): the HDR without them matches in Karma within 1%, the sun alone is 5% red, -3% green, 19% blue off. Karma is biased on hot spots a few texels wide: a uniform 5x5-texel block on black loses 10% in Karma (the same block at 4x the resolution, same solid angle, 5%), independent of samples (128 or 1024) and colour limit; StinsonBeach upsampled 4x (nearest) moves Karma to 1.015, 0.986, 1.043 of the brute force. Eldr (cube from 4x4-supersampled bilinear lookups, SH9 irradiance, the sun extracted as a distant light) stays within 0.2% of the brute force.

Metals follow the material model, as in Karma: UsdPreviewSurface shades single scattering (as Storm and Karma), standard_surface and OpenPBR add MaterialX's multiple-scattering energy compensation (as Karma). In step4, the pillar (-13%) and the gold ball's centre (-18%) are interreflections Eldr leaves out by default: with `eldr:gi`, `eldr:reflectionProbe`, `eldr:rtShadows` and `eldr:rtReflections` on, the pillar is 1.03, 1.05, 0.97 of Karma (the HDRI pattern above). Not explained: the gold ball's sun highlight sits 2-3 pixels higher in Eldr (320x180), so a box on its upper flank reads 50-70% brighter while the column through the ball sums to 0.9 of Karma's (Eldr draws spheres as Hydra's tessellated mesh; suspected, not checked).

Display transforms are separate. In usdview and usdrecord Eldr's colour AOV is ACES tonemapped (`eldr:tonemapper` aces), display-referred but still linear; the host applies the sRGB or OCIO transfer on top. The same scene through usdrecord (default settings) reads, after decoding sRGB: white patch 0.906 -> 0.672, neutral 5 0.192 -> 0.171, black 0.032 -> 0.010, orange red channel 0.747 -> 0.658. So a usdview image or PNG can't be compared with the linear EXR numbers above. Houdini's viewport gets the linear beauty instead (see Solaris viewport, Colour).

## Draft and final renders

Eldr has no quality mode and no final-quality button: a render settings prim says how good a render is, as for any VFX renderer. Make one Render Settings LOP for drafts (the defaults: film, no ray-traced effects) and one for finals that ticks, on its Eldr tab, the settings of docs/settings.md's "Final render" (Film Restarts at Once, Ray-Traced Reflections, Refraction and Hit Shadows, Final Gather, Caustics, Ray-Traced Shadow Rays per Light 1, Ray-Traced Shadow Lights 16, Auto Exposure off; for production also Global Illumination, Ray-Traced Shadows, Denoise and more Film Samples). husk renders the prim it is given (`-s`, or the stage's `renderSettingsPrimPath`).

To see the final in the viewport, choose the final Render Settings prim in the viewport's camera/Render Settings menu (Houdini then sends `houdini:use_render_settings_prim` and Eldr reads that prim's `eldr:` attributes, see "Render Settings LOP" below), or make it the stage's `renderSettingsPrimPath`. The viewport's Display Options (Eldr section) set the same settings per viewport, without a prim. `tests/houdini_render_settings.py` also checks the prim headless.

Every showcase folder has such a pair ready: `scene.hip` next to `scene.usda`, with `eldr_default` (nothing set), `eldr_final` (each final setting ticked), `karma` (a Karma Render Settings LOP with the same camera, resolution and samples), the scene's own A/B nodes, and a USD Render ROP per node (Eldr's Render Command `$ELDR_HUSK`). `hython houdini/make_showcase_hip.py` builds them and writes the same three prims into each `scene.usda`; its `FINAL` list is the one place that says what `eldr_final` ticks. `houdini/make_houdini_tests.py` builds `houdini-tests/*.hip` from the Houdini checklist the same way. To keep the files small, each Render Settings LOP keeps only Eldr's tab (all of it on `eldr_default`, only the settings it sets on the others); a hip is 0.6 to 1.8 MB, most of it Houdini's spare parameter definitions. Karma's node writes its own beauty product, so the scene's AOV products render only from the Eldr nodes. The viewport renderer can't be set from hython: choose Eldr in the viewport.

## Solaris viewport

| Topic | Behaviour |
|---|---|
| Readback | Houdini gives an OpenGL Hgi but composites with Vulkan, so shared GL textures show nothing: Eldr reads frames back. Houdini binds colour (Float16Vec4), depth, normal, primvars:st, primId, instanceId. A buffer returns only if Houdini mapped it since the last frame; while the camera moves, unread extra AOVs are skipped. `HDVK_DEBUG_AOVS=1` prints the bindings. |
| Render Settings LOP | Looking through one changes only what its Eldr tab sets. Houdini's viewport is told by `houdini:viewport` or, when a Render Settings LOP frame arrives without it (`houdini:renderer = husk`), by `houdini:interactive`: the frame stays interactive (no husk mode) and the products' resolution doesn't apply. A missing camera (the default `/cameras/camera1`) keeps the viewport's camera. `tests/houdini_render_settings.py` checks this headless. |
| Diagnostics | `HDVK_DEBUG_AOVS=1` also prints every non-`eldr:` setting the host sets (`houdini:viewport`, `renderCameraPath`, husk's `rasterRenderProducts`); `HDVK_DEBUG_HOLD=1` prints per frame whether the camera moved, the Still Delay hold, the render width and the GPU time |
| Depth, navigation, guides | Houdini's tumble pivot, pan speed and its own light and camera guides (drawn over the image, depth-tested) read the depth AOV. As Karma's it is linear (`depthstyle` `linear`): distance along the view axis, 0 where nothing was hit, whatever near and far planes Houdini assumes. The depth buffer always clears to the far plane, even though husk binds depth with clear value 0. husk's `depth` var matches Karma's within 2% on step4. `viewportBench --nav` (ctest `viewport_nav`) checks the standalone build's window depth back-projects onto the scene, also when mapped frames later and after the film converged. |
| Still viewport | Houdini's free camera is a scene prim (`/__HoudiniFreeCamera__`) that may sync again unchanged; only real camera, lens, scene or setting changes start the film over, so a still view converges and stops changing. `HDVK_DEBUG_AOVS=1` prints every 60 frames how many frames moved the camera or changed the scene, and the near and far planes. |
| Colour | Linear scene colour, the same values as the EXR's linear beauty (`eldr:productLinear`, on): no tonemap, bloom or auto exposure of Eldr's own (auto exposure only if the settings prim authors `eldr:autoExposure`; `eldr:exposureCompensation` applies). The display transform is Houdini's, from its own settings and environment (OCIO config, view, gamma, LUT), as for Karma; Houdini 22's default view is Un-tone-mapped (sRGB). Eldr never reads `$OCIO`. `eldr:productLinear` off applies Eldr's ACES and auto exposure under Houdini's view, which then tonemaps twice (ACES alone takes step4's floor from 0.67 linear to 0.48). |
| Selection outline | `UsdRenderers.json` sets `"needsselection": true`; Houdini draws the outline itself |
| Ids | primId is the render index's prim id, instanceId the instance index, as Houdini's picking expects |
| Frame times | Houdini's bindings (`viewportBench --houdini`: colour, depth and ids mapped every frame, the scene camera with shutter -0.25 to 0.25), 1866x737, moving camera, 6 ms standing in for Houdini's own work, 300 frames: macbeth 8.0 ms median (p95 9.0, p99 14.0, camera-to-image latency 10.1 ms; Eldr 1.1 ms), step4 8.2 ms (p95 9.2, p99 9.6, latency 10.7). Without `eldr:inPlaceReadback`: macbeth 14.2 ms, step4 14.4 ms. Without `eldr:overlapReadback`: macbeth 14.6 ms, step4 15.5 ms. usdview (GPU sharing, no readback), 1280x720: about 15 ms (Kitchen_set), 11 ms (step4). |
| Overlapped readback | While the camera moves, Eldr returns as soon as a frame is submitted and hands it over at the next one (`eldr:overlapReadback`): Houdini shows the frame before, a still camera's frame is its own. Only for colour, depth and ids read back into the same buffers as last frame; extra AOVs, products and husk read at once. `viewportBench --overlap` (ctest `viewport_overlap`) checks the frames arrive unchanged. |
| Readback in place | Houdini maps colour, depth, primId and instanceId every frame: 27 MB at 1866x737. With `eldr:inPlaceReadback` (on) the buffers map the frame where the GPU wrote it (two buffers in turn, so Houdini reads one while the next frame renders into the other) and the linear depth conversion runs in that memory. Without it the CPU copies them, 5 to 8 ms a frame (memory bound), with hitches to 30 ms. What remains is the GPU's copy to host memory (4.6 ms of macbeth's 6 GPU ms, overlapped while moving) and Houdini's own upload of the four buffers. |
| Frame pacing | Houdini's free camera has a shutter (-0.25 to 0.25) but no time samples: Eldr takes it as sampled motion without camera motion, so tumbling isn't motion blurred (no `motionBlur` pass); moving objects still are. Dropping out of a scene camera into it doesn't carry that camera's pose into the shutter (`tests/test_camera_switch.py`). While tumbling Eldr renders at `eldr:movingRenderScale` (0.5) and TAA upscales. `houdini:interactive` (tumbling) isn't used, and its flip on mouse release doesn't count as a settings change. |
| Moving camera mode | `viewportBench --houdini` at 1866x737, moving, p50 (p95), with the moving camera mode vs at full quality: Kitchen 5.9 (6.5) vs 10.1 (11.7) ms, crowd_9k 26.4 (27.2) vs 62.1 (66.8) ms, emissive with GI, SSGI, traced shadows and reflections 6.8 (7.5) vs 25.4 (27.4) ms. Full quality returns once the camera has been still for `eldr:stillDelay` (0.3 s; 0: the first still frame), stepping up over `eldr:rampFrames` (6 frames), each step a resize of about 3 ms CPU. crowd_9k's first still frame: 79 ms at stillDelay 0, 46 ms at 0.3. Readback stays 2.7 ms GPU (full size). |
| Frame timing CSV | `HDVK_TIMING=2` writes a row per frame to `HDVK_TIMING_CSV` (else `%TEMP%\eldr_timing_<unix time>.csv`; the path is printed to the console at the first frame): `interval_ms` between frames, `app_ms` Houdini's time between Eldr's executes (its sync, maps, upload and drawing), Eldr's `setup_ms`, `collect_ms` (a moving frame's handover), `wait_ms` (for the GPU), `render_ms` (recording and submitting), `after_ms` (depth conversion, products), `execute_ms`, `fetch_ms` (buffers read back lazily in Houdini's maps), `gpu_ms`, and whether the frame moved, was deferred or collected. Any `HDVK_TIMING` also times teardown. `HDVK_TIMING=1` prints startup laps: device, the renderer's construction, the first three frames' passes over 5 ms (pipelines compile on first use). Then the time Houdini spent in Eldr between two executes, with call counts: `stats_ms` (GetRenderStats), `map_ms` (render buffer maps), `alloc_ms`, `isconverged_ms`, `camsync_ms`, `sync_ms` (meshes, lights, materials, instancers), and `app_outside_eldr_ms`, the rest of `app_ms`: Houdini's own. After it: `sync_wall_ms` (first to last prim sync, wall clock; `sync_ms` is thread time), the thread time of a mesh sync's parts (`mesh_topology_ms` ... `mesh_share_ms`; Hydra 2.0 evaluates UsdSkel lazily in them, so `mesh_skin_ms` and `mesh_shutter_ms` are mostly UsdSkel's) and `shutter_hits`, shutter samples shared between prims (an instancer's for its prototypes, a skeleton's pose for its meshes). `HDVK_TIMING` also prints the readback memory's type once (host cached: Houdini reads it at memory speed; on the GTX 1080 Ti type 9, flags 0xe). `HDVK_NO_STATS=1` makes GetRenderStats return the renderer name only (for comparison). |
| Viewport stats while moving | A moving frame reads "TAA interactive (moving)". Each frame renders one sample (`eldr:interactiveSamples` only cycles the noise pattern over 8 frames). macbeth's moving frame: 6 ms GPU, of which 4.6 copying to the CPU. |
| Still frames | Houdini binds normal and st AOVs it doesn't read. With `eldr:lazyExtraAovs` they render once, when the film has converged (one more frame), not every still frame: macbeth 1866x737 accumulating the film takes 16 ms a frame (GPU 6.9) with it, 52 ms (GPU 19.2, of which the AOV pass 8.1) without. Film frames cost more than moving ones: they wait for their own frame (no overlap) and add the film pass. |
| Guides and depth | With `eldr:overlapReadback` a moving camera's colour and depth are the frame before, while Houdini draws its guides with the current camera: guides slide over the image and depth-test against the old depth by one frame of motion, about 47 px per degree turned between redraws for macbeth's camera at 1866 px wide (12 px at the bench's 0.25 degree a frame). Off: aligned, macbeth 14.6 instead of 8.0 ms a frame and step4 15.5 instead of 8.2 (6 ms of Houdini work), camera-to-image latency 8.6 instead of 10.1 ms. Depth and ids are jittered (TAA, half a pixel) only while moving; once still they settle within two frames (the film keeps the sample nearest the pixel centre): 9845 then 1950 then 0 pixels change. |
| Startup | macbeth 1866x737 (`viewportBench --houdini --startup`; a new exe name makes the NVIDIA driver's shader cache cold, an empty `HDVK_PIPELINE_CACHE_DIR` Eldr's): cold, plugin to converged 9.6 s (delegate 2.1, first frame 6.1: shading 4.8, AOVs 0.9). Warm: 2.2 s (delegate 0.55 of which the Vulkan device 0.3-0.4, first frame 0.5, then 64 film frames). `eldr:lazyPipelines` compiles only the shading variants drawn; volume pipelines wait for their first use; ray tracing pipelines (one per ray generation shader: pixels, probes, refraction, photons) compile only once a ray traced setting is on (`eldr:rtShadows`, `eldr:rtShadowsAuto`, `eldr:rtReflections`, `eldr:rtRefraction`, `eldr:gi`, `eldr:caustics`), on threads: pixels and probes from then, refraction and photons once the scene also has glass or caustic casters and lights (a scene without them never compiles those), and a frame waits only for the ones it uses (`HDVK_RT_EARLY_COMPILE=0`: compiled on first use). Cold driver cache, final settings, 1920x1080: first frame 9.5-10 s (pixels 7.5-8 s; probes 0.9, photons 2.0, refraction 12.5 compile alongside). Every pipeline goes through Eldr's cache file (`%LOCALAPPDATA%\Eldr\pipelines-*.bin`, one file per build: the cache key includes the size and time of the module holding the shaders, so usdview's and Houdini's hdVk.dll each have their own, and the first run after a rebuild starts cold). |
| Render Outputs menu | The viewport's Render Outputs menu lists the vars of the Render Settings LOP's products (Render Var LOP, then Render Product LOP, then Render Settings LOP). The viewport binds a var by its prim name (`rendervar1`) without the source; Eldr looks the name up in the active settings prim's products and uses that var's `sourceName`/`sourceType` (also for the buffer format it is asked about), so a var picked there fills like it does in husk. Put a beauty var (`C.*[LO]`, color4f) in the product first: Houdini shows only the product's vars, so without one there is no `color` entry (Karma does the same). Supported sources: the "Supported LPEs" list above, light groups included; other LPEs stay black (per-object LPEs, `unoccluded;`). Tested headlessly (ctest `aovs`: a var bound by name alone); the menu itself is not clicked through. |
| Viewport channel buttons (RGB, R, G, B, A, L) | Do nothing in Eldr's view as far as known. Houdini's Python API, HDAs, config and help don't expose or describe them for Hydra delegates (the help only mentions channel toggles for the Copernicus and Unreal viewers), so they are applied inside Houdini's compiled viewport, and nothing a delegate reports (AOV format, colour space, settings) is documented as controlling them. Eldr gives the same Float16Vec4 colour as the descriptor asks for. Not verified in the GUI (no Karma comparison made): use Render Outputs to view single AOVs, or the Cop viewer for channels. |

## Not checked

- The Solaris viewport is tested and works with Eldr. Not measured in detail: navigation speed against Karma's, the outline.
- The viewport's refine level with `complexitymultiplier` 1.3 (headless checks reach husk only).
- The Python ctest suite uses `usdenv.cmd` (USD 26.08) and doesn't apply to this build.
