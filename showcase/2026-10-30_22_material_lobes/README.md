# Material lobes

<!-- summary -->
Clearcoat, sheen, fuzz, thin film, anisotropic highlights and hair, plus toon shading.

- **Compare:** open `showcase/2026-10-30_22_material_lobes/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `lobes` vs `lobesOff`, `toon` (`eldr:toonBands` 3).
- **Look at:** The coats, sheen, iridescence and stretched highlights vanish with lobes off.
<!-- summary -->

`tests/scenes/lobes.usda`: one sphere per lobe under a sun and StinsonBeach.hdr. Top row: specular workflow, clearcoat, car paint, OpenPBR coat, velvet sheen, OpenPBR fuzz. Bottom row: thin film (oil slick), anodized metal, brushed steel and brushed gold (anisotropic), toon (per material), a plain reference. Third row: hair curves, textured specular F0, textured thin film, textured transmission. Each lobe has its own pipeline: plain materials don't pay for it.

## What to look at

- `/Render/lobes` (the default): every lobe on.
- `/Render/lobesOff` (`eldr:clearcoat`, `eldr:sheen`, `eldr:thinFilm`, `eldr:anisotropicSpecular`, `eldr:multiScatter` off): coats, sheen, iridescence and stretched highlights vanish; rough metals darken a little (single scattering).
- `/Render/toon` (`eldr:toonBands` 3): every material shaded in three flat bands.

StinsonBeach.hdr: see showcase 2026-10-30_08_dome_sun's README for the search path.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_22_material_lobes\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/lobes scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
