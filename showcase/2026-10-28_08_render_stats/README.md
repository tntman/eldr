# Render statistics in Houdini's Render Gallery

`scene.usda` sublayers the Cornell box with a `/Render/default` settings prim and one beauty product (`renderStats.exr`). It has no A/B toggles: the feature is the `husk:render_stats` metadata of Eldr's EXRs and the `statsdatapaths` of `houdini/UsdRenderers.json`.

**Viewport snapshot.** In a Solaris viewport with Eldr as the renderer, save a snapshot to the Render Gallery (Render Gallery > snapshot button). Select its tile and open the statistics panel: renderer "Eldr", stage, progress, Memory and Peak Memory, render time.

**Background Render / husk EXR.** Render the scene with a Karma/USD Render ROP or a Render Gallery Background Render using Eldr as the delegate, or from a shell:

    husk -R HdVkRendererPlugin -s /Render/default -o out.exr scene.usda

The image's tile in the Render Gallery shows the same panel.

**See the data.** `iinfo -J out.exr` (in `$HFS/bin`) prints the image's metadata; the stats are `aovs[0].metadata."husk:render_stats"`, a JSON object with `__delegate` (`HdVkRendererPlugin`), `rendererName`, `rendererStage`, `percentDone`, `totalClockTime`, `totalMemory`, `peakMemory`, `trianglesDrawn`, `gpuTimeMs`, `gpuPassMs` and the rest of `GetRenderStats()`. Compare with `-R BRAY_HdKarma`.

Verified headlessly (husk, `iinfo -J`, the panel's own `normalize_stats` and `findStats` in hython); the panel's look in the Houdini GUI was not checked.
