# Per-effect denoise

`tests/scenes/emissive.usda` (Cornell box lit by a glowing panel and a bulb, chrome ball) with the ray-traced recipe: GI final gather, traced shadows and traced reflections. Each effect's signal is filtered before it is shaded: the gathered light by two edge-aware a-trous passes (`eldr:finalGatherDenoise`), the shadow visibility over its penumbra (up to 3 pixels) in the receiver's plane (`eldr:rtShadowDenoise`), rough reflections over up to 12 pixels (`eldr:rtReflectionDenoise`). The filters fade out over the film's first `eldr:effectDenoiseSamples` (64) samples, so few samples come out clean and the converged image stays sharp.

- `/Render/raw4` vs `/Render/denoise4`: 4 film samples. Look at the floor's soft shadow under the box and the walls' indirect light.
- `/Render/raw16` vs `/Render/denoise16`: 16 samples.
- `/Render/raw64` vs `/Render/denoise64`: 64 samples; the denoised one is still cleaner (RMSE vs 1024 samples 0.008 against 0.012); from sample 64 on the filters are off.
- `/Render/noFade64`: `eldr:effectDenoiseSamples` 0, the filters run on every sample: smoother, but the shadow edges and corners stay slightly blurred however long it renders.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_11_effect_denoise\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_11_effect_denoise/scene.usda`

Settings prims: `/Render/raw4`, `/Render/denoise4`, `/Render/raw16`, `/Render/denoise16`, `/Render/raw64`, `/Render/denoise64`, `/Render/noFade64`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
