# Traced light sampling

<!-- summary -->
Traced hits sample one light stochastically and traced shadows skip lights that give a pixel nothing (`eldr:rtLightSampling`).

- **Compare:** open `showcase/2026-10-30_04_light_sampling/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `sampled` vs `everyLight`.
- **Look at:** After 64 samples the same image, about twice as fast per sample.
<!-- summary -->

In the RT recipe (`eldr_final`, or the prims here), traced shadow rays cost the most where many lights shine on the scene. emissive.usda turns its panel and bulb into several lights. With `eldr:rtLightSampling` (on), each hit of a reflection, refraction or final gather ray traces one shadow ray. It goes to a light picked in proportion to its unshadowed light and is weighted 1/p. Off, every light gets a ray, up to 16. The per-pixel traced shadows skip lights that give the pixel nothing (out of range, outside the cone, or light-linked away). From the film's second sample, scenes with more than 4 traced lights trace each light per 8x8 block in 4/lights of the samples, weighted to match.

What to look at: render `/Render/sampled` (the defaults) and `/Render/everyLight` (light sampling and the gather roulette off). After 64 samples they match within noise. The first is about twice as fast per sample.

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_04_light_sampling\scene.usda`, then pick a settings prim, or flip Ray-Traced Light Sampling in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/sampled scene.usda`.

GTX 1080 Ti, 1920x1080, RT recipe, ms per sample (on / off): emissive 78 / 147, Kitchen with dome 158 / 195, crowd_9k 478 / 542. RMSE at 64 samples against a 1024-sample render (on / off): emissive 0.0074 / 0.0070, Kitchen with dome 0.0089 / 0.0088.

`eldr:finalGatherRoulette` (off) also halves the gather rays per sample (8x8 blocks in turn, weighted by 2). It saves 13-19% per sample but raises RMSE at 64 samples 1.4-1.5x, so it is off by default.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_04_light_sampling\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-30_04_light_sampling/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
