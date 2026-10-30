# Light linking

<!-- summary -->
A light can skip objects (light linking) and its shadows can skip casters (shadow linking), from UsdLux collections as Houdini's Light Linker writes them.

- **Compare:** open `showcase/2026-10-29_15_light_linking/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `default` vs `noLinking` (`eldr:lightLinking` off), `traced` (the same links with traced shadows).
- **Look at:** The right ball, lit only blue and warm (the key light skips it), and the floor with no blue shadow from the left ball.
<!-- summary -->

Three balls on a floor (`tests/scenes/lightlink.usda`) under three lights:

- `key` (front left, above): its `collection:lightLink` excludes `/balls/right`, so the right ball gets none of its light (it still casts the key's shadow).
- `fill` (blue, from the right): its `collection:shadowLink` excludes `/balls/left`, so the left ball casts no fill shadow onto the floor.
- `iesSpot` (warm): an IES spot, shown in `2026-10-29_24_ies_profiles`.

These are the UsdLux collections Houdini's Light Linker LOP edits (`includeRoot`, `includes`, `excludes` on the light).

- `/Render/default`: the links on.
- `/Render/noLinking`: the right ball lit by the key too, the left ball's blue shadow back.
- `/Render/traced` (`eldr:rtShadows` on): the same links with ray-traced shadows.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_15_light_linking\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_15_light_linking/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
