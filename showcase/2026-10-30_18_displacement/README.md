# Displacement

`tests/scenes/displacement.usda`. Left: UsdPreviewSurface displacement from a height map (round bumps on five cells of a checker) times 0.6. Middle: the same map through a MaterialX displacement shader. Right: a cube displaced outwards by a constant 0.25. Vertices move along their normals after subdivision (the planes are 16 x 16 quads, diced by their size on screen), so silhouettes, shadows and traced rays agree.

## What to look at

- `/Render/displacement` (the default): bumps rise from the planes and cast shadows; the cube swells.
- `/Render/displacementOff` (`eldr:displacement` off): flat planes, the cube at its authored size.
- `/Render/displacementHalf` (`eldr:displacementScale` 0.5): half height.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_18_displacement\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/displacement scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
