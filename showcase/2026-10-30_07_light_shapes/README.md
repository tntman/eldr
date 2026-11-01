# Light shapes and highlights

<!-- summary -->
Sun, sphere, rect, disk and tube lights give highlights of their own shape, and lights set to show their shape appear to the camera.
<!-- summary -->

A sun, a blue tube, a warm sphere, a rect panel and a green disk over a glossy floor, seen at grazing angles. The tube gives a streak, the rect a stretched rectangle.

- **shapes**: all light shapes hidden; only the highlights show.
- **shapesVisible**: the lights themselves are drawn, so you can see which light makes which highlight.
- **eldr_default / eldr_final / karma**: Eldr with nothing set, Eldr's final settings, Karma to compare.

## Open

Houdini: `scene.hip`. usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_07_light_shapes\scene.usda`, then pick a settings prim. husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-30_07_light_shapes/scene.usda`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
