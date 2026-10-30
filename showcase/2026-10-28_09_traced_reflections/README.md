# Ray-traced reflections

<!-- summary -->
Reflections traced against the whole scene, including what is behind the camera or off screen (`eldr:rtReflections`).

- **Compare:** open `showcase/2026-10-28_09_traced_reflections/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `traced` vs `screenSpace` (`eldr:rtReflections` off), `tracedNoHitShadows` (`eldr:rtHitShadows` off).
- **Look at:** The chrome ball: traced, it shows the room behind the camera and the walls the screen cannot see; screen-space, patches stay dark. Without hit shadows the floor seen in the ball is unshadowed.
<!-- summary -->

`tests/scenes/emissive.usda`: the Cornell box lit by a glowing panel and a bulb, a chrome ball, no dome.

- `/Render/traced`: `eldr:rtReflections` with `eldr:rtHitShadows` on: the ball reflects the whole room, and the floor in it is shadowed.
- `/Render/screenSpace`: `eldr:rtReflections` off (the default): screen-space reflections and the probe.
- `/Render/tracedNoHitShadows`: traced, but reflected surfaces are shaded without shadows: cheaper, the floor in the ball is lit under the box.

Ray tracing runs on a compute fallback on GPUs without RT cores, so it costs more there.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_09_traced_reflections\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-28_09_traced_reflections/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
