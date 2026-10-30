# Light linking and IES profiles

Three balls on a floor (`tests/scenes/lightlink.usda`) under three lights:

- `key` (front left, above): its `collection:lightLink` excludes `/balls/right`, so the right ball gets none of its light (it still casts the key's shadow).
- `fill` (blue, from the right): its `collection:shadowLink` excludes `/balls/left`, so the left ball casts no fill shadow onto the floor; the other two do.
- `iesSpot` (warm, above the gap between the middle and right balls): `inputs:shaping:ies:file` is `tests/scenes/textures/spot_asym.ies`, a 40 degree spot that is brighter towards phi 0, the light's -X as in Karma: its pool on the floor leans left.

These are the UsdLux collections Houdini's Light Linker LOP edits (`includeRoot`, `includes`, `excludes` on the light).

## What to look at

- `/Render/default`: the right ball lit only blue (fill) and warm (IES); no blue shadow from the left ball.
- `/Render/noLinking` (`eldr:lightLinking` off): the right ball lit by the key too, the left ball's blue shadow back.
- `/Render/noIes` (`eldr:lightIes` off): the spot becomes a plain sphere light lighting the whole floor.
- `/Render/traced` (`eldr:rtShadows` on): the same links with ray-traced shadows.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_15_light_linking_ies\scene.usda`, then pick a settings prim.
- husk: `husk -R Eldr -s /Render/default showcase/2026-10-29_15_light_linking_ies/scene.usda -o linking.exr`; `-R BRAY_HdKarma` renders Karma's for comparison.
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; a Light Linker LOP after it edits the same collections.

Karma comparisons: `tests/karma_refs.py --scenes lightlink,ies` (numbers in docs/houdini.md).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
