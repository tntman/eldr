# IES profiles

<!-- summary -->
A light's IES profile (`inputs:shaping:ies:file`) shapes its light, as in Karma (`eldr:lightIes`).

- **Compare:** open `showcase/2026-10-29_24_ies_profiles/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `ies` vs `noIes`.
- **Look at:** The warm pool of light between the middle and right balls: a spot leaning left with the profile, a plain sphere light lighting the whole floor without.
<!-- summary -->

`tests/scenes/lightlink.usda`'s three balls plus `iesSpot`: a warm sphere light with `tests/scenes/textures/spot_asym.ies`, a 40 degree spot brighter towards phi 0 (the light's -X, as in Karma). The scene's light links stay on.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_24_ies_profiles\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_24_ies_profiles/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
