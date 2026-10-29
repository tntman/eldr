# Per-component quality multipliers

emissive.usda (a Cornell box lit by a glowing panel and a bulb, plus a mirror sphere) in final quality with GI, final gather, traced shadows and reflections, at 32 film samples. Each settings prim spends rays on one component, the way Karma's quality controls do, instead of raising the film samples for everything.

- `/Render/default`: every quality at 1.
- `/Render/diffuse2`: `eldr:diffuseQuality` 2. Gather and GI probes trace twice the rays, so the walls and the floor are cleaner.
- `/Render/diffuse2LightHalf`: diffuse 2, `eldr:lightQuality` and `eldr:reflectQuality` 0.5. Shadows and reflections are traced on every other sample, and those samples keep the last image.
- `/Render/karmaNames`: the same as the previous prim, set with Karma's names (`karma:object:diffusequality`, `karma:global:light_sampling_quality`, `karma:object:reflectquality`). usdrecord renders it identical to `diffuse2LightHalf`.
- `/Render/allHalf`: every quality at 0.5. It is faster and noisier.

Measured on a GTX 1080 Ti (compute ray tracing), 960x540, 32 samples, effect denoisers off. RMSE is against a 1024-sample render at quality 1:

| Setting | 0.5 | 1 | 2 |
|---|---|---|---|
| diffuse: ms/sample, RMSE | 15.2, 0.0154 | 20.1, 0.0116 | 35.2, 0.0080 |
| light: ms/sample, RMSE | 18.3, 0.0116 | 20.1, 0.0116 | 23.0, 0.0116 |
| reflect: ms/sample, RMSE | 19.6, 0.0116 | 20.1, 0.0116 | 20.4, 0.0116 |

On this scene the final gather is the noise. Diffuse 2 matches 64 samples at quality 1 (0.0079) in 1.20 s instead of 1.32 s. Light and reflection quality change the time but not the noise here, because the small emitters and the mirror sphere hardly add any noise. They pay off with large soft lights and rough reflections. At 512 samples with every quality at 0.5, the RMSE is 0.0036 (0.0021 at quality 1). The fractions converge, at about the rate of half the rays.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_12_quality_multipliers\scene.usda`, then pick a settings prim
- Houdini: Sublayer LOP on the file with Eldr as the renderer. Karma Render Settings' Diffuse, Reflection, SSS and Volume Quality and Light Sampling Quality are read.
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_12_quality_multipliers/scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -w 960 -rs /Render/<name> <scene.usda> out.png`
