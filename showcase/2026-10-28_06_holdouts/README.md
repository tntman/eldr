# Holdouts and phantom objects

<!-- summary -->
Objects set as holdouts are cut out of the image as mattes, and phantom objects are hidden from the camera but still cast shadows and show in reflections.

- **Compare:** open `showcase/2026-10-28_06_holdouts/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `holdoutScene` vs `holdoutsOff` (`eldr:holdouts`).
- **Look at:** The ball (a matte: alpha 0, its shadow kept), the box (eldr says no holdout, beating karma's 1: drawn) and the front object (phantom: unseen, its shadow still there).
<!-- summary -->

`tests/scenes/holdout.usda` with per-object overrides. Eldr's `primvars:eldr:object:*` names win over `primvars:karma:object:*`; both are set here with different values so you can see the winner. Houdini's Eldr tab on a Render Geometry Settings LOP authors the eldr names.

- `/box`: karma holdoutmode 1, eldr 0. Eldr wins: the box renders normally.
- `/ball`: only karma mode 2: a matte.
- `/front`: eldr `rendervisibility = "-primary"` (phantom), karma says `*`.

`/Render/holdoutScene` (`eldr:holdouts` on, the default) cuts the mattes to alpha 0; `/Render/holdoutsOff` draws them as usual. Karma reads the karma names, so its `/box` is a holdout.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_06_holdouts\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-28_06_holdouts/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
