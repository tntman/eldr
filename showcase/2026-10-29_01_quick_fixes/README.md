# Quick fixes: colour limit, final-quality reflections, opacityMode, displacement bound

`scene.usda` sublayers `tests/scenes/emissive.usda` (the Cornell box lit by a glowing panel and a bulb, a chrome ball, no dome) and adds four things.

- **Colour limit** (`eldr:colorLimit`, Karma's `karma:global:colorlimit`, default 20): `/Render/colorLimit20` (the default), `colorLimit5`, `colorLimitOff` (0) and `karmaColorLimit5` (Karma's name, same result as `colorLimit5`), all with `eldr:gi`. Look at the walls and ceiling beside the panel: a lower limit darkens what bounce rays see of the panel (radiance 40). `eldr:indirectClamp` overrides the colour limit.
- **Traced reflections in final quality** (`eldr:rtReflections` 1, the default): `/Render/finalReflections` against `finalReflectionsOff`. The chrome ball shows the room behind the camera and the walls the screen cannot see; without it, patches of the ball stay dark. `default` (not final quality) uses screen-space reflections.
- **opacityMode**: the two small 30% opaque dark metal quads at the front left, tilted to mirror the glowing ceiling panel. Left `transparent`, right `presence`: the presence one fades that highlight with the opacity. `/Render/opacityModeOff` shades both as transparent.
- **Displacement bound**: two orange patches at the front right, 3.3 units below the floor and lifted into the room by a constant displacement graph. Only the left one states `inputs:eldr:displacementBound = 3.3` on its displacement shader and shows; the other keeps the graph's default bound of |scale| and is culled (turn off `eldr:frustumCulling` to see it).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_01_quick_fixes\scene.usda`, then pick a settings prim
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_01_quick_fixes/scene.usda`
