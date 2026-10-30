# Emitters, shadows, reflections

`tests/scenes/emissive.usda`: the Cornell box lit only by a glowing panel under the ceiling and a small bulb on the floor, with a chrome ball.

- `/Render/default`: the defaults (shadow maps, emissive lights on).
- `/Render/rtShadows`: `eldr:rtShadows` on. Look under the ball and the box: the difference is subtle (contact shadows without the map's bias).
- `/Render/rtReflectionsHitShadows2` vs `...0`: `eldr:rtReflections` with `eldr:rtHitShadows` 2 (always) or 0. The floor seen in the chrome ball is shadowed with 2.
- `/Render/shadowMapsOnly`: shadow maps only (the default, so it equals `default`); the shadow near the panel is soft.
- `/Render/emissiveLightsOff` vs `...On`: `eldr:emissiveLights`; with it the panel's edges light the ceiling.

Ray tracing runs on a compute fallback on GPUs without RT cores, so the traced settings cost more there.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_03_emitters_shadows_reflections\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_03_emitters_shadows_reflections/scene.usda`

Settings prims: `/Render/default`, `/Render/rtShadows`, `/Render/rtReflectionsHitShadows2`, `/Render/rtReflectionsHitShadows0`, `/Render/shadowMapsOnly`, `/Render/emissiveLightsOff`, `/Render/emissiveLightsOn`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
