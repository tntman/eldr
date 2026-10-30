# MaterialX nodes and surfaces

<!-- summary -->
MaterialX compositing nodes, glTF PBR, Disney and unlit surfaces, and Karma's Ramp Const, Hex Tiled Texture and Voronoi nodes.

- **Compare:** open `showcase/2026-10-29_16_materialx_nodes/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `graphsOn` vs `graphsOff` (`eldr:materialGraphs` off), `proceduralsOff` (`eldr:proceduralTextures` off).
- **Look at:** The top row of blend modes and the bottom row of Karma nodes go flat with graphs off; the middle row of surfaces stays.
<!-- summary -->

A grid of the nodes and surfaces Eldr reads since lane 38 (`scene.usda` is generated; materials are inline).

- Top row, unlit (emission only): the compositing nodes `plus`, `minus`, `difference`, `burn`, `dodge`, `screen`, `overlay`, `over` (color4) of a left-right ramp (fg: orange to blue) over a bottom-top ramp (bg: dark to light). Before, each showed its bg ramp only (the node was bypassed). Burn goes black where the fg is dark blue; dodge blows out towards the top.
- Middle row: glTF PBR gold (metal), glTF with clearcoat and sheen, glTF with `alpha` 0.3 in BLEND mode (translucent), Disney principled red plastic and coated metal, `surface_unlit` (orange glow, no highlight). Before, all six rendered the default grey.
- Bottom row, Karma context (`outputs:kma:surface`, as Houdini's Material Library writes it): Karma Ramp Const with five bspline colours along u, a float Ramp Const mixing monotone cubic, constant and linear segments along u, Karma Hex Tiled Texture of `tests/scenes/textures/quadrants.png` (rotated, offset hexagonal tiles, no visible repeat grid), Voronoi Noise 3D's F1 distance (unlit), and Voronoi flakes as a bump under a clear coat (Houdini's Car Paint).

A/B: `/Render/graphsOn` (default) vs `/Render/graphsOff` (`eldr:materialGraphs` off: every graph-driven input keeps its constant, so the top and bottom rows go flat) and `/Render/proceduralsOff` (`eldr:proceduralTextures` off: Voronoi reads 0; hex tiling still runs).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_16_materialx_nodes\scene.usda` (Storm doesn't read the Karma-context bottom row)
- Houdini: Sublayer LOP on the file, Eldr as the viewport renderer; compare with Karma on the same stage
- husk: `husk -R Eldr -s /Render/graphsOn showcase/2026-10-29_16_materialx_nodes/scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam showcase\2026-10-29_16_materialx_nodes\scene.usda out.png`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
