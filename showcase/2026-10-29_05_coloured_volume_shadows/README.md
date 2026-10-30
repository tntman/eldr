# Coloured volume shadows and calibrated multiple scattering

volume.usda's grey smoke cloud with the fire replaced by orange-brown soot. The soot is an absorbing MaterialX volume with absorption (0.15, 0.6, 1.2) per unit of density, so it lets red through and stops blue.

- `/Render/maps`: the soot's shadow on the floor is orange (the sun's tint maps, per channel).
- `/Render/traced`: `eldr:rtShadows`. Traced shadow rays march the soot per channel and give the same orange shadow.
- `/Render/halfResolution`: `eldr:volumeResolution` 2. The soot's own see-through colour stays orange-brown.
- `/Render/shadowsOff`: `eldr:volumeShadows` off, for comparison.
- `/Render/volumeLimit1`, `/Render/volumeLimit16`: `karma:object:volumelimit` on the settings prim. The grey cloud gets brighter with more bounces, fitted to Karma's gain over single scattering (within 7% at limits 1, 4 and 16 on volume.usda and fog.usda clouds).

Against Karma (husk -R BRAY_HdKarma, the same soot over the floor), the floor-shadow transmittance per channel is Karma 0.63 / 0.24 / 0.11 and Eldr 0.63 / 0.24 / 0.12 (maps and traced).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_05_coloured_volume_shadows\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_05_coloured_volume_shadows/scene.usda`

Settings prims: `/Render/maps`, `/Render/traced`, `/Render/halfResolution`, `/Render/shadowsOff`, `/Render/volumeLimit1`, `/Render/volumeLimit4`, `/Render/volumeLimit16`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
