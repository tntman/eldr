# Render vars in the Houdini viewport

<!-- summary -->
A Render Product's vars (beauty, CDA, N, depth) show in Houdini's Render Outputs menu and husk writes them to one EXR.

- **Compare:** open `showcase/2026-10-28_05_houdini_render_vars/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `default` (writes `renderVars.exr`) vs `viewport` (`houdini:viewport` flag).
- **Look at:** In Houdini's viewport pick `rendervar1`, N or depth in the Render Outputs menu: each shows an image, not black.
<!-- summary -->

The Cornell box with a product on the settings prims: `beauty` with the vars `C` (`C.*[LO]`), `rendervar1` (`CDA`), `N` (`ray:hitN`) and `depth` (`ray:hitPz`).

## What to look at

- Houdini: Sublayer LOP on the file, Eldr viewport. In the viewport's Render Outputs menu pick `rendervar1`. It should show an image, not black. N and depth too. Without a beauty var in the product the menu has no `color` entry (Karma does the same).
- The RGB / R / G / B channel buttons in the viewport are a known limitation: they do not switch channels for Eldr.
- husk: `/Render/default` writes all four vars to `renderVars.exr`. `/Render/viewport` sets the `houdini:viewport` flag (interactive, as the Solaris viewport does).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_05_houdini_render_vars\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_05_houdini_render_vars/scene.usda`

Settings prims: `/Render/default`, `/Render/viewport`; standard: `eldr_default`, `eldr_final`, `karma`.

Note: Houdini's Render Outputs menu lists render vars that carry Houdini's own authoring (`HuskRenderVarAPI`, `driver:parameters:aov:husk:name` / `format`), as the Render Var LOP writes them; plain UsdRender vars render (husk, usdrecord) but may not show in the menu. This scene authors both, and sets `renderSettingsPrimPath`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
