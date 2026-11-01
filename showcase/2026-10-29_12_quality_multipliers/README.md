# Quality per effect

<!-- summary -->
Spend more rays on the noisy part of the image (diffuse, light, reflection) instead of more film samples, as Karma's quality controls do.
<!-- summary -->

A Cornell box with a mirror ball at 32 film samples. Most of the noise comes from the bounced light, so diffuse quality helps most.

- **default**: every quality at 1.
- **diffuse2**: twice the rays for bounced light; the walls and floor are cleaner.
- **diffuse2LightHalf**: double diffuse rays, half the rays for shadows and reflections.
- **karmaNames**: the same as diffuse2LightHalf, set with Karma's names; the image is identical.
- **allHalf**: half the rays everywhere; faster and noisier.
- **eldr_default / eldr_final / karma**: Eldr with nothing set, Eldr's final settings, Karma to compare.

## Open

Houdini: `scene.hip`. usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_12_quality_multipliers\scene.usda`, then pick a settings prim. husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_12_quality_multipliers/scene.usda`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
