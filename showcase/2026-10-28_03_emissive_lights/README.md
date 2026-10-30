# Emissive meshes as lights

<!-- summary -->
Glowing meshes light the scene as lights do, with shadows (`eldr:emissiveLights`).

- **Compare:** open `showcase/2026-10-28_03_emissive_lights/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `emissiveLightsOn` vs `emissiveLightsOff`.
- **Look at:** The ceiling next to the glowing panel and the floor around the bulb: lit and shadowed with it on, dark without.
<!-- summary -->

The Cornell box of `tests/scenes/emissive.usda`, lit only by a glowing panel under the ceiling and a small bulb on the floor, with a chrome ball.

- `/Render/emissiveLightsOn`: `eldr:emissiveLights` on (the default): the panel's edges light the ceiling, the box and ball cast shadows from the panel and the bulb.
- `/Render/emissiveLightsOff`: off: the emitters only glow; without GI the room stays dark.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_03_emissive_lights\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-28_03_emissive_lights/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
