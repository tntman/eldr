# Ambient light and occlusion

A white corner (floor and two walls), stacked boxes and a ball against the wall, lit only by a plain white dome (no texture, so no sun to extract). Ambient light is the dome's unoccluded light darkened by GTAO, screen-space ambient occlusion over `eldr:aoRadius` (0.5); with SSGI on, its traced visibility replaces it; under the final gather, traced rays do.

## What to look at

- `/Render/ambient` (the default): AO darkens the corner, the gaps between the boxes and under the ball.
- `/Render/aoOff` (`eldr:ambientOcclusion` off): flat sky light everywhere.
- `/Render/aoWide` (`eldr:aoRadius` 2): wider, softer darkening.
- `eldr_final` (final gather) shows the traced version of the same occlusion.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_09_ambient\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/ambient scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
