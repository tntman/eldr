# Quick fixes: colour limit, traced reflections, opacityMode, displacement bound

`scene.usda` sublayers `tests/scenes/emissive.usda` (the Cornell box lit by a glowing panel and a bulb, a chrome ball, no dome) and adds four things.

- **Colour limit** (`eldr:colorLimit`, Karma's `karma:global:colorlimit`, default 20): `/Render/colorLimit20` (the default), `colorLimit5`, `colorLimitOff` (0) and `karmaColorLimit5` (Karma's name, same result as `colorLimit5`), all with `eldr:gi`. Look at the walls and ceiling beside the panel: a lower limit darkens what bounce rays see of the panel (radiance 40). `eldr:indirectClamp` overrides the colour limit.
- **Traced reflections** (`eldr:rtReflections` on; off by default): `/Render/finalReflections` (the final-render settings, 32 film samples) against `finalReflectionsOff` (the same with `eldr:rtReflections` 0). The chrome ball shows the room behind the camera and the walls the screen cannot see; without it, patches of the ball stay dark. `default` (nothing set) uses screen-space reflections.
- **opacityMode**: the two small 30% opaque dark metal quads at the front left, tilted to mirror the glowing ceiling panel. Left `transparent`, right `presence`: the presence one fades that highlight with the opacity. `/Render/opacityModeOff` shades both as transparent.
- **Displacement bound**: two orange patches at the front right, 3.3 units below the floor and lifted into the room by a constant displacement graph. Only the left one states `inputs:eldr:displacementBound = 3.3` on its displacement shader and shows; the other keeps the graph's default bound of |scale| and is culled (turn off `eldr:frustumCulling` to see it).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_01_quick_fixes\scene.usda`, then pick a settings prim
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_01_quick_fixes/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
