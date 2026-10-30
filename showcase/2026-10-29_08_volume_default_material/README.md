# Volumes without a material

<!-- summary -->
A volume with no material scatters as Karma's default medium: white, no absorption, isotropic, the dome lighting it from its upper half.

- **Compare:** open `showcase/2026-10-29_08_volume_default_material/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `newPhase` (the default) vs `oldPhase` (`eldr:volumeAnisotropy` 0.2).
- **Look at:** The clouds' brightness and the sun-side rim: within 1% of Karma at volume limit 0.
<!-- summary -->

`tests/scenes/volume.usda`'s clouds have no material. Eldr now gives them Karma's medium: scattering 1
per unit of density, no absorption, isotropic phase. The dome lights them from its upper half only,
because Karma's single scattering doesn't see the dome below the ground. Measured against Karma CPU at
volume limit 0, the sun-lit clouds come out within 1% (they were 0.56x before).

Render settings prims:
- `/Render/newPhase`: the new default.
- `/Render/oldPhase`: `eldr:volumeAnisotropy` 0.2, the old forward phase.
- Multiple scattering (Karma's Volume Limit) has its own showcase, `2026-10-29_26_volume_multiple_scattering`.

Open it:
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_08_volume_default_material\scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -rs /Render/newPhase scene.usda out.png`
- husk: `husk -R HdVkRendererPlugin -s /Render/newPhase scene.usda -o out.exr`

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_08_volume_default_material\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_08_volume_default_material/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
