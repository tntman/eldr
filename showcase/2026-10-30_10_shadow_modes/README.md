# Shadow modes

<!-- summary -->
Shadows from maps (sun cascades, light atlas) softened by PCSS and jitter, hard PCF, or traced (`eldr:shadowJitter`, `eldr:softShadows`, `eldr:rtShadows`).

- **Compare:** open `showcase/2026-10-30_10_shadow_modes/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `maps` (default) vs `pcss` vs `hard` vs `traced`.
- **Look at:** The contact hardening: sharp where the posts meet the ground, soft under the beam and the sphere light.
<!-- summary -->

`tests/scenes/sunangle.usda` (a 4 degree sun over two posts, a beam and a ball) plus a warm sphere light of radius 0.3 low on the right, so both the sun's cascades and the local light atlas show. Look at the contact hardening: sharp where the posts meet the ground, soft under the beam, and the sphere light's soft shadow of the ball.

## What to look at

- `/Render/maps` (the default): shadow maps (4 sun cascades, atlas tiles for the sphere light), soft from the light's size (PCSS), exact penumbrae from maps jittered over the light every film sample (`eldr:shadowJitter`).
- `/Render/pcss` (`eldr:shadowJitter` off): the same maps, PCSS only: penumbra widths estimated.
- `/Render/hard` (`eldr:softShadows`, `eldr:softSunShadows` off): 3x3 PCF: hard edges everywhere.
- `/Render/traced` (`eldr:rtShadows`): ray-traced shadows, soft from the light's size, denoised over the film's first samples.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_10_shadow_modes\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/maps scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
