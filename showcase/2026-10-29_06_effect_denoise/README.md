# Per-effect denoise

`tests/scenes/emissive.usda` (Cornell box lit by a glowing panel and a bulb, chrome ball) with the ray-traced recipe: GI final gather, traced shadows and traced reflections. Each effect's signal is filtered before it is shaded: the gathered light by two edge-aware a-trous passes (`eldr:finalGatherDenoise`), the shadow visibility over 3 pixels in the receiver's plane (`eldr:rtShadowDenoise`), rough reflections over up to 12 pixels (`eldr:rtReflectionDenoise`). The filters fade out over the film's first `eldr:effectDenoiseSamples` (16) samples, so few samples come out clean and the converged image stays sharp.

- `/Render/raw4` vs `/Render/denoise4`: 4 film samples. Look at the floor's soft shadow under the box and the walls' indirect light.
- `/Render/raw16` vs `/Render/denoise16`: 16 samples.
- `/Render/raw64` vs `/Render/denoise64`: 64 samples; nearly the same, by design (the filters are off after sample 16).
- `/Render/noFade64`: `eldr:effectDenoiseSamples` 0, the filters run on every sample: smoother, but the shadow edges and corners stay slightly blurred however long it renders.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_06_effect_denoise\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_06_effect_denoise/scene.usda`

Settings prims: `/Render/raw4`, `/Render/denoise4`, `/Render/raw16`, `/Render/denoise16`, `/Render/raw64`, `/Render/denoise64`, `/Render/noFade64`.
