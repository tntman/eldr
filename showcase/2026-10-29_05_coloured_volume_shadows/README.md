# Coloured volume shadows and calibrated multiple scattering

volume.usda's grey smoke cloud with the fire replaced by orange-brown soot. The soot is an absorbing MaterialX volume with absorption (0.15, 0.6, 1.2) per unit of density, so it lets red through and stops blue.

- `/Render/maps`: the soot's shadow on the floor is orange (the sun's tint maps, per channel). Before this change it was grey, the mean of the three channels.
- `/Render/traced`: `eldr:rtShadows`. Traced shadow rays march the soot per channel and give the same orange shadow.
- `/Render/halfResolution`: `eldr:volumeResolution` 2. The soot's own see-through colour stays orange-brown; before, it was only the mean transmittance.
- `/Render/shadowsOff`: `eldr:volumeShadows` off, for comparison.
- `/Render/volumeLimit1`, `/Render/volumeLimit16`: `karma:object:volumelimit` on the settings prim. The grey cloud gets brighter with more bounces, fitted to Karma's gain over single scattering (within 7% at limits 1, 4 and 16 on volume.usda and fog.usda clouds).

Against Karma (husk -R BRAY_HdKarma, the same soot over the floor), the floor-shadow transmittance per channel is Karma 0.63 / 0.24 / 0.11 and Eldr 0.63 / 0.24 / 0.12 (maps and traced). Grey shadows were 0.22 in every channel.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_05_coloured_volume_shadows\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_05_coloured_volume_shadows/scene.usda`

Settings prims: `/Render/maps`, `/Render/traced`, `/Render/halfResolution`, `/Render/shadowsOff`, `/Render/volumeLimit1`, `/Render/volumeLimit4`, `/Render/volumeLimit16`.
