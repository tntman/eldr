# Per-object curve and point styles

<!-- summary -->
Each curve or points prim can set its width scale, tubes or ribbons, and sphere points, with Eldr's names winning over Karma's.

- **Compare:** open `showcase/2026-10-28_10_curve_styles/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `styles` (the per-object values) vs `ribbons` (`eldr:curveRibbons`: every curve without its own style becomes a ribbon); `eldr_default` vs `karma` (Karma reads only the karma names).
- **Look at:** The bezier curve (eldr width 0.4 beats karma 3: thin), the bspline (karma 2.5 only: thick), the catmullRom (eldr tubes beat karma ribbons, so it stays a tube under `ribbons`) and the sphere points.
<!-- summary -->

`tests/scenes/curves.usda` with overrides:

- `bezier`: `primvars:eldr:object:width_scale` 0.4 vs karma 3: thin in Eldr, thick in Karma.
- `bspline`: karma width_scale 2.5 only: Eldr reads it too.
- `catmullRom`: eldr curve_style 1 (tubes) vs karma 0 (ribbons).
- `points`: eldr point_style 1 (spheres).

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_10_curve_styles\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-28_10_curve_styles/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
