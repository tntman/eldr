# Curves and points

Beyond showcase 2026-10-28_06: every basis of `tests/scenes/curves.usda` (polyline with vertex widths, periodic square, ribbon with normals, bspline, catmullRom, pinned bspline, bezier, periodic loop, points with vertex widths and colours) plus a patch of 600 hair curves without a material (bottom left: Kajiya-Kay hair shading), under a sun and a soft dome. The camera is pulled back to fit the patch.

## What to look at

- `/Render/curves` (the default): 6-sided tubes, icosahedron points, shiny hair.
- `/Render/ribbons` (`eldr:curveRibbons`): camera-facing ribbons (2 triangles a step instead of 12): flat-shaded strokes.
- `/Render/sprites` (`eldr:pointSprites`): points as camera-facing discs.
- `/Render/hairOff` (`eldr:hairShading` off): the fur as plain diffuse tubes, no hair highlight.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_19_curves_points\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/curves scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
