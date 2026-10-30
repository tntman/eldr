# Instancing

<!-- summary -->
Nested point instancers and native instances, with per-instance culling by screen size (`eldr:cullPixels`).

- **Compare:** open `showcase/2026-10-30_20_instancing/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `instances` vs `cullSmall`.
- **Look at:** The far rows of trees thin out with `cullSmall`.
<!-- summary -->

A PointInstancer of 12 x 12 "trees" whose prototype holds a trunk and a second, nested PointInstancer of three leaf balls, with per-instance scales; in front on the left three native USD instances (instanceable references of one rock). Each prototype draws once per pass (one indirect draw per prototype) however many times it appears; frustum and small-object culling run per instance on the GPU.

## What to look at

- `/Render/instances` (the default): all 144 trees and 432 leaf balls, far ones included (`eldr:cullPixels` 0.5).
- `/Render/cullSmall` (`eldr:cullPixels` 30): instances under 30 pixels on screen are skipped: the far rows thin out.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_20_instancing\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/instances scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
