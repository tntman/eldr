# Denoiser (OIDN) and tile-based adaptive sampling

`tests/scenes/emissive.usda` (the Cornell box lit by its glowing panel and bulb) with GI and final gather, so few samples are noisy. Every prim here ticks the final-render settings (as `eldr_final` does) with its own `eldr:filmSamples`; `eldr_final` itself is the same at 256 samples with the denoiser on.

- `/Render/raw16` vs `/Render/denoise16`: 16 film samples, `eldr:denoise` off and on. Once the film converged, Intel Open Image Denoise cleans it on the CPU (about 1.3 s at 1080p, 0.3 s at this size), guided by albedo and normal. Look at the walls and ceiling: the raw image is grainy, the denoised one smooth with sharp edges. `/Render/raw64` for comparison: 16 denoised samples are cleaner than 64 raw ones.
- `/Render/denoise16` also writes `denoise16.exr` with the denoised beauty (`C`) and the raw one (`raw`, source `eldr:beautyRaw`).
- `/Render/karmaImageFilter16`: no `eldr:denoise`, but Karma's image filter string with `"denoise"` (as the Karma LOP writes it) turns the denoiser on too.
- `/Render/adaptiveTiles256` vs `/Render/adaptiveFull256`: adaptive sampling at threshold 0.01 with `eldr:adaptiveTiles` on and off. Same image; with tiles on, tiles that stopped skip shading and rays, so the render is faster (render stats: `rayTracing` and `shading` pass times drop).

Needs Eldr built with OIDN (CMake finds `OIDN_ROOT`, default C:/dev/oidn-2.5.1; Houdini's own OIDN is used inside Houdini). Without it `eldr:denoise` prints one warning and renders raw.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_07_oidn_adaptive_tiles\scene.usda` (pick a settings prim: right-click, set as active)
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`, or the viewport's Karma denoise toggle
- husk: `husk -R Eldr -s /Render/denoise16 showcase/2026-10-29_07_oidn_adaptive_tiles/scene.usda`

Settings prims: `/Render/raw16`, `/Render/denoise16`, `/Render/karmaImageFilter16`, `/Render/raw64`, `/Render/adaptiveTiles256`, `/Render/adaptiveFull256`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
