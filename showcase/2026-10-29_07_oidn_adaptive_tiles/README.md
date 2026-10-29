# Denoiser (OIDN) and tile-based adaptive sampling

`tests/scenes/emissive.usda` (the Cornell box lit by its glowing panel and bulb) with GI and final gather, so few samples are noisy.

- `/Render/raw16` vs `/Render/denoise16`: 16 film samples, `eldr:denoise` off and on. Once the film converged, Intel Open Image Denoise cleans it on the CPU (about 1.3 s at 1080p, 0.3 s at this size), guided by albedo and normal. Look at the walls and ceiling: the raw image is grainy, the denoised one smooth with sharp edges. `/Render/raw64` for comparison: 16 denoised samples are cleaner than 64 raw ones.
- `/Render/denoise16` also writes `denoise16.exr` with the denoised beauty (`C`) and the raw one (`raw`, source `eldr:beautyRaw`).
- `/Render/karmaImageFilter16`: no `eldr:denoise`, but Karma's image filter string with `"denoise"` (as the Karma LOP writes it) turns the denoiser on too.
- `/Render/adaptiveTiles256` vs `/Render/adaptiveFull256`: adaptive sampling at threshold 0.01 with `eldr:adaptiveTiles` on and off. Same image; with tiles on, tiles that stopped skip shading and rays, so the render is faster (render stats: `rayTracing` and `shading` pass times drop).

Needs Eldr built with OIDN (CMake finds `OIDN_ROOT`, default C:/dev/oidn-2.5.1; Houdini's own OIDN is used inside Houdini). Without it `eldr:denoise` prints one warning and renders raw.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_07_oidn_adaptive_tiles\scene.usda` (pick a settings prim: right-click, set as active)
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`, or the viewport's Karma denoise toggle
- husk: `husk -R Eldr -s /Render/denoise16 showcase/2026-10-29_07_oidn_adaptive_tiles/scene.usda`

Settings prims: `/Render/raw16`, `/Render/denoise16`, `/Render/karmaImageFilter16`, `/Render/raw64`, `/Render/adaptiveTiles256`, `/Render/adaptiveFull256`.
