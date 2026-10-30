# Volumes without a material, as Karma

`tests/scenes/volume.usda`'s clouds have no material. Eldr now gives them Karma's medium: scattering 1
per unit of density, no absorption, isotropic phase. The dome lights them from its upper half only,
because Karma's single scattering doesn't see the dome below the ground. Measured against Karma CPU at
volume limit 0, the sun-lit clouds come out within 1% (they were 0.56x before).

Render settings prims:
- `/Render/karma`: the new default.
- `/Render/oldPhase`: `eldr:volumeAnisotropy` 0.2, the old forward phase.
- `/Render/volumeLimit16`: `karma:object:volumelimit` 16, multiple scattering.

Open it:
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_08_volume_karma_defaults\scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -rs /Render/karma scene.usda out.png`
- husk: `husk -R HdVkRendererPlugin -s /Render/karma scene.usda -o out.exr`
