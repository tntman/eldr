# GI and final gather

`tests/scenes/emissive.usda` (the Cornell box lit by the glowing panel and bulb; `cornell.usda` is the rect-light version) with the bounced-light settings. The A/B prims here (except `probesOnly` and `ssgi`) also set the final-render settings, so the image converges over film samples: wait for it.

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

Settings prims: `/Render/probesOnly`, `/Render/finalGather`, `/Render/finalGatherNoRelocation`, `/Render/finalGatherDistance0`, `/Render/finalGatherDistance05`, `/Render/denoise0`, `/Render/denoise1`, `/Render/rays1`, `/Render/rays4`, `/Render/ssgi`, `/Render/giPlusSsgi`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
