# Denoising during the first samples

<!-- summary -->
Bounced light, traced shadows and traced reflections are filtered while few film samples exist, so early images are clean.
<!-- summary -->

A Cornell box with a chrome ball. Look at the soft shadow under the box and the walls' bounce light.

- **raw4 / raw16 / raw64**: 4, 16 or 64 film samples, no filtering; noisy.
- **denoise4 / denoise16 / denoise64**: the same sample counts, filtered; cleaner.
- **noFade64**: filtering stays on for every sample; smooth, but shadow edges and corners stay slightly soft.
- **eldr_default / eldr_final / karma**: Eldr with nothing set, Eldr's final settings, Karma to compare.

The filters fade out over the first 64 samples, so a converged image stays sharp.

## Open

Houdini: `scene.hip`. usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_11_effect_denoise\scene.usda`, then pick a settings prim. husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_11_effect_denoise/scene.usda`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
