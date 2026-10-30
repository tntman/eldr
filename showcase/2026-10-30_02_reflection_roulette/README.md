# Traced reflection roulette

With `eldr:rtReflections` on (as in `eldr_final`), traced reflections cost the most per sample (crowd_9k: 86% of a sample). From the film's second sample, `eldr:rtReflectionRoulette` (on) traces dim reflections in only some of the samples. A reflection counts as dim when its Fresnel is low, as on dielectrics seen head-on. Each one traces with probability 3 x Fresnel (at least 1/8; near-mirrors under roughness 0.2 more), decided per 8x8 block, and weighs 1 over that share. The chrome ball, grazing views and the sharpest floor still trace every sample. Surfaces with specular weight 0 trace none.

What to look at: render `/Render/roulette` (the default) and `/Render/everySample`. After 64 samples they match within noise. In the first few samples the polished floor's reflections near the camera show faint 8x8 blocks.

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_02_reflection_roulette\scene.usda`, pick a settings prim, or flip Ray-Traced Reflection Roulette in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/roulette scene.usda`.

GTX 1080 Ti, 1920x1080, traced reflections on, ms per sample (on / off):
- crowd_9k: 146 / 249
- Kitchen with dome: 57 / 73
- Kitchen: 34 / 49

RMSE at 64 samples against 1024 samples with the old code:
- crowd and emissive: unchanged
- Kitchen with dome: 0.00219 against 0.00195
- this scene: 0.00080 against 0.00023 (its floor's sharp reflection converges very fast without the roulette)

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
