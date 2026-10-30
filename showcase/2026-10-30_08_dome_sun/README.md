# Dome sun

<!-- summary -->
The HDRI's sun is taken out of the dome and cast as a shadowed distant light (`eldr:domeSun`).

- **Compare:** open `showcase/2026-10-30_08_dome_sun/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `domeSun` vs `domeSunOff`.
- **Look at:** Crisp sun shadows on the ground and the sun's glint in the chrome; none without.
<!-- summary -->

`tests/scenes/step4.usda` (chrome, gold and matte objects on a ground) lit by StinsonBeach.hdr alone; its distant light is set to 0. The HDRI's sun is a few very bright texels: as image light only they give soft, shadowless sunlight. Eldr takes texels brighter than `eldr:domeSunThreshold` (10) out of the dome and lights with them as a shadowed distant light of the same power and direction.

## What to look at

- `/Render/domeSun` (the default): crisp sun shadows from the HDRI, the sun's glint in the chrome.
- `/Render/domeSunOff` (`eldr:domeSun` off): the dome alone: no cast shadows.

StinsonBeach.hdr ships with USD: `usdenv.cmd` puts its folder on the resolver's search path. For husk set `PXR_AR_DEFAULT_SEARCH_PATH` to `<USD>\lib\usd\hdx\resources\textures` (Houdini's own USD has the same file).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_08_dome_sun\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/domeSun scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
