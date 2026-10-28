# Houdini Render Vars

The Cornell box with a product on the settings prims: `beauty` with the vars `C` (`C.*[LO]`), `rendervar1` (`CDA`), `N` (`ray:hitN`) and `depth` (`ray:hitPz`).

## What to look at

- Houdini: Sublayer LOP on the file, Eldr viewport. In the viewport's Render Outputs menu pick `rendervar1`. It should show an image, not black. N and depth too. Without a beauty var in the product the menu has no `color` entry (Karma does the same).
- The RGB / R / G / B channel buttons in the viewport are a known limitation: they do not switch channels for Eldr.
- husk: `/Render/default` writes all four vars to `renderVars.exr`. `/Render/viewport` sets the `houdini:viewport` flag (interactive, as the Solaris viewport does).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_05_houdini_render_vars\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_05_houdini_render_vars/scene.usda`

Settings prims: `/Render/default`, `/Render/viewport`.
