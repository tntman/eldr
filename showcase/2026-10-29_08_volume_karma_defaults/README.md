# Volumes without a material, as Karma

`tests/scenes/volume.usda`'s clouds have no material. Eldr gives them Karma's medium: scattering 1
per unit of density, no absorption, isotropic phase. The dome lights them from its upper half only,
because Karma's single scattering doesn't see the dome below the ground. Measured against Karma CPU at
volume limit 0, the sun-lit clouds come out within 1%.

Render settings prims:
- `/Render/newPhase`: the default (isotropic).
- `/Render/oldPhase`: `eldr:volumeAnisotropy` 0.2, a forward-scattering phase.
- `/Render/volumeLimit16`: `karma:object:volumelimit` 16, multiple scattering.

Open it:
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_08_volume_karma_defaults\scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -rs /Render/newPhase scene.usda out.png`
- husk: `husk -R HdVkRendererPlugin -s /Render/newPhase scene.usda -o out.exr`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
