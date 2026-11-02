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

Open `scene.hip`. A Sublayer LOP loads `geo.usda`, the scene's geometry only. Everything else is a Houdini node you can change: the camera (Camera LOP), each light (Light, Distant Light, Dome Light LOPs), the materials (a Material Library with MaterialX, USD Preview Surface and Karma VOPs), render products and AOVs (Render Product, Render Var LOPs), and one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

A value a node has no parameter for sits on the node as an extra parameter, or on an Edit Properties LOP right after it. A material Houdini's VOPs can't rebuild exactly comes from `scene.usda` through a Reference LOP.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). `scene.usda` holds the same scene for usdview and husk (it sublayers `geo.usda`): `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda` (`--check` compares the hip's stage with `scene.usda`).
