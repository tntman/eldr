# MaterialX coverage: compositing, glTF / Disney / unlit surfaces, Karma's VEX nodes

A grid of the nodes and surfaces Eldr reads since lane 38 (`scene.usda` is generated; materials are inline).

- Top row, unlit (emission only): the compositing nodes `plus`, `minus`, `difference`, `burn`, `dodge`, `screen`, `overlay`, `over` (color4) of a left-right ramp (fg: orange to blue) over a bottom-top ramp (bg: dark to light). Before, each showed its bg ramp only (the node was bypassed). Burn goes black where the fg is dark blue; dodge blows out towards the top.
- Middle row: glTF PBR gold (metal), glTF with clearcoat and sheen, glTF with `alpha` 0.3 in BLEND mode (translucent), Disney principled red plastic and coated metal, `surface_unlit` (orange glow, no highlight). Before, all six rendered the default grey.
- Bottom row, Karma context (`outputs:kma:surface`, as Houdini's Material Library writes it): Karma Ramp Const with five bspline colours along u, a float Ramp Const mixing monotone cubic, constant and linear segments along u, Karma Hex Tiled Texture of `tests/scenes/textures/quadrants.png` (rotated, offset hexagonal tiles, no visible repeat grid), Voronoi Noise 3D's F1 distance (unlit), and Voronoi flakes as a bump under a clear coat (Houdini's Car Paint).

A/B: `/Render/graphsOn` (default) vs `/Render/graphsOff` (`eldr:materialGraphs` off: every graph-driven input keeps its constant, so the top and bottom rows go flat) and `/Render/proceduralsOff` (`eldr:proceduralTextures` off: Voronoi reads 0; hex tiling still runs).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_09_mtlx_coverage\scene.usda` (Storm doesn't read the Karma-context bottom row)
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; compare with Karma on the same stage
- husk: `husk -R Eldr -s /Render/graphsOn showcase/2026-10-29_09_mtlx_coverage/scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam showcase\2026-10-29_09_mtlx_coverage\scene.usda out.png`
