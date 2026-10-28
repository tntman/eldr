# GI and final gather

`tests/scenes/emissive.usda` (the Cornell box lit by the glowing panel and bulb; `cornell.usda` is the rect-light version) with the bounced-light settings. Final quality renders over film samples, so wait for the image to converge.

- `/Render/probesOnly`: `eldr:gi` with `eldr:finalGather` 0, DDGI probes only (soft, some leaking at walls).
- `/Render/finalGather`: `eldr:finalGather` 1, a ray per pixel and sample shaded at its hit.
- `/Render/finalGatherNoRelocation`: `eldr:giRelocation` off; probes near walls are only switched off.
- `/Render/finalGatherDistance0` vs `...05`: `eldr:finalGatherDistance` 0 (rays go until they hit) vs 0.5 (short rays, probes beyond).
- `/Render/ssgi`: screen-space GI only; `/Render/giPlusSsgi`: both.

Coming (another lane is adding them): `eldr:finalGatherRays` (rays per pixel and sample) and `eldr:finalGatherDenoise`. They are not in the scene yet; add them to a settings prim once they exist.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_04_gi_final_gather\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_04_gi_final_gather/scene.usda`

Settings prims: `/Render/probesOnly`, `/Render/finalGather`, `/Render/finalGatherNoRelocation`, `/Render/finalGatherDistance0`, `/Render/finalGatherDistance05`, `/Render/ssgi`, `/Render/giPlusSsgi`.
