# Transparency order: glass, translucent meshes and smoke

tests/scenes/transparency_order.usda. Top row: a half-opaque red quad (UsdPreviewSurface opacity 0.5) in front of blue thin glass, and to its right the pair the other way round. Bottom right: dense smoke balls with green-tinted panes. The left ball is behind two overlapping panes, and the right ball is between a pane in front and a pane behind.

What to look at:
- Red behind blue glass (right pair) is seen through the glass, so it is purple-blue. Before this change the red was composited over every refracting mesh, so both pairs looked the same.
- The left ball is dimmed once where one pane covers it and twice where both do. The right ball is dimmed by its front pane only. Before, the smoke was split only at the nearest pane, so smoke between panes was also dimmed and refracted by the pane behind it. Under final quality (traced refraction), smoke behind glass disappeared entirely.

- Bottom left (this showcase only): a cyan quad over a yellow one, both opacity 0.7, no glass between them. `/Render/sorted` blends them back to front; the default's weighted blended OIT averages them, so the overlap is greener. `eldr:oit` only matters where two translucent meshes overlap between the same pair of refracting ones: the red quads above each sit alone in their run, and one OIT layer resolves exactly like a sorted blend.

Settings prims: `/Render/default`, `/Render/sorted` (`eldr:oit` off), `/Render/oneLayer` (`eldr:refractionLayers` 1: one copy, one peeled layer), `/Render/volumesOverGlass` (`eldr:volumeUnderTransparency` off), `/Render/final` (`eldr:finalQuality`, traced refraction). usdrecord and husk already render final quality (offline: the first frame starts a final film run), so `default` and `final` match there; `final` differs only in an interactive viewer (usdview, Houdini).

Against Karma (husk, 256 spp, 320x180, tests/karma_refs.py), luminance Eldr / Karma per region in final quality: red in front of glass 1.04, red alone 1.03, glass in front of red 1.00, smoke behind two panes 0.86, behind one pane 1.04, between panes 0.94. Whole image: lum 1.03, mald 0.050. With screen-space refraction (`eldr:rtRefraction` 0) the smoke regions are 1.14 / 1.16 / 0.94. With `eldr:volumeUnderTransparency` off they are 1.51 / 1.34 / 1.27.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_09_transparency_order\scene.usda`, then pick a settings prim
- Houdini: a Sublayer LOP on the file, with Eldr as the viewport renderer
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-29_09_transparency_order/scene.usda`
