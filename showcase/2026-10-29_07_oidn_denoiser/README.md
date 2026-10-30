# Intel Open Image Denoise

<!-- summary -->
The converged film is denoised by Intel Open Image Denoise, guided by albedo and normal (`eldr:denoise`, or Karma's image filter).

- **Compare:** open `showcase/2026-10-29_07_oidn_denoiser/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `raw16` vs `denoise16`, `karmaImageFilter16` (Karma's denoiser string), `raw64`.
- **Look at:** The walls and ceiling: grainy raw, smooth with sharp edges denoised; 16 denoised samples beat 64 raw ones.
<!-- summary -->

`tests/scenes/emissive.usda` (the Cornell box lit by its glowing panel and bulb) with GI and final gather, so few samples are noisy. Every prim ticks the final-render settings with its own `eldr:filmSamples`.

- `/Render/raw16` vs `/Render/denoise16`: 16 film samples, `eldr:denoise` off and on (about 1.3 s at 1080p on the CPU).
- `/Render/denoise16` also writes `denoise16.exr` with the denoised beauty (`C`) and the raw one (`raw`, source `eldr:beautyRaw`).
- `/Render/karmaImageFilter16`: no `eldr:denoise`, but Karma's image filter string with `"denoise"` turns the denoiser on too.
- `/Render/raw64`: 64 raw samples, for comparison.

Needs Eldr built with OIDN (CMake finds `OIDN_ROOT`, default C:/dev/oidn-2.5.1; inside Houdini, Houdini's own OIDN). Without it `eldr:denoise` prints one warning and renders raw.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_07_oidn_denoiser\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_07_oidn_denoiser/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
