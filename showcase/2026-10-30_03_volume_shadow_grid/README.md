# Volume shadow grids (2026-10-30)

tests/scenes/volume.usda's three volumes (sun, dome) with three sphere lights. Each volume's optical
depth towards the dome, the sun and (for mutual shadows) the sphere lights is built once on a 64^3 grid
per light and read trilinearly in the march, instead of a shadow march per step and light.

- `/Render/grid`: the default, `eldr:volumeShadowGrid` 64.
- `/Render/exact`: `eldr:volumeShadowGrid` 0 marches every shadow ray, as before.
- `/Render/grid32`: a coarser grid.

All three should look the same (57-60 dB PSNR). At 1920x1080 with the final-render settings (`eldr_final`) the volume pass drops from
234 to 77 ms per sample.

Open: `usdenv usdview --renderer Eldr showcase\2026-10-30_03_volume_shadow_grid\scene.usda` and pick a
settings prim, or `husk -R Eldr -s /Render/exact scene.usda`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
