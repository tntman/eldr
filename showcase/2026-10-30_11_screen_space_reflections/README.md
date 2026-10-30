# Screen-space reflections

<!-- summary -->
Reflections ray-marched in the last frame's image, fading out with roughness (`eldr:ssr`).

- **Compare:** open `showcase/2026-10-30_11_screen_space_reflections/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `ssr` vs `ssrOff`.
- **Look at:** The columns and the ball mirrored in the polished floor, blurrier in the rough strip; only the dome without.
<!-- summary -->

`tests/scenes/reflections.usda` (columns, an arch and a chrome ball on a polished floor of roughness 0.08) plus a rougher strip (roughness 0.35) over the right half. SSR is a 32-step march of last frame's image; it fades out between roughness 0.3 and 0.6, and where it has no hit the dome (or the reflection probe, 2026-10-30_23_reflection_probe) is reflected. `eldr_final` (traced reflections) sees everything, on and off screen.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_11_screen_space_reflections\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-30_11_screen_space_reflections/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
