# Transparency order

<!-- summary -->
Glass, translucent meshes and smoke are composited in one depth order, so what is behind a pane is seen through it.

- **Compare:** open `showcase/2026-10-29_09_transparency_order/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `default`, `sorted` (`eldr:oit` off), `oneLayer` (`eldr:refractionLayers` 1), `volumesOverGlass` (`eldr:volumeUnderTransparency` off), `final` (traced refraction).
- **Look at:** The red quad behind blue glass (purple-blue through the glass) and the smoke balls dimmed once per pane in front of them.
<!-- summary -->

tests/scenes/transparency_order.usda. Top row: a half-opaque red quad (UsdPreviewSurface opacity 0.5) in front of blue thin glass, and to its right the pair the other way round. Bottom right: dense smoke balls with green-tinted panes. The left ball is behind two overlapping panes, and the right ball is between a pane in front and a pane behind.

What to look at:
- Red behind blue glass (right pair) is seen through the glass, so it is purple-blue.
- The left ball is dimmed once where one pane covers it and twice where both do. The right ball is dimmed by its front pane only.

- Bottom left (this showcase only): a cyan quad over a yellow one, both opacity 0.7, nearer than all the glass. `/Render/sorted` blends them back to front; the default's weighted blended OIT averages them by depth weight, so the yellow behind shows more in the overlap. `eldr:oit` only matters where two translucent meshes overlap between the same pair of refracting ones: the red quads above each sit alone in their run, and one OIT layer resolves exactly like a sorted blend.

Settings prims: `/Render/default`, `/Render/sorted` (`eldr:oit` off), `/Render/oneLayer` (`eldr:refractionLayers` 1: one copy, one peeled layer), `/Render/volumesOverGlass` (`eldr:volumeUnderTransparency` off), `/Render/final` (the final-render settings, with traced refraction; `default` has screen-space refraction, so pick `final` to see traced refraction, or `eldr_final`); standard: `eldr_default`, `eldr_final`, `karma`.

Against Karma (husk, 256 spp, 320x180, tests/karma_refs.py), luminance Eldr / Karma per region with traced refraction (`final`): red in front of glass 1.04, red alone 1.03, glass in front of red 1.00, smoke behind two panes 0.86, behind one pane 1.04, between panes 0.94. Whole image: lum 1.03, mald 0.050. With screen-space refraction (`eldr:rtRefraction` 0) the smoke regions are 1.14 / 1.16 / 0.94. With `eldr:volumeUnderTransparency` off they are 1.51 / 1.34 / 1.27.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_09_transparency_order\scene.usda`, then pick a settings prim
- Houdini: a Sublayer LOP on the file, with Eldr as the viewport renderer
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_09_transparency_order/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
