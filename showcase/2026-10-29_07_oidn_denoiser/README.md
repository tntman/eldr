# Denoising

<!-- summary -->
The finished image is cleaned by Intel Open Image Denoise, guided by albedo and normal.
<!-- summary -->

A Cornell box with bounced light, rendered with few film samples so it is noisy.

- **raw16**: 16 samples, noisy.
- **denoise16**: 16 samples, denoised: smooth with sharp edges; also writes the raw beauty.
- **karmaImageFilter16**: 16 samples, denoised through Karma's image filter setting.
- **raw64**: 64 samples, no denoiser, for comparison.
- **eldr_default / eldr_final / karma**: Eldr with nothing set, Eldr's final settings, Karma to compare.

Needs Eldr built with Open Image Denoise; without it the image stays raw.

## Open

Houdini: `scene.hip`. usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_07_oidn_denoiser\scene.usda`, then pick a settings prim. husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_07_oidn_denoiser/scene.usda`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
