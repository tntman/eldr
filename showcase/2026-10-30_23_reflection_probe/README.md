# Reflection probe

<!-- summary -->
A cube captured around the scene supplies reflections where the screen has none (`eldr:reflectionProbe`).

- **Compare:** open `showcase/2026-10-30_23_reflection_probe/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `probe` vs `probeOff`.
- **Look at:** The chrome ball: it shows the red wall behind the camera with the probe, the dome without.
<!-- summary -->

`tests/scenes/reflections.usda` (columns, an arch and a chrome ball on a polished floor, a red wall behind the camera) plus a rougher strip on the right. Screen-space reflections are on in both prims; the probe fills in what the screen cannot see.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_23_reflection_probe\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-30_23_reflection_probe/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
