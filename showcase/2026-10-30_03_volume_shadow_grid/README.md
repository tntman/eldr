# Volume shadow grids (2026-10-30)

tests/scenes/volume.usda's three volumes (sun, dome) with three sphere lights. Each volume's optical
depth towards the dome, the sun and (for mutual shadows) the sphere lights is built once on a 64^3 grid
per light and read trilinearly in the march, instead of a shadow march per step and light.

- `/Render/grid`: the default, `eldr:volumeShadowGrid` 64.
- `/Render/exact`: `eldr:volumeShadowGrid` 0 marches every shadow ray, as before.
- `/Render/grid32`: a coarser grid.

All three should look the same (57-60 dB PSNR). At 1920x1080 final quality the volume pass drops from
234 to 77 ms per sample.

Open: `usdenv usdview --renderer Eldr showcase\2026-10-30_03_volume_shadow_grid\scene.usda` and pick a
settings prim, or `husk -R Eldr -s /Render/exact scene.usda`.
