# Karma default material

<!-- summary -->
Prims without a material shade as in Karma: 0.4 x `displayColor` (white without one) with a faint untinted highlight, and `displayOpacity` as opacity (`eldr:defaultMaterialKarma`).

- **Compare:** open `showcase/2026-10-31_02_karma_default_material/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `karmaDefault` vs `lambertDefault`.
- **Look at:** The ground, posts and balls about as bright as Karma's; the blue ball on the right half see-through.
<!-- summary -->

`tests/scenes/sunangle.usda` (a ground of `displayColor` 0.7, two posts, a beam and a ball without a `displayColor`, under a sun and a blue sky) plus a red ball and a blue ball with `displayOpacity` 0.5. None has a material.

## What to look at

- `/Render/karmaDefault` (the default): the ground is about 0.4 of its `displayColor`; the posts, beam and white ball read white at the same 0.4. The blue ball shows the ground through it. Karma renders the same.
- `/Render/lambertDefault` (`eldr:defaultMaterialKarma` off): `displayColor` at full strength (as Storm), about 2.3x as bright in a linear render (auto exposure evens much of it out), posts and ball a mid grey, both balls opaque.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-31_02_karma_default_material\scene.usda`, then pick a settings prim (right-click, set as active), or flip the setting in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/karmaDefault scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
