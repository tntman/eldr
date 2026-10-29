# Karma's AOVs: light groups, transmission, subsurface, coat, volume, shadow mattes

The Cornell box with a product `aovs` listing the render vars a Karma Render Settings LOP writes, by Karma's names and light path expressions: the beauty, diffuse and glossy (direct, indirect), `glossytransmission` (`C<TG>.*L`), `sss` (`C<TD>.*L`), `coat` (`C<...'coat'>.*L`), `combinedvolume` (`CV.*L`), emission, `shadow` (`shadow;C.*[LO]`), `holdout_shadow` (`holdouts;shadow;CDL`) and two light groups, `beauty_ceiling` (`C.*<L.'ceiling'>`) and `beauty_lamp` (`C.*<L.'lamp'>`). The ceiling light and a warm lamp carry Karma's LPE tag (`inputs:karma:light:lpetag`; Eldr's `inputs:eldr:light:lpetag` would win).

## What to look at

- `beauty_ceiling` and `beauty_lamp`: each light's direct light alone. Groups + `indirectdiffuse` + `indirectglossyreflection` + `glossytransmission` + `combinedemission` + `combinedvolume` rebuild the beauty (the subsurface ball aside: its indirect light is in `sss`).
- `glossytransmission`: the glass ball's refraction, no longer in `indirectglossyreflection`.
- `sss`: the subsurface ball on the box; its light is out of the diffuse vars.
- `coat`: the blue ball's clear coat, also counted in glossy (as Karma).
- `combinedvolume`: the small cloud; what is behind it is dimmed in every split, so the splits still add up.
- `holdout_shadow`: the black block on the floor is a holdout (`karma:object:holdoutmode`): no light in any var, its shadows here for comp.
- `/Render/noHoldouts` (`eldr:holdouts` off): the block is drawn, its shadows move to `shadow`.

## Open

- husk: `husk -R Eldr -s /Render/default showcase/2026-10-29_01_light_groups/scene.usda` writes `lightGroups.exr`; render the same with `-R BRAY_HdKarma` to compare layer by layer.
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; the Render Outputs menu lists the vars.
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_01_light_groups\scene.usda`, AOV menu, or `usdrecord --renderSettingsPrimPath /Render/default`.

Settings prims: `/Render/default`, `/Render/noHoldouts`.
