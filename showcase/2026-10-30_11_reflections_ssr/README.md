# Screen-space reflections and the reflection probe

`tests/scenes/reflections.usda` (columns, an arch and a chrome ball on a polished floor of roughness 0.08, a red wall behind the camera) plus a rougher strip (roughness 0.35) over the right half of the floor. SSR is a 32-step ray march of last frame's image; it fades out with roughness (0.3 to 0.6) and at the screen's edges, where the dome (or a probe) takes over.

## What to look at

- `/Render/ssr` (the default): the columns and the ball mirrored in the polished floor; blurrier and fainter in the rough strip.
- `/Render/ssrOff` (`eldr:ssr` off): only the dome in the floor.
- `/Render/probe` (`eldr:reflectionProbe`): a captured cube: the chrome ball shows the red wall behind the camera, which SSR can't see.
- `eldr_final` (traced reflections) sees everything, on and off screen.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_11_reflections_ssr\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/ssr scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
