# Shadows of further suns

<!-- summary -->
A second distant light, such as the HDRI's extracted sun beside the scene's own sun, casts traced shadows, through smoke too (`eldr:rtExtraSunShadows`).

- **Compare:** open `showcase/2026-10-30_26_further_sun_shadows/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `furtherSuns` vs `furtherSunsOff`.
- **Look at:** Dark, sharp shadows of the clouds and of the smoke ball at the bottom on the floor; a bright floor under the clouds without.
<!-- summary -->

`tests/scenes/volume.usda` (two smoke clouds, an orange ball poking out of one, a floor) lit by a distant light and StinsonBeach.hdr. Eldr takes the HDRI's sun out of the dome as a second distant light (`eldr:domeSun`). Shadow maps cover one sun, the scene's own; every further sun casts traced shadows, which also pass through volumes. The orb and floor get plain UsdPreviewSurface materials, as in Karma's golden comparison.

## What to look at

- `/Render/furtherSuns` (the default): the HDRI's sun shadows the floor under the left cloud and behind the right cloud and the smoke ball, as in Karma.
- `/Render/furtherSunsOff` (`eldr:rtExtraSunShadows` off): the HDRI's sun reaches the floor through clouds and objects.

Traced shadows need a ray-tracing capable GPU; without one, further suns cast no shadow. StinsonBeach.hdr ships with USD: `usdenv.cmd` puts its folder on the resolver's search path. For husk set `PXR_AR_DEFAULT_SEARCH_PATH` to `<USD>\lib\usd\hdx\resources\textures`.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_26_further_sun_shadows\scene.usda`, then pick a settings prim (right-click, set as active), or flip the setting in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/furtherSuns scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
