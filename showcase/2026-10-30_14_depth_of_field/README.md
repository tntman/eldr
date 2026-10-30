# Depth of field

<!-- summary -->
The camera's f-stop and focus distance blur what is out of focus, with true bokeh over the film (`eldr:depthOfField`).

- **Compare:** open `showcase/2026-10-30_14_depth_of_field/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `lens` vs `dofOff`.
- **Look at:** Only the green sphere at 5 m is sharp; everything is sharp with it off. Render frame 1.
<!-- summary -->

`tests/scenes/lens.usda` (centimetres: an 85 mm lens at f/1.4 focused at 5 m on spheres from 2 to 20 m). Under the film the camera moves across the aperture every sample (true bokeh with occlusion). Render frame 1: the orange cube moves from frame 2 on.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_14_depth_of_field\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-30_14_depth_of_field/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
