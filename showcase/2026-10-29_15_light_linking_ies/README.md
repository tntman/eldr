# Light linking and IES profiles

Three balls on a floor (`tests/scenes/lightlink.usda`) under three lights:

- `key` (front left, above): its `collection:lightLink` excludes `/balls/right`, so the right ball gets none of its light (it still casts the key's shadow).
- `fill` (blue, from the right): its `collection:shadowLink` excludes `/balls/left`, so the left ball casts no fill shadow onto the floor; the other two do.
- `iesSpot` (warm, above the gap between the middle and right balls): `inputs:shaping:ies:file` is `tests/scenes/textures/spot_asym.ies`, a 40 degree spot that is brighter towards the light's +X: its pool on the floor leans right.

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
