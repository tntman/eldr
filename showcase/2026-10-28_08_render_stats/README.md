# Render statistics in Houdini's Render Gallery

`scene.usda` sublayers the Cornell box with a `/Render/default` settings prim and one beauty product (`renderStats.exr`). It has no A/B toggles: the feature is the `husk:render_stats` metadata of Eldr's EXRs and the `statsdatapaths` of `houdini/UsdRenderers.json`.

**Viewport snapshot.** In a Solaris viewport with Eldr as the renderer, save a snapshot to the Render Gallery (Render Gallery > snapshot button). Select its tile and open the statistics panel: renderer "Eldr", stage, progress, Memory and Peak Memory, render time.

**Background Render / husk EXR.** Render the scene with a Karma/USD Render ROP or a Render Gallery Background Render using Eldr as the delegate, or from a shell:

    husk -R HdVkRendererPlugin -s /Render/default -o out.exr scene.usda

The image's tile in the Render Gallery shows the same panel.

**See the data.** `iinfo -J out.exr` (in `$HFS/bin`) prints the image's metadata; the stats are `aovs[0].metadata."husk:render_stats"`, a JSON object with `__delegate` (`HdVkRendererPlugin`), `rendererName`, `rendererStage`, `percentDone`, `totalClockTime`, `totalMemory`, `peakMemory`, `trianglesDrawn`, `gpuTimeMs`, `gpuPassMs` and the rest of `GetRenderStats()`. Compare with `-R BRAY_HdKarma`.

Verified headlessly (husk, `iinfo -J`, the panel's own `normalize_stats` and `findStats` in hython); the panel's look in the Houdini GUI was not checked.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
