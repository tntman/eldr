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

Settings prims: `/Render/default`, `/Render/rtShadows`, `/Render/rtReflectionsHitShadows2`, `/Render/rtReflectionsHitShadows0`, `/Render/shadowMapsOnly`, `/Render/emissiveLightsOff`, `/Render/emissiveLightsOn`.
