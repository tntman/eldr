# Traced reflection roulette

In final quality, traced reflections cost the most per sample (crowd_9k: 86% of a sample). From the film's second sample, `eldr:rtReflectionRoulette` (on) traces dim reflections in only some of the samples. A reflection counts as dim when its Fresnel is low, as on dielectrics seen head-on. Each one traces with probability 3 x Fresnel (at least 1/8; near-mirrors under roughness 0.2 more), decided per 8x8 block, and weighs 1 over that share. The chrome ball, grazing views and the sharpest floor still trace every sample. Surfaces with specular weight 0 trace none.

What to look at: render `/Render/roulette` (the default) and `/Render/everySample`. After 64 samples they match within noise. In the first few samples the polished floor's reflections near the camera show faint 8x8 blocks.

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_02_reflection_roulette\scene.usda`, pick a settings prim, or flip Ray-Traced Reflection Roulette in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/roulette scene.usda`.

GTX 1080 Ti, 1920x1080 final quality, ms per sample (on / off):
- crowd_9k: 146 / 249
- Kitchen with dome: 57 / 73
- Kitchen: 34 / 49

RMSE at 64 samples against 1024 samples with the old code:
- crowd and emissive: unchanged
- Kitchen with dome: 0.00219 against 0.00195
- this scene: 0.00080 against 0.00023 (its floor's sharp reflection converges very fast without the roulette)
