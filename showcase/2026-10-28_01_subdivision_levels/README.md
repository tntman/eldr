# Subdivision levels

<!-- summary -->
Subdivision surfaces are smoothed to the host's level, and a mesh can set its own level or opt in.
<!-- summary -->

Five cubes show how smoothly each is refined: the default, one capped at level 1, a plain polygon cage, a polygon cube opted in to smoothing, and one limited by Karma's dicing depth.

- **level3**: every cube refined to level 3; the smooth ones are round.
- **level1**: the cap is level 1; all cubes look faceted.
- **subdivOff**: subdivision off; every cube is a plain cage.
- **eldr_default / eldr_final / karma**: Eldr with nothing set, Eldr's final settings, Karma to compare.

Husk's `--complexity` and usdview's Complexity menu set the level.

## Open

Houdini: `scene.hip`. usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_01_subdivision_levels\scene.usda`, then pick a settings prim. husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-28_01_subdivision_levels/scene.usda`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
