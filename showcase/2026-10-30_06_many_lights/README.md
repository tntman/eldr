# Many lights (clustered)

200 small coloured sphere lights hang just above a glossy floor around a ball, a box and a post, under a faint night dome. Clustered shading (63 lights per cluster, cut off at 1% of mid-gray on a white surface) lights each pixel with only the lights near it; every light still gets a shadow (atlas tiles halve before any light goes unshadowed).

## What to look at

- `/Render/lights` (the default): 200 coloured pools of light and 200 small highlights on the floor.
- `/Render/clustersOff` (`eldr:clusteredLights` off): every light for every pixel: the same picture, slower (compare the frame time in Eldr > Render Statistics).
- `/Render/localLightsOff` (`eldr:localLights` off): no local lights, only the blue night dome: nearly black.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_06_many_lights\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/lights scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
