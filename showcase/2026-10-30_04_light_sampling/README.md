# Traced light sampling

In the RT recipe, traced shadow rays cost the most where many lights shine on the scene. emissive.usda turns its panel and bulb into several lights. With `eldr:rtLightSampling` (on), each hit of a reflection, refraction or final gather ray traces one shadow ray. It goes to a light picked in proportion to its unshadowed light and is weighted 1/p. Before, every light got a ray, capped at 16. The per-pixel traced shadows skip lights that give the pixel nothing (out of range, outside the cone, or light-linked away). From the film's second sample, scenes with more than 4 traced lights trace each light per 8x8 block in 4/lights of the samples, weighted to match.

What to look at: render `/Render/sampled` (the defaults) and `/Render/everyLight` (light sampling and the gather roulette off). After 64 samples they match within noise. The first is about twice as fast per sample.

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_04_light_sampling\scene.usda`, then pick a settings prim, or flip Ray-Traced Light Sampling in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/sampled scene.usda`.

GTX 1080 Ti, 1920x1080, RT recipe, ms per sample (on / off): emissive 78 / 147, Kitchen with dome 158 / 195, crowd_9k 478 / 542. RMSE at 64 samples against 1024 samples with the old code: emissive 0.0074 / 0.0070, Kitchen with dome 0.0089 / 0.0088.

`eldr:finalGatherRoulette` (off) also halves the gather rays per sample (8x8 blocks in turn, weighted by 2). It saves 13-19% per sample but raises RMSE at 64 samples 1.4-1.5x, so it is off by default.
