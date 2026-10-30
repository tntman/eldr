# Planar reflection blur

<!-- summary -->
A rough planar mirror blurs its reflection by its roughness (`eldr:planarBlur`).

- **Compare:** open `showcase/2026-10-29_03_planar_reflection_blur/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `blurred` vs `sharp`.
- **Look at:** The columns reflected in the wet floor: soft with the blur, mirror-sharp without.
<!-- summary -->

`tests/scenes/reflections.usda` with the floor at roughness 0.4 and flagged as a planar mirror (`primvars:eldr:planarReflection`).

- `/Render/sharp` vs `/Render/blurred`: `eldr:planarBlur` 0 vs 1 (12-tap disc, radius about roughness squared).
- Both set `eldr:rtReflections` 0: with traced reflections on (as in `eldr_final`) they replace the planar mirror, so the blur has nothing to change.

Not done: refraction in planar mirrors, contact-hardening blur.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_03_planar_reflection_blur\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_03_planar_reflection_blur/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
