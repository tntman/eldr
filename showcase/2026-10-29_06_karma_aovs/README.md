# Karma's AOVs and light groups

<!-- summary -->
The render vars a Karma Render Settings LOP writes (light groups, diffuse and glossy splits, transmission, subsurface, coat, volume, shadow mattes) come out of Eldr by the same names.

- **Compare:** open `showcase/2026-10-29_06_karma_aovs/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `default` vs `noHoldouts` (`eldr:holdouts` off).
- **Look at:** The EXR's layers: `beauty_ceiling` and `beauty_lamp` (one light each), `glossytransmission`, `sss`, `coat`, `combinedvolume`, `holdout_shadow`.
<!-- summary -->

The Cornell box with a product `aovs` listing the render vars a Karma Render Settings LOP writes, by Karma's names and light path expressions: the beauty, diffuse and glossy (direct, indirect), `glossytransmission` (`C<TG>.*L`), `sss` (`C<TD>.*L`), `coat` (`C<...'coat'>.*L`), `combinedvolume` (`CV.*L`), emission, `shadow` (`shadow;C.*[LO]`), `holdout_shadow` (`holdouts;shadow;CDL`) and two light groups, `beauty_ceiling` (`C.*<L.'ceiling'>`) and `beauty_lamp` (`C.*<L.'lamp'>`). The ceiling light and a warm lamp carry Karma's LPE tag (`inputs:karma:light:lpetag`; Eldr's `inputs:eldr:light:lpetag` would win).

## What to look at

- `beauty_ceiling` and `beauty_lamp`: each light's direct light alone. Groups + `indirectdiffuse` + `indirectglossyreflection` + `glossytransmission` + `combinedemission` + `combinedvolume` rebuild the beauty (the subsurface ball aside: its indirect light is in `sss`).
- `glossytransmission`: the glass ball's refraction, no longer in `indirectglossyreflection`.
- `sss`: the subsurface ball on the box; its light is out of the diffuse vars.
- `coat`: the blue ball's clear coat, also counted in glossy (as Karma).
- `combinedvolume`: the small cloud; what is behind it is dimmed in every split, so the splits still add up.
- `holdout_shadow`: the block on the floor at the front right is a holdout (`karma:object:holdoutmode`): cut out of the beauty (zero alpha) and no light in any var, its shadows here for comp.
- `/Render/noHoldouts` (`eldr:holdouts` off): the block is drawn, its shadows move to `shadow`.

## Open

- husk: `husk -R Eldr -s /Render/default showcase/2026-10-29_06_karma_aovs/scene.usda` writes `lightGroups.exr`; render the same with `-R BRAY_HdKarma` to compare layer by layer.
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; the Render Outputs menu lists the vars.
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_06_karma_aovs\scene.usda`, AOV menu, or `usdrecord --renderSettingsPrimPath /Render/default`.

Settings prims: `/Render/default`, `/Render/noHoldouts`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
