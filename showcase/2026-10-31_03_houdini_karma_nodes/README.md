# Houdini materials with Karma's nodes

<!-- summary -->
Houdini's basic materials that use Karma's own nodes (Ramp Const, Hex-Tiled Texture, Voronoi Noise 3D) render as in Karma, and juice absorbs over its path through the ball.

- **Compare:** open `showcase/2026-10-31_03_houdini_karma_nodes/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `graphsOn` vs `graphsOff`.
- **Look at:** The plastic's hex-tiled grain with no repeat grid, the silk's sheen at the edges, the marble's veins, the car paint's blue under its flakes, the apple juice a dark amber that lets red through.
<!-- summary -->

Five spheres with materials from Houdini 22's `basic_materials` library (`$HFS/houdini/usd/materials`): Plastic (`kma_color_hextiling`), SilkSurfaceGreen and Marble (`kma_rampconst`), CarPaintMetallic (`kma_voronoinoise3d` flakes as a bump under a coat, `kma_camerafalloff` colour), AppleJuice (transmission with `transmission_depth`). The library path is Houdini 22.0.466's default install; edit the `Materials` reference for another one. Eldr reads Houdini's MaterialX library (`kma_camerafalloff` and others) from `$HFS`: Houdini sets it; for usdview and husk outside Houdini, set `HFS` to the Houdini folder.

## What to look at

- `/Render/graphsOn` (the default): each material as Karma renders it (median colour difference 1.6 dE over Houdini's 121 basic and quick materials). The silk is about 1.2x brighter than Karma's.
- `/Render/graphsOff` (`eldr:materialGraphs` off): every graph-driven input keeps its constant: the plastic loses its grain, the silk its edge sheen, the marble its veins.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-31_03_houdini_karma_nodes\scene.usda`, then pick a settings prim (right-click, set as active), or flip the setting in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/graphsOn scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
