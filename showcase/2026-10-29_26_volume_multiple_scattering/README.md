# Volume multiple scattering

<!-- summary -->
Light bouncing more than once inside smoke, set with Karma's Volume Limit (`karma:object:volumelimit`) and fitted to Karma's gain.

- **Compare:** open `showcase/2026-10-29_26_volume_multiple_scattering/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `volumeLimit1` vs `volumeLimit4` vs `volumeLimit16`.
- **Look at:** The clouds get brighter and softer with more bounces (within 7% of Karma at limits 1, 4 and 16).
<!-- summary -->

`tests/scenes/volume.usda`'s smoke clouds (no material) under the sun and a dome.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_26_volume_multiple_scattering\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_26_volume_multiple_scattering/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
