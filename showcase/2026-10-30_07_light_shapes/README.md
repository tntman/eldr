# Light shapes and highlights

<!-- summary -->
Sun, sphere, rect, disk and tube lights give highlights of their own shape, and the shapes can be drawn (`eldr:lightShapesVisible`).

- **Compare:** open `showcase/2026-10-30_07_light_shapes/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `shapes` vs `shapesVisible`.
- **Look at:** The highlights on the glossy floor: a streak for the tube, a stretched rectangle for the rect; with shapes visible, which light gives which.
<!-- summary -->

A 4 degree sun low ahead, a blue tube (cylinder light), a warm sphere, a rect panel and a green disk over a glossy floor (roughness 0.15), seen at grazing angles (`tests/scenes/light_reflections.usda` plus three lights). Each highlight takes its light's shape: the tube's a streak, the rect's a stretched box, the sphere's and disk's stretched along the view. Highlights are energy-conserving: reflected power, peak and shape match a brute-force GGX integral within 5%, as Karma.

## What to look at

- `/Render/shapes` (the default): light shapes hidden from the camera; only their highlights show.
- `/Render/shapesVisible` (`eldr:lightShapesVisible`): the shapes themselves drawn, so you see which light gives which highlight. One light can opt out with `inputs:karma:light:renderlightgeo = 0`.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_07_light_shapes\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/shapes scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
