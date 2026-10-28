# GI and final gather

`tests/scenes/emissive.usda` (the Cornell box lit by the glowing panel and bulb; `cornell.usda` is the rect-light version) with the bounced-light settings. Final quality renders over film samples, so wait for the image to converge.

- `/Render/probesOnly`: `eldr:gi` with `eldr:finalGather` 0, DDGI probes only (soft, some leaking at walls).
- `/Render/finalGather`: `eldr:finalGather` 1, a ray per pixel and sample shaded at its hit.
- `/Render/finalGatherNoRelocation`: `eldr:giRelocation` off; probes near walls are only switched off. With final gather the first bounce is traced, so the difference is tiny; look at `/Render/probesOnly` with it toggled.
- `/Render/finalGatherDistance0` vs `...05`: `eldr:finalGatherDistance` 0 (rays go until they hit) vs 0.5 (short rays, probes beyond).
- `/Render/ssgi`: screen-space GI only; `/Render/giPlusSsgi`: both.

- `/Render/denoise0` vs `/Render/denoise1`: `eldr:finalGatherDenoise` 0 to 1. It filters only the indirect light, so edges and textures stay sharp.
- `/Render/rays1` vs `/Render/rays4`: `eldr:finalGatherRays` 1 or 4 per pixel and sample. 4 rays cost about 4x the gather for little gain once denoised.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_04_gi_final_gather\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_04_gi_final_gather/scene.usda`

Settings prims: `/Render/probesOnly`, `/Render/finalGather`, `/Render/finalGatherNoRelocation`, `/Render/finalGatherDistance0`, `/Render/finalGatherDistance05`, `/Render/denoise0`, `/Render/denoise1`, `/Render/rays1`, `/Render/rays4`, `/Render/ssgi`, `/Render/giPlusSsgi`.
